/* splice.c - the otaman's L4 splicer: relay the public port across the fleet.
 *
 * Backends are DISCOVERED from the roster at 1 Hz, never configured. The
 * listener is watched only when a relay slot AND backend capacity exist:
 * a splicer without admission control is a queue amplifier. Single task,
 * no locks taken here - swarm_snapshot() does its own.
 * Every constant and law below was measured on the fleet. */
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lwip/ip4_addr.h"
#include "sdkconfig.h"
#include "swarm.h"
#include "swarm_identity.h"   /* swarm_self() - the solo backend wears our own id */
#include "serve.h"
#include "blackbox.h"
#include "logic.h"
#include "splice.h"
#if CONFIG_BOHUN_SPLICE_TEST
#include "esp_heap_caps.h"
#endif

#define MAX_PAIRS     12
/* Measured: bigger/PSRAM buffers and zero-copy do NOT help - the leader's
 * cost is per-packet tcpip + SPI, not copies. Leave at 2 KB. */
#define BUF_SZ        2048
#define IDLE_KILL_MS  30000
/* A client that has said nothing since accept is not a visitor (bohun_pair_silent):
 * scanners hoarding slots and PCBs for the full idle window were what filled
 * the pool under the leader and tripped its self-probe. */
#define SILENT_KILL_MS 8000
#define STATS_MS      5000
#define ROSTER_MS     1000            /* backend table refresh cadence */
#define FLUSH_MS      500             /* one side gone: how long to push what is still buffered */
/* Measured: deeper in-flight queues LOWER throughput past this point.
 * Change this alone, and measure. */
#define INFLIGHT_CAP  4
#define MAX_BACKENDS  8

/* Measured: a deeper backlog collapses under PCB exhaustion (each queued
 * connection costs a TCB; each relay costs two). Shallow + honest refusals. */
#define LISTEN_BACKLOG 8

static const char *TAG = "bohun_splice";

typedef struct {
    char *buf;                        /* PSRAM - allocated once at start */
    int  len, off;
    bool eof;
} dir_t;

typedef struct {
    int      cfd, bfd;
    uint8_t  backend_id;              /* member id - stable across roster refreshes */
    bool     used;
    bool     connecting;
    bool     c_spoke;                 /* the client has sent at least one byte */
    int64_t  last_io_ms;
    int64_t  t_accept;
    int64_t  t_conn;                  /* backend connect completed */
    int64_t  t_first;                 /* first byte back from the backend */
    int64_t  t_half;                  /* when the FIRST side went away */
    dir_t    c2b, b2c;
} pair_t;

typedef struct {
    uint8_t  id;
    uint32_t ip;                      /* network order, from the roster */
    bool     is_self;                 /* solo mode: our own loopback */
} backend_t;

/* THE CIRCUIT BREAKER: the heartbeat's `serving` flag is a claim, not a
 * pulse - a wedged blade can gossip serving=1 for hours. The splicer
 * benches on MEASURED outcomes only. */
#define FAIL_STREAK   3               /* dead connections before benching */
/* Doubling backoff: every retry of a wedged blade costs a real visitor,
 * so repeat offenders wait longer while brief stumbles heal in seconds. */
#define SUSPECT_MS    15000           /* first bench */
#define SUSPECT_MAX_MS 240000         /* ceiling - a dead blade is still re-checked */

/* Bench state is keyed by NODE ID: the backend table is rebuilt every second
 * in unstable order, so per-slot state evaporates or is inherited wrongly. */
#define MAX_IDS 8
static uint8_t  s_fails[MAX_IDS];
static int64_t  s_suspect_until[MAX_IDS];
static uint32_t s_bench_ms[MAX_IDS];
static inline bool benched(uint8_t id, int64_t now)
{
    return id < MAX_IDS && s_suspect_until[id] && now < s_suspect_until[id];
}

