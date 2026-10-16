/* logic.h - the swarm's pure decision kernels, in one dependency-free header.
 *
 * Everything here is plain C99 over plain integers and strings: no FreeRTOS,
 * no lwIP, no esp_*. That is the point - these are the rules the fleet lives
 * by (who leads, who is alive, what availability means, when a backend is
 * benched), and keeping them free of hardware lets the host test suite in
 * firmware/tests/ prove them on every change without a board on the desk.
 * The callers own locking and I/O; these functions own only the arithmetic.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

/* Liveness from 32-bit millisecond stamps. Unsigned subtraction survives the
 * 49-day wrap; last_seen == 0 means "never heard at all", not "heard at t=0"
 * (the first heartbeat always lands at now > 0). */
static inline bool bohun_peer_alive_ms(uint32_t last_seen_ms, uint32_t now_ms,
                                       uint32_t lost_after_ms)
{
    return last_seen_ms != 0 && (uint32_t)(now_ms - last_seen_ms) < lost_after_ms;
}

/* Availability in tenths of a percent (1000 = 100.0%). served = requests
 * answered 2xx/3xx, refused = every non-2xx, refused_srv = the 5xx subset.
 * Only OUR failures count against us: a 404 to a scanner is a correct answer,
 * and counting the internet's background noise as downtime makes a healthy
 * fleet read as broken. No traffic at all is 100%, not 0. The clamp covers
 * peers whose counters were gossiped in separate frames and can skew. */
static inline uint16_t bohun_avail_x10(uint32_t served, uint32_t refused,
                                       uint32_t refused_srv)
{
    uint32_t total = served + refused;
    if (total == 0) {
        return 1000;
    }
    if (refused_srv > total) {
        refused_srv = total;
    }
    return (uint16_t)(((uint64_t)(total - refused_srv) * 1000) / total);
}

/* Heartbeat delivery percentage over the window since we first heard a peer
 * (first_seq), so it is not skewed by the observer having booted later than
 * the sender. A reboot resets the window (last < first): call it 100 until
 * the new window has data. */
static inline uint32_t bohun_delivery_pct(uint32_t first_seq, uint32_t last_seq,
                                          uint32_t rx_win)
{
    uint32_t span = (last_seq >= first_seq) ? (last_seq - first_seq + 1) : 1;
    return (rx_win >= span) ? 100 : (rx_win * 100) / span;
}

/* Heartbeats missed from one sender between two frames, by seq arithmetic.
 * prev_seq == 0 means "first frame ever heard" - no gap to report. A seq
 * that went BACKWARDS is a reboot, also not a gap. Capped at 99: the
 * consumer prints two digits and a bigger number carries no more meaning. */
static inline uint8_t bohun_seq_gap(uint32_t prev_seq, uint32_t seq)
{
    if (prev_seq == 0 || seq <= prev_seq + 1) {
        return 0;
    }
    uint32_t g = seq - prev_seq - 1;
    return g > 99 ? 99 : (uint8_t)g;
}

/* Nearest-rank percentile over w[0..cnt), sorting w in place (insertion sort -
 * every caller's ring is <= 128 wide). The caller passes a scratch copy; the
 * live ring is never reordered. */
static inline uint32_t bohun_pctl(uint32_t *w, uint32_t cnt, uint32_t pct)
{
    if (cnt == 0) {
        return 0;
    }
    for (uint32_t i = 1; i < cnt; i++) {
        uint32_t v = w[i], j = i;
        while (j > 0 && w[j - 1] > v) {
            w[j] = w[j - 1];
            j--;
        }
        w[j] = v;
    }
    return w[(pct * (cnt - 1)) / 100];
}

/* THE ELECTION. Deterministic and preemptive: the leader is the lowest-id
 * eligible member that can actually serve - either ourselves (self_ok folds
 * in eligibility, link and a listening server) or a peer that is alive AND
 * holding up its end of the data plane. Every node computes the same answer
 * from the same roster; no coordinator, no vote exchange, nothing to wedge.
 * 0 = nobody can lead. */