static pair_t    s_pairs[MAX_PAIRS];
static backend_t s_be[MAX_BACKENDS];
static int       s_nbe;
static uint32_t  s_next;          /* rotating tie-break cursor for equal-load backends */
static bool      s_active;
/* Drain is a REQUEST, executed by the splice task itself. Closing the pair
 * fds from the caller's task raced the relay loop: lwIP reuses a closed fd
 * number immediately, so a concurrent httpd accept could inherit it and
 * receive a dead pair's relay bytes mid-handshake. Only the task that
 * selects on the fds may close them. */
static volatile bool s_drain_req;
static const char *volatile s_drain_why;

static uint32_t s_accepted, s_completed, s_refused, s_killed, s_timeouts;
/* Two rings: setup timings bank when the backend first speaks; lifetimes
 * bank at close. One ring froze the stats behind keep-alive connections. */
#define PHASE_N 32
static uint16_t s_ph_conn[PHASE_N], s_ph_first[PHASE_N];
static uint32_t s_ph_setup_n;
static uint16_t s_ph_total[PHASE_N];
static uint32_t s_ph_n;

static uint32_t pctl_n(const uint16_t *ring, int pct, uint32_t count)
{
    uint32_t w[PHASE_N];
    uint32_t n = (count < PHASE_N) ? count : PHASE_N;
    for (uint32_t i = 0; i < n; i++) {
        w[i] = ring[i];
    }
    return bohun_pctl(w, n, (uint32_t)pct);
}
static uint32_t pctl(const uint16_t *ring, int pct) { return pctl_n(ring, pct, s_ph_n); }
static uint64_t s_bytes_in, s_bytes_out;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void set_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

typedef enum { END_DONE, END_ERR, END_REFUSED, END_TIMEOUT, END_SILENT } end_t;

static void safe_close(int *fd)
{
    if (*fd >= 0) {
        int f = *fd;
        *fd = -1;
        close(f);
    }
}

/* did this pair ever hear anything back from the backend? */
static void judge_backend(const pair_t *p, end_t why)
{
    uint8_t id = p->backend_id;
    if (id < MAX_IDS) {
        int i = id;                              /* keyed by id - see MAX_IDS above */
        if (why == END_DONE || p->t_first) {
            s_fails[i] = 0;                      /* it spoke - it is alive */
            s_suspect_until[i] = 0;
            s_bench_ms[i] = 0;                   /* forgiven completely */
        } else if (why == END_ERR || why == END_REFUSED || why == END_TIMEOUT) {
            /* Threshold and backoff are logic.h's bench laws: a repeat
             * offender re-benches on ONE failure, doubled each time; any
             * answer forgives instantly and completely. */
            if (++s_fails[i] >= bohun_bench_threshold(s_bench_ms[i], FAIL_STREAK)) {
                uint32_t b = bohun_bench_next_ms(s_bench_ms[i], SUSPECT_MS, SUSPECT_MAX_MS);
                s_bench_ms[i] = b;
                s_suspect_until[i] = now_ms() + b;
                s_fails[i] = 0;
                ESP_LOGW(TAG, "backend id=%u benched for %lu s", id, (unsigned long)(b / 1000));
                blackbox_event(BBX_SPLICE, id, "backend %u benched %lus", id,
                               (unsigned long)(b / 1000));
            }
        }
    }
}

static void pair_close(pair_t *p, end_t why)
{
    judge_backend(p, why);
    safe_close(&p->cfd);
    safe_close(&p->bfd);
    p->used = false;
    switch (why) {
    case END_DONE:    s_completed++; break;
    case END_ERR:     s_killed++;    break;
    case END_REFUSED: s_refused++;   break;
    case END_TIMEOUT: s_timeouts++;  break;
    case END_SILENT:  s_timeouts++;  break;   /* the idle bucket; the backend goes unjudged */
    }
}

/* in-flight per backend, counted from the live pairs - nothing to leak */
static int inflight(uint8_t backend_id)
{
    int n = 0;
    for (int i = 0; i < MAX_PAIRS; i++) {
        if (s_pairs[i].used && s_pairs[i].backend_id == backend_id) n++;
    }
    return n;
}

/* Every alive+serving blade that is not us; a dead backend leaves within
 * ~2 s of silence. SOLO FALLBACK: alone, splice to our own server over
 * loopback - the double-stack tax is paid only when the alternative is
 * serving nothing. The ladder's last rung. */
static void refresh_backends(void)
{
    swarm_snap_entry_t snap[8];
    uint8_t leader = 0;
    int n = swarm_snapshot(snap, 8, &leader);
    const swarm_member_t *self = swarm_self();
    uint8_t self_id = self ? self->id : 0;
    int old = s_nbe;
    bool was_solo = (s_nbe == 1 && s_be[0].is_self);
    s_nbe = 0;
    for (int i = 0; i < n && s_nbe < MAX_BACKENDS; i++) {
        if (snap[i].alive && snap[i].serving && !snap[i].self && snap[i].ip != 0) {
            s_be[s_nbe].id = snap[i].id;
            s_be[s_nbe].ip = snap[i].ip;
            s_be[s_nbe].is_self = false;
            s_nbe++;
        }
    }
    /* Self is NOT a backend except in solo mode - measured: adding the
     * leader as a fourth terminator DROPS fleet throughput (3.15 -> 2.99
     * conn/s), because the loopback tax lands on the one node that is
     * already the constraint. */
    bool solo = false;
    if (s_nbe == 0 && serve_is_ready()) {
        s_be[0].id = self_id;
        s_be[0].ip = htonl(INADDR_LOOPBACK);
        s_be[0].is_self = true;
        s_nbe = 1;
        solo = true;
    }
    if (s_nbe != old || solo != was_solo) {
        ESP_LOGW(TAG, "backend set: %d -> %d%s", old, s_nbe, solo ? " (SOLO - own loopback)" : "");
        blackbox_event(BBX_SPLICE, (uint16_t)s_nbe, "backends %d%s", s_nbe, solo ? " solo" : "");
    }
    /* Leaving solo: cut self-pairs, or keep-alive connections stay pinned
     * to the leader's loopback long after the fleet heals. One reconnect. */
    if (was_solo && !solo) {
        int cut = 0;
        for (int i = 0; i < MAX_PAIRS; i++) {
            if (s_pairs[i].used && s_pairs[i].backend_id == self_id) {
                pair_close(&s_pairs[i], END_DONE);
                cut++;
            }
        }
        if (cut) ESP_LOGW(TAG, "left solo - dropped %d self-served connection(s) to rebalance", cut);
    }
}

/* LEAST CONNECTIONS, round-robin on ties. Source-IP hash was measured worse
 * (one NAT'd source serialises on one blade); plain round-robin feeds busy
 * blades while others idle, since handshake costs vary ~2x. */
static int backend_pick(uint32_t src_ip)
{
    (void)src_ip;
    int best = -1, best_load = INFLIGHT_CAP;
    int64_t t = now_ms();
    /* Pass 1: blades we have no reason to doubt. */
    for (int k = 0; k < s_nbe; k++) {
        int idx = (int)((s_next + (uint32_t)k) % (uint32_t)s_nbe);   /* rotate the tie-break */
        if (benched(s_be[idx].id, t)) continue;
        int load = inflight(s_be[idx].id);
        if (load < best_load) {
            best = idx;
            best_load = load;
            if (load == 0) break;            /* an idle blade wins outright */
        }
    }
    /* Pass 2: everyone benched - try one anyway. The bench only shapes
     * which backend is picked, never whether a visitor is admitted:
     * refusing every visitor once darkened the site. */
    if (best < 0) {
        best_load = INFLIGHT_CAP;
        for (int k = 0; k < s_nbe; k++) {
            int idx = (int)((s_next + (uint32_t)k) % (uint32_t)s_nbe);
            int load = inflight(s_be[idx].id);
            if (load < best_load) { best = idx; best_load = load; }
        }
    }
    if (best >= 0) s_next = (uint32_t)best + 1;
    return best;
}