typedef struct {
    uint8_t id;
    bool eligible;
    bool alive;      /* heard on the radio inside the lost window */
    bool able;       /* link up AND its TLS server is listening */
} bohun_cand_t;

static inline uint8_t bohun_elect(uint8_t self_id, bool self_ok,
                                  const bohun_cand_t *peers, size_t n)
{
    uint8_t best = 0xff;
    if (self_ok) {
        best = self_id;
    }
    for (size_t i = 0; i < n; i++) {
        if (peers[i].alive && peers[i].able && peers[i].eligible
            && peers[i].id < best) {
            best = peers[i].id;
        }
    }
    return best == 0xff ? 0 : best;
}

/* THE BENCH's two laws (the splicer's circuit breaker).
 * Threshold: a fresh offender must fail `streak` connections in a row before
 * it is benched, but a repeat offender re-benches on ONE - re-proving a known
 * bad blade from scratch costs three visitors per cycle.
 * Backoff: each re-bench doubles the sit-out, capped so a dead blade is still
 * re-checked. Any successful answer forgives instantly and completely. */
static inline uint8_t bohun_bench_threshold(uint32_t prev_bench_ms, uint8_t streak)
{
    return prev_bench_ms ? 1 : streak;
}

static inline uint32_t bohun_bench_next_ms(uint32_t prev_bench_ms,
                                           uint32_t first_ms, uint32_t max_ms)
{
    uint32_t b = prev_bench_ms ? prev_bench_ms * 2 : first_ms;
    return b > max_ms ? max_ms : b;
}

/* Does Accept-Encoding contain a "br" token? Token-boundary check so
 * "abrupt" or "sbr" never false-match; q-values ignored - no real client
 * advertises brotli only to refuse it. */
static inline bool bohun_accepts_br(const char *ae)
{
    for (const char *p = ae; (p = strstr(p, "br")) != NULL; p += 2) {
        bool head = (p == ae) || p[-1] == ',' || p[-1] == ' ' || p[-1] == '\t';
        char t = p[2];
        if (head && (t == '\0' || t == ',' || t == ';' || t == ' ' || t == '\t')) {
            return true;
        }
    }
    return false;
}

/* 10/8, 172.16/12, 192.168/16 - dotted-quad only, no names. Used by the port-80
 * redirect: echoing back a private literal the client itself typed discloses
 * nothing, where any hardcoded fallback would. */
static inline bool bohun_is_private_literal(const char *h)
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(h, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }
    if (a == 10) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    return false;
}

/* Does an Origin header name OUR host? Full-label match past the scheme, so
 * "https://host.evil.com" never passes as "host". host must arrive with any
 * :port already stripped. */
static inline bool bohun_origin_host_ok(const char *origin, const char *host)
{
    const char *oh = strstr(origin, "://");
    oh = oh ? oh + 3 : origin;
    size_t hlen = strlen(host);
    return hlen > 0 && strncmp(oh, host, hlen) == 0
           && (oh[hlen] == '\0' || oh[hlen] == ':' || oh[hlen] == '/');
}

/* TRUNCATION MUST STILL PARSE. A clamped JSON document ends mid-member with
 * no closing brackets - invalid to every consumer, indistinguishable from a
 * frontend bug. Walk back to the last complete member object and close the
 * document by hand: a SHORT roster is honest, an unparseable one is not.
 * Writes "]}\n" plus the NUL after the last '}', which needs cut+5 <= len;
 * returns the repaired length, or 0 when no repair fits the buffer. */
static inline size_t bohun_json_close_clamped(char *buf, size_t len)
{
    size_t cut = len - 1;
    while (cut > 0 && buf[cut] != '}') {
        cut--;
    }
    if (cut == 0 || len < cut + 5) {
        return 0;
    }
    memcpy(buf + cut + 1, "]}\n", 4);   /* incl. NUL */
    return cut + 3;
}

/* THE PROBE MUST NOT COMPETE WITH THE WORK. The self-probe is a loopback
 * connect, and a connect needs a TCP control block from the same pool the
 * splicer fills on purpose (LWIP_MAX_ACTIVE_TCP stays at 16 as the fleet's
 * admission control - sdkconfig.defaults has the measurements). A full pool
 * fails socket()/connect() with ENOBUFS, ENOMEM, ENFILE, EMFILE or EAGAIN:
 * that is "busy", not "dead", and reading it as dead made a fully loaded
 * leader stand itself down 21 times in 16 days. Only a refusal or a timeout
 * is evidence that the listener is gone. */
typedef enum { BOHUN_PROBE_OK, BOHUN_PROBE_BUSY, BOHUN_PROBE_DEAD } bohun_probe_t;

static inline bohun_probe_t bohun_probe_verdict(bool connected, int err)
{
    if (connected) {
        return BOHUN_PROBE_OK;
    }
    switch (err) {
    case ENOBUFS:
    case ENOMEM:
    case ENFILE:
    case EMFILE:
    case EAGAIN:
        return BOHUN_PROBE_BUSY;
    default:
        return BOHUN_PROBE_DEAD;
    }
}

/* One verdict folded into the failure streak: OK clears it, DEAD grows it,
 * BUSY leaves it exactly where it was - a probe that could not run neither
 * confirms nor forgives. Saturates rather than wrapping back to healthy. */
static inline uint8_t bohun_probe_streak(uint8_t fails, bohun_probe_t v)
{
    if (v == BOHUN_PROBE_OK) {
        return 0;
    }
    if (v == BOHUN_PROBE_DEAD) {
        return fails < 255 ? (uint8_t)(fails + 1) : 255;
    }
    return fails;
}

/* Fit to be elected and handed traffic? Not while the streak is at the sick
 * threshold, and not for hold_ms after the LAST sick-level failure even once
 * probes pass again. The election is lowest-id-wins with no stickiness, so
 * without the hold a recovered leader takes the mask straight back the moment
 * its pool drains: two handovers, seven seconds apart, for one hiccup.
 * stood_down_ms == 0 means never stood down; unsigned math survives the wrap. */
static inline bool bohun_probe_fit(uint8_t fails, uint8_t sick_at,
                                   uint32_t stood_down_ms, uint32_t now_ms,
                                   uint32_t hold_ms)
{
    if (fails >= sick_at) {
        return false;
    }
    if (stood_down_ms != 0 && (uint32_t)(now_ms - stood_down_ms) < hold_ms) {
        return false;
    }
    return true;
}

/* Room for the probe? A loopback connect costs two control blocks (the client
 * side and the accepted side) out of a pool of `pool`, and every relay pair
 * costs two. When the pairs leave fewer than four behind, the probe either
 * fails at socket() or takes the last block so the listener cannot accept -
 * a silent SYN drop that reads as a timeout, i.e. as death. Do not probe at
 * all then: the pool is full, and full is load. */
static inline bool bohun_probe_room(uint8_t pairs, uint16_t pool)
{
    return (uint16_t)(2u * pairs + 4u) <= pool;
}

/* A relayed connection whose client has sent nothing since accept is not a
 * visitor: a browser's ClientHello follows the SYN within one round trip.
 * Past silent_ms it is a scanner or a half-open probe holding a relay slot
 * and two control blocks out of the admission-control pool. It is killed,
 * and the backend is NOT judged for it - it was never asked anything, so
 * its silence is not evidence. Signed ms stamps, as the splicer keeps them. */
static inline bool bohun_pair_silent(bool client_spoke, int64_t accept_ms,
                                     int64_t now_ms, int64_t silent_ms)
{
    return !client_spoke && now_ms - accept_ms > silent_ms;
}

/* THE REFUSAL SAMPLER. The per-kind tallies say HOW MANY 400s; nothing said
 * WHAT they were - no peer, no Host, no request line - and 12,600 of them
 * landed on one leader in 36 hours with no way to tell a wordlist scanner
 * from a vhost probe from Host-less garbage. A small RAM ring of the most
 * recent refusals answers that: the newest N, each clipped to a line's worth
 * of fields. No flash - this is what is happening now, not history. Peers
 * mean different things per listener: the :80 redirect sees the internet
 * (the router forwards it straight to the mask), the public TLS port sees
 * the splicer's own address (the relay stands between). The dump says so. */