/* Room to put a connection - suspicion deliberately NOT consulted here. The
 * bench decides WHO gets the next visitor, never WHETHER there is one. */
static bool capacity_exists(void)
{
    for (int i = 0; i < s_nbe; i++) {
        if (inflight(s_be[i].id) < INFLIGHT_CAP) return true;
    }
    return false;
}

static int backend_connect_start(const backend_t *b)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    set_nonblock(fd);
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_BOHUN_BACKEND_PORT),
        .sin_addr.s_addr = b->ip,
    };
    int r = connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (r != 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    return fd;
}

/* DRAIN, do not sip: move as much as the sockets pass per readable turn
 * (bounded so one fast pair cannot starve the rest). One-chunk-per-select
 * turned a 21 KB page into 13 stalled sips. */
#define DRAIN_ROUNDS 12

static bool dir_step(dir_t *d, int src, int dst, bool src_readable, uint64_t *bytes)
{
    for (int round = 0; round < DRAIN_ROUNDS; round++) {
        if (d->len == d->off) {
            if (d->eof || (!src_readable && round == 0)) break;
            int n = recv(src, d->buf, BUF_SZ, 0);
            if (n > 0)       { d->len = n; d->off = 0; }
            else if (n == 0) { d->eof = true; break; }
            else if (errno == EWOULDBLOCK || errno == EAGAIN) break;
            else return false;
        }
        while (d->off < d->len) {
            int w = send(dst, d->buf + d->off, d->len - d->off, 0);
            if (w > 0) { d->off += w; *bytes += (uint64_t)w; }
            else if (errno == EWOULDBLOCK || errno == EAGAIN) break;
            else return false;
        }
        if (d->off < d->len) break;        /* sink is full - park and wait for writability */
        d->len = d->off = 0;
    }
    /* NO shutdown(), deliberately: it raced RST into a use-after-free in
     * lwIP under churn. TLS close_notify travels as data and closes the
     * peer; the FIN ceremony is not needed. */
    return true;
}