#define BOHUN_REFUSAL_N       32
#define BOHUN_REFUSAL_METHOD  8
#define BOHUN_REFUSAL_URI     48
#define BOHUN_REFUSAL_HOST    32

typedef struct {
    uint32_t at_ms;                      /* ms since boot */
    uint32_t peer;                       /* IPv4, host byte order; 0 = unknown */
    uint16_t port;                       /* the listener it arrived on */
    uint16_t status;                     /* what we answered: 400, 404, 500 ... */
    bool     bad_host;                   /* the :80 redirect's own 400 - Host named nobody we are */
    char     method[BOHUN_REFUSAL_METHOD];
    char     uri[BOHUN_REFUSAL_URI];
    char     host[BOHUN_REFUSAL_HOST];
} bohun_refusal_t;

typedef struct {
    bohun_refusal_t rec[BOHUN_REFUSAL_N];
    uint32_t total;                      /* ever recorded; the ring keeps the last N */
} bohun_refusals_t;

/* Bounded copy that always terminates; NULL or "" lands as "-" so a dump
 * column is never blank. Truncation is the point: the first bytes of a URI
 * name the scanner, the rest is its wordlist. cap must be >= 2. */
static inline void bohun_refusal_clip(char *dst, size_t cap, const char *src)
{
    if (!src || !*src) {
        src = "-";
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static inline void bohun_refusal_record(bohun_refusals_t *r, uint32_t at_ms, uint32_t peer,
                                        uint16_t port, uint16_t status, bool bad_host,
                                        const char *method, const char *uri, const char *host)
{
    bohun_refusal_t *e = &r->rec[r->total % BOHUN_REFUSAL_N];
    e->at_ms = at_ms;
    e->peer = peer;
    e->port = port;
    e->status = status;
    e->bad_host = bad_host;
    bohun_refusal_clip(e->method, sizeof(e->method), method);
    bohun_refusal_clip(e->uri, sizeof(e->uri), uri);
    bohun_refusal_clip(e->host, sizeof(e->host), host);
    r->total++;
}

/* Render newest first, one line per refusal, into out[cap]. Whole lines
 * only: a line that does not fit is dropped together with everything older,
 * so the text always parses and the NUL is always there. Returns the length. */
static inline size_t bohun_refusal_dump(const bohun_refusals_t *r, char *out, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    uint32_t have = r->total < BOHUN_REFUSAL_N ? r->total : BOHUN_REFUSAL_N;
    int n = snprintf(out, cap, "refusals total=%u showing=%u (newest first; peers on the "
                     "public TLS port are the splicer, not the visitor)\n",
                     (unsigned)r->total, (unsigned)have);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return 0;
    }
    size_t len = (size_t)n;
    for (uint32_t i = 0; i < have; i++) {
        const bohun_refusal_t *e = &r->rec[(r->total - 1 - i) % BOHUN_REFUSAL_N];
        n = snprintf(out + len, cap - len, "#%u +%u.%03us :%u %u.%u.%u.%u %u%s %s %s host=%s\n",
                     (unsigned)(r->total - i), (unsigned)(e->at_ms / 1000),
                     (unsigned)(e->at_ms % 1000), (unsigned)e->port,
                     (unsigned)(e->peer >> 24), (unsigned)((e->peer >> 16) & 255u),
                     (unsigned)((e->peer >> 8) & 255u), (unsigned)(e->peer & 255u),
                     (unsigned)e->status, e->bad_host ? " bad-host" : "",
                     e->method, e->uri, e->host);
        if (n < 0 || (size_t)n >= cap - len) {
            out[len] = '\0';
            break;
        }
        len += (size_t)n;
    }
    return len;
}

/* THE RATE RING. Observers see only monotonic counters in the heartbeat; a
 * count says how many refusals ever, never how many a minute ago. One add per
 * second (the observers' update cadence) keeps the last minute per second
 * (a sliding "now") and the last hour per minute (the chart). Saturating
 * stores: a 65,535-a-second flood reads as the ceiling, never as a wrap. */
#define BOHUN_RATE_SECS 60
#define BOHUN_RATE_MINS 60

typedef struct {
    uint16_t sec[BOHUN_RATE_SECS];   /* per-second deltas, the last minute */
    uint16_t min[BOHUN_RATE_MINS];   /* per-minute totals, the last hour */
    uint8_t  sec_pos, min_pos;
    uint8_t  sec_n;                  /* seconds banked toward the current minute */
    uint8_t  min_n;                  /* minutes recorded, saturates at BOHUN_RATE_MINS */
    uint32_t acc;                    /* the current minute so far */
} bohun_rate_ring_t;

static inline uint16_t bohun_sat16(uint32_t v)
{
    return v > 0xffffu ? (uint16_t)0xffffu : (uint16_t)v;
}

/* The change between two readings of a counter that may have been reset
 * underneath us (a reboot, POST /reset). A counter running backwards is a
 * fresh start, not a negative rate. */
static inline uint32_t bohun_delta(uint32_t now, uint32_t last, bool have_last)
{
    return (have_last && now >= last) ? now - last : 0;
}

/* Bank one second. Returns true when that second completed a minute, i.e.
 * min[] gained a bucket (bohun_rate_last_min reads it). */
static inline bool bohun_rate_add(bohun_rate_ring_t *r, uint32_t delta)
{
    r->sec[r->sec_pos] = bohun_sat16(delta);
    r->sec_pos = (uint8_t)((r->sec_pos + 1) % BOHUN_RATE_SECS);
    r->acc += delta;
    if (++r->sec_n < BOHUN_RATE_SECS) {
        return false;
    }
    r->min[r->min_pos] = bohun_sat16(r->acc);
    r->min_pos = (uint8_t)((r->min_pos + 1) % BOHUN_RATE_MINS);
    if (r->min_n < BOHUN_RATE_MINS) {
        r->min_n++;
    }
    r->acc = 0;
    r->sec_n = 0;
    return true;
}

/* the last 60 seconds, sliding - "per minute, now" */
static inline uint32_t bohun_rate_last60(const bohun_rate_ring_t *r)
{
    uint32_t sum = 0;
    for (int i = 0; i < BOHUN_RATE_SECS; i++) {
        sum += r->sec[i];
    }
    return sum;
}

/* the most recently completed minute (0 before the first one completes) */
static inline uint16_t bohun_rate_last_min(const bohun_rate_ring_t *r)
{
    if (r->min_n == 0) {
        return 0;
    }
    return r->min[(r->min_pos + BOHUN_RATE_MINS - 1) % BOHUN_RATE_MINS];
}

/* the busiest completed minute of the recorded hour */
static inline uint16_t bohun_rate_peak(const bohun_rate_ring_t *r)
{
    uint16_t peak = 0;
    for (int i = 0; i < r->min_n; i++) {
        if (r->min[i] > peak) {
            peak = r->min[i];
        }
    }
    return peak;
}

/* everything in the recorded hour, the running minute included */
static inline uint32_t bohun_rate_hour(const bohun_rate_ring_t *r)
{
    uint32_t sum = r->acc;
    for (int i = 0; i < r->min_n; i++) {
        sum += r->min[i];
    }
    return sum;
}

/* httpd's error enum, index -> the status it answers with. ONE table for the
 * blade's sampler and anyone else who reads err_kind[] by index; the blade
 * pins the count to the enum with a _Static_assert. Out of range -> 0. */
#define BOHUN_ERR_KINDS 13
static inline uint16_t bohun_err_status(unsigned idx)
{
    static const uint16_t t[BOHUN_ERR_KINDS] = {
        500, 501, 505, 400, 401, 403, 404, 405, 408, 411, 413, 414, 431
    };
    return idx < BOHUN_ERR_KINDS ? t[idx] : 0;
}