static void splice_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < MAX_PAIRS; i++) {
        s_pairs[i].cfd = s_pairs[i].bfd = -1;
        /* internal SRAM: the relay copies through these on every byte, and
         * PSRAM latency measurably hurt (see BUF_SZ above) */
        s_pairs[i].c2b.buf = malloc(BUF_SZ);
        s_pairs[i].b2c.buf = malloc(BUF_SZ);
        if (!s_pairs[i].c2b.buf || !s_pairs[i].b2c.buf) {
            ESP_LOGE(TAG, "no memory for relay buffers");
        }
    }

    /* BIND, AND KEEP TRYING. :443 is briefly busy exactly at mask handover
     * (the old listener is still releasing it) - that is a reason to wait,
     * never to die. Note: a FreeRTOS task that returns aborts the node. */
    int lsock = -1;
    for (int attempt = 1; ; attempt++) {
        lsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (lsock >= 0) {
            int one = 1;
            setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            set_nonblock(lsock);
            struct sockaddr_in me = {
                .sin_family = AF_INET,
                .sin_port = htons(CONFIG_BOHUN_SPLICE_PORT),
                .sin_addr.s_addr = htonl(INADDR_ANY),
            };
            if (bind(lsock, (struct sockaddr *)&me, sizeof(me)) == 0 &&
                listen(lsock, LISTEN_BACKLOG) == 0) {
                break;                       /* we own the door */
            }
            close(lsock);
            lsock = -1;
        }
        if (attempt == 1 || attempt % 30 == 0) {
            ESP_LOGW(TAG, "cannot own :%d yet (attempt %d) - retrying",
                     CONFIG_BOHUN_SPLICE_PORT, attempt);
        }
        if (attempt == 1) {
            blackbox_event(BBX_SPLICE, (uint16_t)CONFIG_BOHUN_SPLICE_PORT,
                           "splice bind busy on :%d - retrying",
                           CONFIG_BOHUN_SPLICE_PORT);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    s_active = true;
    ESP_LOGI(TAG, "splicing :%d -> :%d across the roster (every blade runs this; "
                  "only the mask-wearer receives public traffic)",
             CONFIG_BOHUN_SPLICE_PORT, CONFIG_BOHUN_BACKEND_PORT);
    blackbox_event(BBX_SPLICE, 0, "splice up on :%d", CONFIG_BOHUN_SPLICE_PORT);

    int64_t last_stats = now_ms(), last_roster = 0;
    for (;;) {
        int64_t t = now_ms();
        if (s_drain_req) {
            const char *why = s_drain_why;
            int cut = 0;
            for (int i = 0; i < MAX_PAIRS; i++) {
                if (s_pairs[i].used) {
                    pair_close(&s_pairs[i], END_DONE);   /* deliberate, not a failure */
                    cut++;
                }
            }
            s_drain_req = false;
            if (cut) {
                ESP_LOGW(TAG, "drained %d connection(s) before %s",
                         cut, why ? why : "handover");
                blackbox_event(BBX_SPLICE, (uint16_t)cut, "drained %d conns before %s",
                               cut, why ? why : "handover");
            }
        }
        if (t - last_roster >= ROSTER_MS) {
            last_roster = t;
            refresh_backends();
        }

        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        int maxfd = -1;
        int slot = -1;
        for (int i = MAX_PAIRS - 1; i >= 0; i--) {
            if (!s_pairs[i].used) slot = i;
        }
        if (slot >= 0 && capacity_exists()) {
            FD_SET(lsock, &rd);
            maxfd = lsock;
        }
        for (int i = 0; i < MAX_PAIRS; i++) {
            pair_t *p = &s_pairs[i];
            if (!p->used) continue;
            if (p->connecting) {
                FD_SET(p->bfd, &wr);
            } else {
                if (!p->c2b.eof && p->c2b.len == p->c2b.off) FD_SET(p->cfd, &rd);
                if (!p->b2c.eof && p->b2c.len == p->b2c.off) FD_SET(p->bfd, &rd);
                if (p->c2b.len > p->c2b.off) FD_SET(p->bfd, &wr);
                if (p->b2c.len > p->b2c.off) FD_SET(p->cfd, &wr);
            }
            if (p->cfd > maxfd) maxfd = p->cfd;
            if (p->bfd > maxfd) maxfd = p->bfd;
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };
        int r = select(maxfd + 1, &rd, &wr, NULL, &tv);
        if (r < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* WHILE, not IF. The `break` below must bind to THIS loop - inside
         * an `if` it binds to the outer for(;;), splice_task returns, and
         * FreeRTOS aborts the node. */
        while (slot >= 0 && FD_ISSET(lsock, &rd)) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int c = accept(lsock, (struct sockaddr *)&from, &fl);
            /* Backlog drained - LEAVE, or the loop spins on a stale
             * FD_ISSET at 96% CPU and starves core 0. */
            if (c < 0) {
                break;
            }
            {
                int idx = backend_pick(from.sin_addr.s_addr);
                int b = idx >= 0 ? backend_connect_start(&s_be[idx]) : -1;
                if (b < 0) {
                    close(c);
                    s_refused++;
                } else {
                    set_nonblock(c);
                    set_nodelay(c);
                    pair_t *p = &s_pairs[slot];
                    /* NOT memset: that would clear the buffer POINTERS and the
                     * next recv would write through NULL. Reset the state only. */
                    p->c2b.len = p->c2b.off = 0; p->c2b.eof = false;
                    p->b2c.len = p->b2c.off = 0; p->b2c.eof = false;
                    p->cfd = c;
                    p->bfd = b;
                    p->backend_id = s_be[idx].id;
                    p->connecting = true;
                    p->c_spoke = false;
                    p->used = true;
                    p->last_io_ms = now_ms();
                    p->t_accept = p->last_io_ms;
                    p->t_conn = p->t_first = p->t_half = 0;
                    s_accepted++;
                }
            }
            /* next free slot for the following accept in this same turn */
            slot = -1;
            for (int i = MAX_PAIRS - 1; i >= 0; i--) {
                if (!s_pairs[i].used) slot = i;
            }
            if (slot < 0 || !capacity_exists()) break;
        }

        t = now_ms();
        for (int i = 0; i < MAX_PAIRS; i++) {
            pair_t *p = &s_pairs[i];
            if (!p->used) continue;

            if (p->connecting) {
                if (FD_ISSET(p->bfd, &wr)) {
                    int err = 0;
                    socklen_t el = sizeof(err);
                    getsockopt(p->bfd, SOL_SOCKET, SO_ERROR, &err, &el);
                    if (err != 0) {
                        swarm_record_refusal(true);   /* backend refused us - ours to own */
                        pair_close(p, END_REFUSED);
                        continue;
                    }
                    set_nodelay(p->bfd);
                    p->connecting = false;
                    p->last_io_ms = t;
                    p->t_conn = t;
                }
                if (p->used && t - p->last_io_ms > IDLE_KILL_MS) {
                    pair_close(p, END_TIMEOUT);
                }
                continue;
            }

            uint64_t bi = s_bytes_in, bo = s_bytes_out;
            bool ok = dir_step(&p->c2b, p->cfd, p->bfd,
                               FD_ISSET(p->cfd, &rd), &s_bytes_in)
                   && dir_step(&p->b2c, p->bfd, p->cfd,
                               FD_ISSET(p->bfd, &rd), &s_bytes_out);
            if (s_bytes_in != bi || s_bytes_out != bo) p->last_io_ms = t;
            if (s_bytes_in != bi) p->c_spoke = true;
            if (s_bytes_out != bo && !p->t_first) {
                p->t_first = t;                       /* backend spoke - bank the setup */
                int si = (int)(s_ph_setup_n % PHASE_N);
                s_ph_conn[si]  = (uint16_t)(p->t_conn ? p->t_conn - p->t_accept : 0);
                s_ph_first[si] = (uint16_t)(t - p->t_accept);
                s_ph_setup_n++;
            }

            /* Either end gone + buffers flushed = transaction over. Waiting
             * for both FINs parks slots behind keep-alive backends and caps
             * throughput at the slot-hold time. */
            bool gone    = p->c2b.eof || p->b2c.eof;
            bool flushed = p->c2b.len == p->c2b.off && p->b2c.len == p->b2c.off;
            if (!ok) {
                pair_close(p, END_ERR);
            } else if (gone && flushed) {
#if CONFIG_BOHUN_SPLICE_TEST
                ESP_LOGI(TAG, "done be=%u total=%ld ms",
                         p->backend_id, (long)(t - p->t_accept));
#endif
                /* The leader terminates nothing, so its serve.c counters
                 * would read zero forever; the splicer keeps its own ledger. */
                /* NOT swarm_record_request(): that counter means HTTP
                 * requests answered HERE. A relay is one connection, many
                 * requests, served elsewhere. */
                s_ph_total[(int)(s_ph_n % PHASE_N)] = (uint16_t)(t - p->t_accept);
                s_ph_n++;
                pair_close(p, END_DONE);
            } else if (gone && !p->t_half) {
                p->t_half = t;                /* still bytes to push - brief flush window */
            } else if (p->t_half && t - p->t_half > FLUSH_MS) {
                pair_close(p, END_TIMEOUT);
            } else if (bohun_pair_silent(p->c_spoke, p->t_accept, t, SILENT_KILL_MS)) {
                pair_close(p, END_SILENT);
            } else if (t - p->last_io_ms > IDLE_KILL_MS) {
                pair_close(p, END_TIMEOUT);
            }
        }

        if (t - last_stats >= STATS_MS) {
            last_stats = t;
#if CONFIG_BOHUN_SPLICE_TEST
            /* Heap-corruption trap: validate INTERNAL heap every 5 s so an
             * lwIP scribble aborts near its cause with the block named.
             * INTERNAL only - sweeping poisoned PSRAM inside the heap
             * spinlock outlasts the interrupt watchdog and boot-loops. */
            if (!heap_caps_check_integrity(MALLOC_CAP_INTERNAL, true)) {
                ESP_LOGE(TAG, "HEAP INTEGRITY FAILED - freezing for the coroner");
                blackbox_event(BBX_SPLICE, 0xDEAD, "heap integrity failed in splice window");
                abort();
            }
#endif
            int active = 0;
            for (int i = 0; i < MAX_PAIRS; i++) active += s_pairs[i].used;
            if (active || s_accepted) {
                ESP_LOGI(TAG, "be %d | active %d | acc %lu done %lu ref %lu "
                              "kill %lu idle %lu | in %llu KB out %llu KB",
                         s_nbe, active, (unsigned long)s_accepted,
                         (unsigned long)s_completed, (unsigned long)s_refused,
                         (unsigned long)s_killed, (unsigned long)s_timeouts,
                         (unsigned long long)(s_bytes_in >> 10),
                         (unsigned long long)(s_bytes_out >> 10));
            }
        }
    }
}

void splice_start(void)
{
    /* Core 0, beside lwIP - measured: pinning the relay to the idle core
     * drops high-concurrency throughput (3.33 -> 2.84 conn/s); the relay
     * wants the same core as the stack it feeds. */
    xTaskCreatePinnedToCore(splice_task, "splice", 6144, NULL, 5, NULL, 0);
}

void splice_drain(const char *why)
{
    if (!s_active) {
        return;               /* no listener yet, so nothing can be in flight */
    }
    s_drain_why = why;
    s_drain_req = true;
    /* The splice task honours the request on its next loop turn (its select
     * tick is 10 ms). Wait briefly so the pairs are gone before the caller
     * bounces the interface; on timeout they die with the interface anyway. */
    for (int i = 0; i < 20 && s_drain_req; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool splice_is_up(void) { return s_active; }

bool splice_stats_get(splice_stats_t *out)
{
    if (!out || !s_active) return false;
    int active = 0;
    for (int i = 0; i < MAX_PAIRS; i++) active += s_pairs[i].used;
    out->active = s_active;
    /* "backends" is what the fleet can USE. A blade the balancer has measured as
     * dead is not a backend just because it still says it is: the roster showed
     * three active backends with one of them wedged, which is the fleet
     * repeating a corpse's own opinion of itself. */
    int64_t now = now_ms();
    uint8_t healthy = 0, mask = 0;
    for (int i = 0; i < s_nbe; i++) {
        if (benched(s_be[i].id, now)) mask |= (uint8_t)(1u << (s_be[i].id & 7));
        else healthy++;
    }
    out->backends = healthy;
    out->known = (uint8_t)s_nbe;
    out->benched_mask = mask;
    out->busy = (uint8_t)active;
    out->accepted = s_accepted;
    out->completed = s_completed;
    out->refused = s_refused;
    out->killed = s_killed;
    out->timeouts = s_timeouts;
    out->p50_conn_ms  = pctl_n(s_ph_conn, 50, s_ph_setup_n);
    out->p50_first_ms = pctl_n(s_ph_first, 50, s_ph_setup_n);
    out->p50_total_ms = pctl(s_ph_total, 50);
    out->p95_total_ms = pctl(s_ph_total, 95);
    return true;
}
