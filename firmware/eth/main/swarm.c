/* swarm.c - the control plane: encrypted ESP-NOW heartbeats, the roster,
 * the election. Radio only; callbacks enqueue, named tasks own the work.
 * Tasks: swarm_hb_tx (1 Hz heartbeat), swarm_monitor (RX queue -> roster,
 * LOST after 3 s of silence). Keys come from gitignored secrets.h; rotating
 * them is a flag day - all nodes at once or they go deaf. */
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "swarm_identity.h"
#include "mask.h"
#include "serve.h"
#include "secrets.h"
#include "vitals.h"
#include "selftest.h"
#include "logic.h"
#include "swarm.h"

#define SWARM_CHANNEL     6
#define HB_PERIOD_MS      1000
#define PEER_LOST_MS      3000
#define ROSTER_PRINT_MS   5000

static const char *TAG = "bohun_swarm";
static uint16_t s_fw_ver;   /* this image's version.txt, gossiped since v15 */

/* Radio keys live in the gitignored secrets.h - an ASCII literal in tracked
 * source would be readable with `strings` on any binary. Same 16 bytes on
 * every node or nobody hears anybody. */
static const uint8_t PMK[16] = BOHUN_PMK;
static const uint8_t LMK[16] = BOHUN_LMK;

typedef struct __attribute__((packed)) {
    char magic[4];          /* "ACLD" */
    uint8_t ver;
    uint8_t node_id;
    uint32_t seq;
    uint32_t uptime_s;
    uint32_t ip;            /* v2: current eth IPv4 (esp_ip4 .addr), 0 if none */
    uint32_t generation;    /* v3: leadership-change count (election churn) */
    uint32_t req_count;     /* v3: total HTTP requests this node has served */
    uint32_t req_lat_us;    /* v3: most-recent request latency (microseconds) */
    uint8_t updating;       /* v4: 1 while this node is receiving an OTA */
    uint8_t update_pct;     /* v5: OTA image received, 0-100 */
    uint32_t req_err;       /* v6: requests refused/failed - with req_count gives availability */
    uint8_t link_up;        /* v7: Ethernet PHY is up */
    uint8_t serving;        /* v7: the TLS server is listening (not merely booted) */
    uint16_t err_kind[SERVE_ERR_KINDS];  /* v7: which errors, for the kobzar's table */
    uint8_t reset_reason;   /* v8: esp_reset_reason() from THIS boot - why we last died */
    uint32_t free_heap;     /* v8: bytes free right now */
    uint32_t min_free_heap; /* v8: low-water mark since boot */
    int16_t temp_c10;       /* v9: on-die temperature, tenths of a degree C */
    /* v10: the SPLICER's ledger. Gossiped because the public page is served by
     * a BACKEND - whichever blade the leader routed the reader to - and that
     * blade would otherwise have nothing truthful to say about the load
     * balancer standing in front of it. Every node reports its own splicer;
     * only the leader's is non-zero, and that is the one the page renders. */
    uint8_t  lb_backends;
    uint32_t lb_accepted;   /* connections taken in since this node was elected */
    uint32_t lb_done;       /* relayed to a clean close */
    uint32_t lb_refused;    /* turned away at the door: no free slot or backend */
    uint16_t lb_killed;     /* died mid-relay (client or backend broke the pipe) */
    uint16_t lb_idle;       /* reaped after going silent - a stalled reader */
    uint16_t lb_conn_ms;    /* p50 accept -> backend TCP established */
    uint16_t lb_first_ms;   /* p50 accept -> first byte back from the backend */
    uint16_t lb_total_ms;   /* p50 accept -> closed */
    uint16_t lb_p95_ms;     /* p95 of the same */
    /* v11: this node's OWN request latency percentiles, over its ring of recent
     * requests. The balancer can only time connections (it never sees a request
     * boundary), so the honest source for "how long does a request take" is the
     * blade that actually served it. Every blade computes its own; the page
     * averages the serving ones. */
    uint32_t req_p50_us;
    uint32_t req_p95_us;
    uint8_t  lb_known;        /* v12: backends in the table (healthy or not) */
    uint8_t  lb_benched;      /* v12: bitmask of node ids the balancer has benched */
    uint16_t pcb_active;      /* v13: lwIP live TCP PCBs (cap MAX_ACTIVE_TCP=16) */
    uint16_t pcb_tw;          /* v13: PCBs in TIME_WAIT (self-draining) */
    uint16_t pcb_bound;       /* v13: bound-but-not-connected PCBs */
    uint16_t pcb_listen;      /* v14: LISTENING PCBs - 0 means every port is dead */
    uint16_t fw_ver;          /* v15: this variant's version.txt integer. THE
                               * release identity on the glass; 0 = pre-v15 image */
    uint16_t clicks[SERVE_CLICK_KINDS];   /* v16: the conversion ledger, in
                               * serve.h's canonical slug order */
} hb_frame_t;
/* err_kind[] sits MID-FRAME, so growing SERVE_ERR_KINDS silently moves every
 * field after it while `ver` stays 9 - an un-reflashed observer would then read
 * reset_reason out of an error counter and heap/temperature out of noise. That
 * is the only realizable silent-corruption path left in the protocol, so make
 * it a compile error instead: bump `ver` and the RX gates when this fires. */
_Static_assert(sizeof(hb_frame_t) == 132, "wire layout changed - bump ver and the RX gates");

typedef struct {
    uint8_t src_mac[6];
    int8_t rssi;
    hb_frame_t frame;
} hb_event_t;

typedef struct {
    const swarm_member_t *member;
    /* ms in a uint32_t: a 64-bit store is two stores on Xtensa and readers
     * are lock-free - torn halves once flashed healthy peers as LOST. 32-bit
     * stores are atomic; unsigned subtraction survives the 49-day wrap. */
    uint32_t last_seen_ms;
    uint32_t last_seq;
    uint32_t last_uptime_s;
    int8_t last_rssi;
    uint32_t rx_count;
    uint32_t last_ip;
    uint32_t last_generation;
    uint16_t last_fw_ver;
    uint16_t last_clicks[SERVE_CLICK_KINDS];
    uint32_t last_req_count;
    uint32_t last_req_lat_us;
    uint32_t last_req_err;
    uint8_t last_link_up;
    uint8_t last_serving;
    uint16_t last_err_kind[SERVE_ERR_KINDS];
    uint8_t last_updating;
    uint8_t last_update_pct;
    uint8_t last_reset_reason;
    uint32_t last_free_heap;
    uint32_t last_min_free_heap;
    int16_t last_temp_c10;
    uint32_t last_p50_us, last_p95_us;   /* v11: the peer's own request percentiles */
    uint8_t  last_lb_backends;      /* v10: the peer's splicer, if it is the leader */
    uint8_t  last_lb_known, last_lb_benched;   /* v12 */
    uint16_t last_pcb_active, last_pcb_tw, last_pcb_bound;   /* v13 */
    uint16_t last_pcb_listen;                                /* v14 */
    uint32_t last_lb_accepted, last_lb_done, last_lb_refused;
    uint16_t last_lb_killed, last_lb_idle;
    uint16_t last_lb_conn_ms, last_lb_first_ms, last_lb_total_ms, last_lb_p95_ms;
    uint32_t first_seq;   /* seq when we (re)started counting this peer */
    uint32_t rx_win;      /* heartbeats received since first_seq */
} roster_entry_t;

static QueueHandle_t s_rx_queue;
static roster_entry_t s_roster[8];

/* THE ROSTER LOCK: the monitor writes ~20 fields per entry, JSON and
 * snapshots read them. Write ordering alone still let readers catch a
 * half-updated entry; both sides are sub-microsecond, so a spinlock is free. */
static portMUX_TYPE s_roster_mux = portMUX_INITIALIZER_UNLOCKED;
/* THE RADIO TAIL. One entry per heartbeat heard, ~45 s of a six-member fleet
 * at 1 Hz. Static BSS on every build that links swarm.c - the blades carry the
 * 3.5 KB too and simply never read it, which beats forking the file. Its own
 * lock: tail writes must never lengthen the roster critical section. */
#define RTAIL_N 288
static swarm_radio_evt_t s_rtail[RTAIL_N];
static uint16_t s_rtail_head, s_rtail_count;
static uint32_t s_rtail_prev_seq[16];
static portMUX_TYPE s_rtail_mux = portMUX_INITIALIZER_UNLOCKED;

static void rtail_push(uint8_t id, uint32_t seq, int8_t rssi, bool serving, uint32_t now_ms)
{
    uint8_t gap = 0;
    if (id < 16) {
        gap = bohun_seq_gap(s_rtail_prev_seq[id], seq);
        s_rtail_prev_seq[id] = seq;
    }
    portENTER_CRITICAL(&s_rtail_mux);
    s_rtail[s_rtail_head] = (swarm_radio_evt_t){
        .ms = now_ms, .seq = seq, .id = id, .rssi = rssi,
        .gap = gap, .serving = serving };
    s_rtail_head = (s_rtail_head + 1) % RTAIL_N;
    if (s_rtail_count < RTAIL_N) s_rtail_count++;
    portEXIT_CRITICAL(&s_rtail_mux);
}

int swarm_radio_tail(swarm_radio_evt_t *out, int max)
{
    portENTER_CRITICAL(&s_rtail_mux);
    int n = s_rtail_count < max ? s_rtail_count : max;
    for (int i = 0; i < n; i++) {
        out[i] = s_rtail[(s_rtail_head - 1 - i + 2 * RTAIL_N) % RTAIL_N];
    }
    portEXIT_CRITICAL(&s_rtail_mux);
    return n;
}

/* alive, from 32-bit millisecond stamps - a torn read is impossible because
 * the last_seen store is atomic; the window arithmetic lives in logic.h */
static inline bool peer_alive(const roster_entry_t *r, uint32_t now_ms)
{
    return bohun_peer_alive_ms(r->last_seen_ms, now_ms, PEER_LOST_MS);
}
static size_t s_roster_n;
static uint32_t s_self_seq;   /* our own last-sent heartbeat seq, for /roster completeness */
static uint32_t s_generation; /* how many times leadership has changed since boot */
static uint32_t s_req_count;  /* HTTP requests served (serve.c pushes via swarm_record_request) */
static uint32_t s_req_at_claim; /* req_count when this node last claimed the mask - the "since election" baseline */
static uint32_t s_times_elected; /* lifetime mask claims by THIS node - NVS-persisted so OTA reboots don't wipe it */
static nvs_handle_t s_nvs;
static bool s_elected_dirty;     /* counter moved since the last commit */
static int64_t s_elected_saved_us;
#define ELECTED_SAVE_MS 60000    /* flash is finite; a flapping election is not */
static uint32_t s_req_lat_us; /* most-recent request latency, microseconds */
static uint32_t s_req_err;    /* every non-2xx we answered with (the refusal ledger) */
static uint32_t s_req_err_srv; /* the 5xx subset - the only ones that are OUR failure */

/* The observers (display, mission) compile this control plane but NOT serve.c -
 * they have no HTTP server at all. These weak definitions give them sane answers;
 * the eth firmware's real ones in serve.c override them at link time. An observer
 * reporting "ready" is honest: it is never a leadership candidate, so it can never
 * gate an election, and it has no errors to tally because it refuses nothing. */
__attribute__((weak)) bool serve_is_ready(void) { return true; }
/* Does this BUILD have a web server at all? serve.c overrides with true.
 * Without this gate, observer firmware flashed onto a blade once advertised
 * serving=1 with no server aboard - eligible for a mask it could not honour. */
__attribute__((weak)) bool serve_is_real(void) { return false; }
/* observers have no blackbox partition and no recorder - events vanish there */
__attribute__((weak)) void blackbox_event(uint8_t kind, uint16_t code, const char *fmt, ...) {
    (void)kind; (void)code; (void)fmt;
}
/* was: a hand-copied `enum { BBX_MASK_ON = 5, BBX_MASK_OFF = 6 }`. A
 * renumber in blackbox.h would have silently relabelled every mask event in the
 * ring. blackbox.h is on all three variants' include path (the observers' own
 * CMakeLists add ../../eth/main), so just include it. */
#include "blackbox.h"
#include "splice.h"
#include "lwip/tcpip.h"
#include "lwip/priv/tcp_priv.h"

/* THE WEDGE PROBE: count lwIP's PCB lists (a leak = active pinned at the
 * cap, never draining). The walk runs ON the tcpip thread via callback - the
 * one place it cannot race a free - and that thread is exactly what survives
 * a wedge, so the numbers still travel by radio when the wire cannot. */
static volatile uint16_t s_pcb_active, s_pcb_tw, s_pcb_bound, s_pcb_listen;
static volatile uint32_t s_tcpip_beat;   /* bumped inside count_pcbs_cb (tcpip thread) */
static void count_pcbs_cb(void *arg)
{
    (void)arg;
    uint16_t a = 0, t = 0, b = 0;
    for (struct tcp_pcb *pcb = tcp_active_pcbs; pcb; pcb = pcb->next) a++;
    for (struct tcp_pcb *pcb = tcp_tw_pcbs;     pcb; pcb = pcb->next) t++;
    for (struct tcp_pcb *pcb = tcp_bound_pcbs;  pcb; pcb = pcb->next) b++;
    /* THE LISTENERS - the one census whose zero explains every symptom of a
     * wedge at once: normal active/tw counts, megabytes of free heap, a tcpip
     * thread healthy enough to run THIS callback - and every port refusing.
     * With the listen PCBs gone there is nothing for a SYN to land on, while
     * ICMP, which needs no listener, keeps answering. */
    uint16_t l = 0;
    for (struct tcp_pcb_listen *pcb = tcp_listen_pcbs.listen_pcbs; pcb; pcb = pcb->next) l++;
    s_pcb_active = a; s_pcb_tw = t; s_pcb_bound = b; s_pcb_listen = l;
    s_tcpip_beat++;   /* proof-of-life: only advances when the tcpip thread runs */
}
__attribute__((weak)) void serve_reset_errors(void) { }
/* observers serve no page, so nobody can click one there */
__attribute__((weak)) const uint32_t *serve_click_counts(void) {
    static const uint32_t zeros[SERVE_CLICK_KINDS];
    return zeros;
}
/* Weak stubs so a variant that omits vitals.c fails SOFT (sentinel in the
 * frame), never fails to link - observer builds once stopped compiling for a
 * day before anyone noticed the update path was broken. */
/* observers have no D8 trial - they are USB-flashed, single-slot */
__attribute__((weak)) bool selftest_on_trial(void) { return false; }
__attribute__((weak)) void vitals_sample(void) { }
/* observers have no splicer; the admin JSON below simply omits the block */
__attribute__((weak)) bool splice_stats_get(splice_stats_t *out) { (void)out; return false; }
/* no splicer in this build - the D8 trial must not wait for one */
__attribute__((weak)) bool splice_is_up(void) { return true; }
/* observers have no splicer, so there is nothing to drain when the mask moves */
__attribute__((weak)) void splice_drain(const char *why) { (void)why; }
__attribute__((weak)) int16_t vitals_temp_c10(void) { return VITALS_TEMP_NONE; }
__attribute__((weak)) const uint16_t *serve_err_counts(void)
{
    static const uint16_t none[SERVE_ERR_KINDS];
    return none;
}
static uint32_t s_rx_dropped; /* radio frames rejected before the queue (junk or unledgered) */

/* Why this node last died: esp_reset_reason() is valid for the CURRENT boot
 * only - latch at startup, gossip forever. SW vs PANIC vs BROWNOUT vs
 * INT_WDT demand entirely different fixes. */
static uint8_t s_reset_reason;
static uint8_t s_vitals_tick;   /* 200 ms ticks since the last thermometer read */

const char *swarm_reset_reason_str(uint8_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "POWER-ON";
    case ESP_RST_EXT:        return "EXT RESET";
    case ESP_RST_SW:         return "SOFTWARE";     /* an OTA reboot lands here - expected */
    case ESP_RST_PANIC:      return "PANIC";
    case ESP_RST_INT_WDT:    return "INT WDT";
    case ESP_RST_TASK_WDT:   return "TASK WDT";
    case ESP_RST_WDT:        return "OTHER WDT";
    case ESP_RST_DEEPSLEEP:  return "DEEP SLEEP";
    case ESP_RST_BROWNOUT:   return "BROWNOUT";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU LOCKUP";
    default:                 return "UNKNOWN";
    }
}

#define LAT_RING_N 128
static uint32_t s_lat_ring[LAT_RING_N]; /* last N request latencies, for avg + p95 */
static uint32_t s_lat_total;            /* lifetime samples pushed into the ring */
static volatile bool s_updating;  /* true while THIS node is taking an OTA */
static volatile uint8_t s_update_pct;

void swarm_set_ota(bool on) { s_updating = on; s_update_pct = 0; }
void swarm_set_ota_progress(uint8_t pct) { s_update_pct = pct > 100 ? 100 : pct; }
bool swarm_own_updating(void) { return s_updating; }
uint8_t swarm_own_update_pct(void) { return s_update_pct; }

int swarm_peers_heard(void)
{
    int n = 0;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    for (size_t i = 0; i < s_roster_n; i++) {
        if (peer_alive(&s_roster[i], now_ms)) n++;
    }
    return n;
}

/* the OTA window drags the radio onto the AP's channel; put it back */
void swarm_repin_channel(void)
{
    esp_wifi_set_channel(SWARM_CHANNEL, WIFI_SECOND_CHAN_NONE);
}

void swarm_reset_stats(void)
{
    s_req_count = 0;
    s_req_err = 0;
    s_req_err_srv = 0;
    s_req_at_claim = 0;
    s_req_lat_us = 0;
    s_lat_total = 0;
    memset(s_lat_ring, 0, sizeof(s_lat_ring));
    s_rx_dropped = 0;
    s_generation = 0;
    /* A reign in progress is not history to be erased. The counter only ever
     * increments on a leadership TRANSITION, so zeroing it while this node wears
     * the mask leaves the current otaman claiming zero elections until it happens
     * to lose and retake the address - which could be days. If we hold it now, we
     * were elected at least once, and the count starts at one. */
    s_times_elected = mask_is_wearing() ? 1 : 0;
    if (s_nvs) {
        nvs_set_u32(s_nvs, "elected", s_times_elected);
        nvs_commit(s_nvs);
    }
    s_elected_dirty = false;
    serve_reset_errors();
    ESP_LOGW(TAG, "STATS WIPED - counters, latency ring and the election tally are zero");
}

/* Same story as serve.c's tally(): reached from every httpd task on both cores.
 * swarm_record_request() shares the lock because it touches the latency ring,
 * which a concurrent writer could otherwise interleave mid-sample. */
static portMUX_TYPE s_count_mux = portMUX_INITIALIZER_UNLOCKED;

void swarm_record_refusal(bool our_fault)
{
    portENTER_CRITICAL(&s_count_mux);
    s_req_err++;
    if (our_fault) {
        s_req_err_srv++;
    }
    portEXIT_CRITICAL(&s_count_mux);
}

void swarm_record_error(void) { swarm_record_refusal(false); }

/* availability = what fraction of everything we were asked for, we actually served.
 * Counts only what the server can observe: handler responses vs handler refusals.
 * A connection dropped before HTTP is parsed (socket pool full, TLS handshake failed)
 * never reaches us, so this is an upper bound - honest, but not the whole truth. */
uint16_t swarm_avail_x10(void)
{
    /* The formula (and the only-our-5xx rule) lives in logic.h, shared with
     * the peer-derived figure below so they can never disagree.
     * Caveat: connections dropped before HTTP is parsed (socket pool full, TLS
     * handshake timed out) never reach a handler, so this remains an upper bound. */
    return bohun_avail_x10(s_req_count, s_req_err, s_req_err_srv);
}

void swarm_record_request(uint32_t lat_us)
{
    portENTER_CRITICAL(&s_count_mux);
    s_req_count++;
    s_req_lat_us = lat_us;
    s_lat_ring[s_lat_total % LAT_RING_N] = lat_us;
    s_lat_total++;
    portEXIT_CRITICAL(&s_count_mux);
}

/* A peer's 5xx count, recovered from err_kind[]: the httpd enum puts the
 * 5xx family at slots 0-2 (verified, IDF v6). Keeps self- and peer-computed
 * availability on ONE formula - they once disagreed under scanner noise. */
static uint32_t peer_err_srv(const uint16_t *err_kind)
{
    return (uint32_t)err_kind[0] + err_kind[1] + err_kind[2];
}

/* Availability from a peer's gossiped counters, on the SAME rule the node
 * applies to itself - one formula in logic.h, two feeds. */
static uint16_t peer_avail_x10(uint32_t req_count, uint32_t req_err,
                               const uint16_t *err_kind)
{
    return bohun_avail_x10(req_count, req_err, peer_err_srv(err_kind));
}

/* nearest-rank percentile over the last <=128 requests, computed on demand -
 * the roster is asked for a few times a minute, a 128-wide insertion sort is nothing */
static uint32_t lat_percentile_us(uint32_t pct)
{
    uint32_t cnt = s_lat_total < LAT_RING_N ? s_lat_total : LAT_RING_N;
    uint32_t w[LAT_RING_N];
    memcpy(w, s_lat_ring, cnt * sizeof(uint32_t));
    return bohun_pctl(w, cnt, pct);
}

/* callback context belongs to the radio - enqueue and leave */
static void swarm_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!s_rx_queue) {                           /* a peer frame beat our own init - drop it */
        return;
    }
    if (len < (int)offsetof(hb_frame_t, ip)) {   /* shorter than the v1 base - not ours */
        return;
    }
    /* Validate before spending a queue slot. The queue is 16 deep; without this,
     * anything able to put frames on our channel could keep it full and starve the
     * heartbeats that failover depends on. Magic + a 7-entry table walk is cheap. */
    if (memcmp(data, "ACLD", 4) != 0 || !swarm_member_by_mac(info->src_addr)) {
        s_rx_dropped++;
        return;
    }
    hb_event_t ev;
    memcpy(ev.src_mac, info->src_addr, 6);
    ev.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    memset(&ev.frame, 0, sizeof(ev.frame));      /* v1 senders leave ip = 0 */
    size_t copy = (size_t)len < sizeof(ev.frame) ? (size_t)len : sizeof(ev.frame);
    memcpy(&ev.frame, data, copy);
    /* A full queue means the monitor task is not draining - which would read as
     * pristine RF with rising per-peer "loss" if we swallowed it here. It is a
     * drop; count it as one. */
    if (xQueueSend(s_rx_queue, &ev, 0) != pdTRUE) {
        s_rx_dropped++;
    }
}

static void swarm_hb_tx_task(void *arg)
{
    const swarm_member_t *self = swarm_self();
    hb_frame_t frame = { .magic = {'A', 'C', 'L', 'D'}, .ver = 16, .node_id = self->id, .seq = 0 };
    uint32_t s_tcpip_beat_seen = 0, s_tcpip_stall = 0;
    frame.reset_reason = s_reset_reason;   /* fixed for this whole boot */
    frame.fw_ver = s_fw_ver;               /* likewise - the image cannot change under us */
    for (;;) {
        frame.seq++;
        s_self_seq = frame.seq;
        frame.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
        frame.ip = mask_get_ip();
        frame.generation = s_generation;
        frame.req_count = s_req_count;
        frame.req_lat_us = s_req_lat_us;
        frame.updating = s_updating ? 1 : 0;
        frame.update_pct = s_update_pct;
        frame.req_err = s_req_err;
        frame.link_up = mask_link_lost() ? 0 : 1;
        /* peers elect on THIS bit, so a build with no server must never
         * set it - the self-gate alone would leave the fleet electing an
         * impostor that then declines, and nobody wearing the mask. */
        /* mask_has_ip(): a blade that has dropped the mask and not yet been
         * given a lease has no address, so nothing can reach it - advertising
         * serving=1 there puts an unreachable blade in the backend pool. */
        frame.serving = (serve_is_ready() && serve_is_real() && mask_has_ip()) ? 1 : 0;
        memcpy(frame.err_kind, serve_err_counts(), sizeof(frame.err_kind));
        frame.free_heap = (uint32_t)esp_get_free_heap_size();
        frame.min_free_heap = (uint32_t)esp_get_minimum_free_heap_size();
        frame.temp_c10 = vitals_temp_c10();
        frame.req_p50_us = lat_percentile_us(50);
        frame.req_p95_us = lat_percentile_us(95);
        const uint32_t *ck = serve_click_counts();
        for (int k = 0; k < SERVE_CLICK_KINDS; k++) {
            frame.clicks[k] = ck[k] > 65535 ? 65535 : (uint16_t)ck[k];
        }
        /* TCPIP-THREAD WATCHDOG: count_pcbs_cb runs on the tcpip thread; if
         * its beat freezes ~12 s (link up, past boot settle) the thread is
         * hung and no connection will ever be accepted - record and reboot. */
        if (frame.uptime_s > 30 && !mask_link_lost()) {
            if (s_tcpip_beat == s_tcpip_beat_seen) {
                if (++s_tcpip_stall >= 12) {
                    blackbox_event(BBX_BOOT, (uint16_t)s_tcpip_stall,
                                   "tcpip thread wedged %lus - abort for coredump",
                                   (unsigned long)s_tcpip_stall);
                    /* abort(), NOT esp_restart(): a hung thread has recorded
                     * nothing - the panic writes every stack (including the
                     * frozen one) to the coredump partition. Read back:
                     *   curl -k https://<node>:8443/coredump > core.bin
                     *   espcoredump.py info_corefile -t raw -c core.bin <elf> */
                    ESP_LOGE(TAG, "tcpip thread wedged - aborting to capture a coredump");
                    abort();
                }
            } else {
                s_tcpip_stall = 0;
                s_tcpip_beat_seen = s_tcpip_beat;
            }
        }
        tcpip_callback(count_pcbs_cb, NULL);   /* refresh for the NEXT heartbeat */
        frame.pcb_active = s_pcb_active;
        frame.pcb_tw     = s_pcb_tw;
        frame.pcb_bound  = s_pcb_bound;
        frame.pcb_listen = s_pcb_listen;
        splice_stats_t sp;
        if (splice_stats_get(&sp)) {
            frame.lb_backends = sp.backends;
            frame.lb_known    = sp.known;
            frame.lb_benched  = sp.benched_mask;
            frame.lb_accepted = sp.accepted;
            frame.lb_done     = sp.completed;
            frame.lb_refused  = sp.refused;
            frame.lb_killed   = (uint16_t)sp.killed;
            frame.lb_idle     = (uint16_t)sp.timeouts;
            frame.lb_conn_ms  = (uint16_t)sp.p50_conn_ms;
            frame.lb_first_ms = (uint16_t)sp.p50_first_ms;
            frame.lb_total_ms = (uint16_t)sp.p50_total_ms;
            frame.lb_p95_ms   = (uint16_t)sp.p95_total_ms;
        }
        for (size_t i = 0; i < swarm_fleet_size(); i++) {
            const swarm_member_t *m = swarm_member(i);
            if (m->id == self->id) {
                continue;
            }
            esp_now_send(m->base_mac, (const uint8_t *)&frame, sizeof(frame));
        }
        vTaskDelay(pdMS_TO_TICKS(HB_PERIOD_MS));
    }
}

static roster_entry_t *roster_get(const swarm_member_t *m)
{
    for (size_t i = 0; i < s_roster_n; i++) {
        if (s_roster[i].member == m) {
            return &s_roster[i];
        }
    }
    if (s_roster_n < sizeof(s_roster) / sizeof(s_roster[0])) {
        s_roster[s_roster_n].member = m;
        /* static storage is zero-init, and 0 here would read as a plausible
         * 0.0 C from a node that has never sent a temperature at all. */
        s_roster[s_roster_n].last_temp_c10 = VITALS_TEMP_NONE;
        return &s_roster[s_roster_n++];
    }
    return NULL;
}

/* election: the rule itself is bohun_elect() in logic.h (lowest-id eligible
 * member that can actually serve; deterministic, preemptive, no coordinator).
 * Deliberately no stickiness: every node recomputes from the roster each tick
 * and the same inputs always give the same leader. This function only gathers
 * the candidates. */
static uint8_t s_leader_id;

/* Radio liveness alone qualifies nobody: a node whose Ethernet has died can
 * chat happily on ESP-NOW forever, keeping the mask while the site stays dark.
 * A candidate must hold up its end of the DATA plane too - link up, and a TLS
 * server actually listening. */
static uint8_t compute_leader(int64_t now)
{
    const swarm_member_t *self = swarm_self();
    const uint32_t now_ms = (uint32_t)(now / 1000);
    /* serve_is_real() must sit in this gate. Without it, firmware with no
     * server aboard answers the WEAK serve_is_ready() with true, sees no link
     * loss (its link was never up), self-nominates, and takes a mask it cannot
     * honour. */
    bool self_ok = self->eligible && serve_is_real()
                   && !mask_link_lost() && serve_is_ready();
    bohun_cand_t cand[sizeof(s_roster) / sizeof(s_roster[0])];
    size_t n = 0;
    for (size_t i = 0; i < s_roster_n; i++) {
        roster_entry_t *r = &s_roster[i];   /* monitor task owns the roster - no lock needed here */
        cand[n++] = (bohun_cand_t){
            .id = r->member->id,
            .eligible = r->member->eligible,
            .alive = peer_alive(r, now_ms),
            .able = r->last_link_up && r->last_serving,
        };
    }
    return bohun_elect(self->id, self_ok, cand, n);
}

/* the swarm's roles, in the Cossack lexicon */
static const char *role_word(const swarm_member_t *m)
{
    if (!m) return "unknown";
    if (m->id == s_leader_id) return "otaman";     /* the elected chief, wears the Bulava */
    if (m->eligible) return "kozak";               /* sworn brother of the host */
    return m->witness_role ? m->witness_role : "pysar";   /* scribe / bard */
}

static void swarm_monitor_task(void *arg)
{
    int64_t last_print = 0;
    for (;;) {
        /* One sampler for the whole node: the heartbeat and the roster JSON then
         * quote the SAME reading and can never disagree by a sample. Every 5th
         * tick = the 1 Hz the module documents; this loop runs at 200 ms and was
         * reading the sensor five times per second for no consumer. */
        if (++s_vitals_tick >= 5) {
            s_vitals_tick = 0;
            vitals_sample();
        }
        hb_event_t ev;
        while (xQueueReceive(s_rx_queue, &ev, 0) == pdTRUE) {
            if (memcmp(ev.frame.magic, "ACLD", 4) != 0 || ev.frame.ver < 1) {
                continue;
            }
            const swarm_member_t *m = swarm_member_by_mac(ev.src_mac);
            if (!m) {
                ESP_LOGW(TAG, "heartbeat from unledgered MAC %02x:%02x:%02x:%02x:%02x:%02x",
                         ev.src_mac[0], ev.src_mac[1], ev.src_mac[2],
                         ev.src_mac[3], ev.src_mac[4], ev.src_mac[5]);
                continue;
            }
            roster_entry_t *r = roster_get(m);
            if (r) {
                /* NOTHING that logs, blocks or takes a lock may sit inside
                 * this critical section: under portENTER interrupts are masked,
                 * xPortCanYield() is false, and picolibc treats taking stdout's
                 * recursive mutex there as an ISR-context error - instant
                 * abort(), a reboot loop at one boot per heartbeat. */
                uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
                bool joined = false;
                portENTER_CRITICAL(&s_roster_mux);
                if (r->rx_count == 0) {
                    joined = true;
                    r->first_seq = ev.frame.seq;
                    r->rx_win = 0;
                } else if (ev.frame.seq < r->last_seq) {   /* peer rebooted - restart delivery window */
                    r->first_seq = ev.frame.seq;
                    r->rx_win = 0;
                }
                r->last_seq = ev.frame.seq;
                r->last_uptime_s = ev.frame.uptime_s;
                r->last_rssi = ev.rssi;
                r->last_ip = ev.frame.ip;
                r->last_generation = ev.frame.generation;
                r->last_req_count = ev.frame.req_count;
                r->last_req_lat_us = ev.frame.req_lat_us;
                r->last_req_err = ev.frame.req_err;
                if (ev.frame.ver >= 7) {
                    r->last_link_up = ev.frame.link_up;
                    r->last_serving = ev.frame.serving;
                    memcpy(r->last_err_kind, ev.frame.err_kind, sizeof(r->last_err_kind));
                } else {
                    /* a node still on v6 cannot tell us; assume healthy so a rolling
                     * update never makes half the fleet ineligible mid-flight */
                    r->last_link_up = 1;
                    r->last_serving = 1;
                }
                if (ev.frame.ver >= 8) {
                    r->last_reset_reason   = ev.frame.reset_reason;
                    r->last_free_heap      = ev.frame.free_heap;
                    r->last_min_free_heap  = ev.frame.min_free_heap;
                }
                /* A v8 node has no thermometer field at all - the frame is
                 * literally shorter. Say UNKNOWN rather than let an uninitialised
                 * read look like a plausible 0.0 C during a rolling update. */
                if (ev.frame.ver >= 12) {
                    r->last_lb_known   = ev.frame.lb_known;
                    r->last_lb_benched = ev.frame.lb_benched;
                }
                if (ev.frame.ver >= 14) {
                    r->last_pcb_listen = ev.frame.pcb_listen;
                }
                r->last_fw_ver = (ev.frame.ver >= 15) ? ev.frame.fw_ver : 0;
                if (ev.frame.ver >= 16) {
                    memcpy(r->last_clicks, ev.frame.clicks, sizeof(r->last_clicks));
                } else {
                    memset(r->last_clicks, 0, sizeof(r->last_clicks));
                }
                if (ev.frame.ver >= 13) {
                    r->last_pcb_active = ev.frame.pcb_active;
                    r->last_pcb_tw     = ev.frame.pcb_tw;
                    r->last_pcb_bound  = ev.frame.pcb_bound;
                }
                if (ev.frame.ver >= 11) {
                    r->last_p50_us = ev.frame.req_p50_us;
                    r->last_p95_us = ev.frame.req_p95_us;
                }
                if (ev.frame.ver >= 10) {
                    r->last_lb_backends = ev.frame.lb_backends;
                    r->last_lb_accepted = ev.frame.lb_accepted;
                    r->last_lb_done     = ev.frame.lb_done;
                    r->last_lb_refused  = ev.frame.lb_refused;
                    r->last_lb_killed   = ev.frame.lb_killed;
                    r->last_lb_idle     = ev.frame.lb_idle;
                    r->last_lb_conn_ms  = ev.frame.lb_conn_ms;
                    r->last_lb_first_ms = ev.frame.lb_first_ms;
                    r->last_lb_total_ms = ev.frame.lb_total_ms;
                    r->last_lb_p95_ms   = ev.frame.lb_p95_ms;
                }
                r->last_temp_c10 = (ev.frame.ver >= 9) ? ev.frame.temp_c10
                                                       : VITALS_TEMP_NONE;
                /* a v7 node leaves these zero, which reads as UNKNOWN / no heap
                 * figure - honest, and it keeps a rolling update from inventing
                 * a heap crisis on nodes that simply have not been flashed yet */
                r->last_updating = ev.frame.updating;
                r->last_update_pct = ev.frame.update_pct;
                r->rx_win++;
                r->rx_count++;
                /* last_seen is the PUBLISH gate: snapshot readers compute alive
                 * from it with no lock, so it must move only after every payload
                 * field above is in place. When it led this block, a reader could
                 * catch a just-revived peer alive-but-half-written - which is how
                 * a v8 node's first post-reboot heartbeat once chronicled as
                 * "REBOOTED: UNKNOWN" with the real reason one line away. */
                r->last_seen_ms = now_ms;
                portEXIT_CRITICAL(&s_roster_mux);
                /* outside the roster lock on purpose: the tail has its own */
                rtail_push(m->id, ev.frame.seq, ev.rssi,
                           ev.frame.ver >= 7 ? ev.frame.serving : true, now_ms);
                if (joined) {
                    ESP_LOGI(TAG, "peer JOINED: %s (rssi %d)", m->name, ev.rssi);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));   /* election/mask run every tick, not gated on queue */
        int64_t now = esp_timer_get_time();

        /* the wire died under us while wearing it: stop pretending, immediately */
        if (mask_is_wearing() && mask_link_lost()) {
            blackbox_event(BBX_MASK_OFF, 0, "link lost - abdicated");
            mask_abdicate();
            s_leader_id = 0;   /* force a real election on the next tick */
        }

        uint8_t leader = compute_leader(now);
        if (leader != s_leader_id) {
            s_leader_id = leader;
            s_generation++;
            const swarm_member_t *self = swarm_self();
            if (leader == 0) {
                ESP_LOGW(TAG, "election: NO otaman (no eligible member alive)");
            } else if (leader == self->id) {
                s_req_at_claim = s_req_count;   /* a fresh reign counts from zero */
                s_times_elected++;
                s_elected_dirty = true;   /* persisted lazily - see below */
                blackbox_event(BBX_MASK_ON, (uint16_t)s_generation, "gen %" PRIu32, s_generation);
                ESP_LOGW(TAG, "election: I AM THE OTAMAN (bohun-n%u, elected %" PRIu32 "x)",
                         leader, s_times_elected);
            } else {
                if (mask_is_wearing()) {
                    blackbox_event(BBX_MASK_OFF, (uint16_t)s_generation, "lost to n%u", leader);
                }
                ESP_LOGW(TAG, "election: otaman = bohun-n%u (I am a %s)", leader,
                         self->eligible ? "kozak" : "pysar");
            }
            mask_set_leader(leader == self->id);
        }
        mask_tick();

        /* Persist the election count at most once a minute, not once per
         * election: an abdication flap can claim the mask hundreds of times in
         * minutes, and committing each one would chew through an NVS sector.
         * The counter lives in RAM; a reboot inside the window loses at most a
         * minute of it - a fair trade for the flash. */
        if (s_elected_dirty && s_nvs &&
            now - s_elected_saved_us > (int64_t)ELECTED_SAVE_MS * 1000) {
            nvs_set_u32(s_nvs, "elected", s_times_elected);
            nvs_commit(s_nvs);
            s_elected_dirty = false;
            s_elected_saved_us = now;
        }

        if (now - last_print >= (int64_t)ROSTER_PRINT_MS * 1000) {
            last_print = now;
            const swarm_member_t *self = swarm_self();
            ESP_LOGI(TAG, "roster [leader=bohun-n%u, self=%s %s]:", s_leader_id,
                     self ? self->name : "UNLEDGERED", self ? role_word(self) : "-");
            for (size_t i = 0; i < s_roster_n; i++) {
                roster_entry_t *r = &s_roster[i];
                bool alive = peer_alive(r, (uint32_t)(now / 1000));
                esp_ip4_addr_t ip4 = { .addr = r->last_ip };
                ESP_LOGI(TAG, "roster: %s %s ip=" IPSTR " seq=%" PRIu32 " up=%" PRIu32 "s rssi=%d rx=%" PRIu32,
                         r->member->name, alive ? "ALIVE" : "LOST", IP2STR(&ip4),
                         r->last_seq, r->last_uptime_s, r->last_rssi, r->rx_count);
            }
        }
    }
}

/* observability: the node's live view of the swarm as JSON (served at /roster).
 * self + the peers it hears; benign startup-only race on s_roster (grows to
 * fleet size then stops), acceptable for a display endpoint. */
int swarm_roster_json(char *buf, size_t len)
{
    return swarm_roster_json_ex(buf, len, false);
}

/* "42.3", or JSON null when the node never said. null is the only honest
 * rendering of "unknown" - 0.0 is a plausible-looking lie, and a consumer can
 * test for null. Sign is handled explicitly because -0.5 has a zero whole part
 * and would otherwise print as "0.5". */
static void temp_json(int16_t c10, char *out, size_t n)
{
    if (c10 == VITALS_TEMP_NONE) {
        snprintf(out, n, "null");
        return;
    }
    int w = c10 / 10, f = (c10 < 0 ? -c10 : c10) % 10;
    snprintf(out, n, "%s%d.%d", (c10 < 0 && w == 0) ? "-" : "", w, f);
}

void swarm_lb_view(swarm_lb_view_t *out)
{
    memset(out, 0, sizeof(*out));
    splice_stats_t sp;
    if (mask_is_wearing() && splice_stats_get(&sp)) {
        out->valid = true;  out->live = true;
        out->backends = sp.backends;       out->known = sp.known;
        out->benched_mask = sp.benched_mask;
        out->accepted = sp.accepted;       out->done = sp.completed;
        out->refused = sp.refused;         out->killed = sp.killed;
        out->idle_kills = sp.timeouts;
        out->conn_ms = sp.p50_conn_ms;     out->first_ms = sp.p50_first_ms;
        out->total_ms = sp.p50_total_ms;   out->p95_ms = sp.p95_total_ms;
        return;
    }
    for (size_t i = 0; i < s_roster_n; i++) {
        roster_entry_t snap;
        portENTER_CRITICAL(&s_roster_mux);
        snap = s_roster[i];
        portEXIT_CRITICAL(&s_roster_mux);
        if (snap.member->id == s_leader_id && snap.last_lb_backends) {
            out->valid = true;
            out->backends = snap.last_lb_backends;  out->known = snap.last_lb_known;
            out->benched_mask = snap.last_lb_benched;
            out->accepted = snap.last_lb_accepted;  out->done = snap.last_lb_done;
            out->refused = snap.last_lb_refused;    out->killed = snap.last_lb_killed;
            out->idle_kills = snap.last_lb_idle;
            out->conn_ms = snap.last_lb_conn_ms;    out->first_ms = snap.last_lb_first_ms;
            out->total_ms = snap.last_lb_total_ms;  out->p95_ms = snap.last_lb_p95_ms;
            return;
        }
    }
}

/* with_ip belongs to the ADMIN plane only (port 8443, never forwarded). Every node
 * already gossips its DHCP address in the heartbeat, so the swarm has always known
 * where everyone is - we simply refuse to tell the public. Exposing it here means
 * tooling can ask the fleet where its members are instead of sweeping the subnet,
 * which matters because follower leases rotate daily. */
int swarm_roster_json_ex(char *buf, size_t len, bool with_ip)
{
    const swarm_member_t *self = swarm_self();
    if (!self) {
        /* eFuse MAC absent from FLEET[] (a replacement board). swarm_start() bailed,
         * but serve_start() already ran and /roster is live - answer, do not panic. */
        return snprintf(buf, len, "{\"err\":\"unledgered board\"}\n");
    }
    int64_t now = esp_timer_get_time();
    const char *leader_name = "none";
    for (size_t i = 0; i < swarm_fleet_size(); i++) {
        if (swarm_member(i)->id == s_leader_id) { leader_name = swarm_member(i)->name; break; }
    }
    size_t n = 0;
#define RAPP(...) do { if (n < len) { int _w = snprintf(buf + n, len - n, __VA_ARGS__); \
    if (_w > 0) n = (n + (size_t)_w > len) ? len : n + (size_t)_w; } } while (0)
    /* The lb block is top-level because the page is served by a BACKEND:
     * the reader needs the OTAMAN's numbers - own splicer if we lead, else
     * what the leader gossiped. The door they came through, not the room. */
    {
        splice_stats_t sp;
        bool have = false;
        if (mask_is_wearing() && splice_stats_get(&sp)) {
            have = true;
        } else {
            for (size_t i = 0; i < s_roster_n; i++) {
                roster_entry_t lsnap;
                portENTER_CRITICAL(&s_roster_mux);
                lsnap = s_roster[i];
                portEXIT_CRITICAL(&s_roster_mux);
                if (lsnap.member->id == s_leader_id && lsnap.last_lb_backends) {
                    sp.backends     = lsnap.last_lb_backends;
                    sp.known        = lsnap.last_lb_known;
                    sp.benched_mask = lsnap.last_lb_benched;
                    sp.accepted     = lsnap.last_lb_accepted;
                    sp.completed    = lsnap.last_lb_done;
                    sp.refused      = lsnap.last_lb_refused;
                    sp.killed       = lsnap.last_lb_killed;
                    sp.timeouts     = lsnap.last_lb_idle;
                    sp.p50_conn_ms  = lsnap.last_lb_conn_ms;
                    sp.p50_first_ms = lsnap.last_lb_first_ms;
                    sp.p50_total_ms = lsnap.last_lb_total_ms;
                    sp.p95_total_ms = lsnap.last_lb_p95_ms;
                    have = true;
                    break;
                }
            }
        }
        if (have) {
            RAPP("{\"lb\":{\"backends\":%u,\"known\":%u,\"benched\":%u,\"accepted\":%" PRIu32 ",\"done\":%" PRIu32 ","
                 "\"refused\":%" PRIu32 ",\"killed\":%" PRIu32 ",\"idle_kills\":%" PRIu32 ","
                 "\"conn_ms\":%u,\"first_ms\":%u,\"total_ms\":%u,\"p95_ms\":%u},",
                 sp.backends, sp.known, sp.benched_mask, sp.accepted, sp.completed, sp.refused, sp.killed,
                 sp.timeouts, (unsigned)sp.p50_conn_ms, (unsigned)sp.p50_first_ms,
                 (unsigned)sp.p50_total_ms, (unsigned)sp.p95_total_ms);
        } else {
            RAPP("{");
        }
    }
    RAPP("\"self\":\"%s\",\"id\":%u,\"role\":\"%s\",\"eligible\":%d,\"wearing\":%d,"
         "\"roster_n\":%u,\"leader\":\"%s\",\"members\":[",
         self->name, self->id,
         role_word(self),
         self->eligible ? 1 : 0, mask_is_wearing() ? 1 : 0, (unsigned)s_roster_n,
         leader_name);
    /* temp is PUBLIC: a die temperature identifies no address. Emitted once,
     * here - admin readers see it too, since their document is this body plus
     * the private fields. ~14 bytes x 6 members against the 3 KB buffer. */
    char tj_self[12];
    temp_json(vitals_temp_c10(), tj_self, sizeof(tj_self));
    RAPP("{\"name\":\"%s\",\"id\":%u,\"eligible\":%d,\"self\":true,\"state\":\"alive\",\"role\":\"%s\","
         "\"seq\":%" PRIu32 ",\"uptime_s\":%" PRIu32 ",\"rssi\":0,\"rx\":0,"
         "\"generation\":%" PRIu32 ",\"req_count\":%" PRIu32 ",\"req_lat_us\":%" PRIu32 ","
         "\"req_since_el\":%" PRIu32 ",\"elected\":%" PRIu32 ","
         "\"req_err\":%" PRIu32 ",\"req_err_srv\":%" PRIu32 ",\"avail_x10\":%u,\"rf_dropped\":%" PRIu32 ","
         "\"link_up\":%d,\"serving\":%d,\"temp_c\":%s,"
         "\"p50_lat_us\":%" PRIu32 ",\"p95_lat_us\":%" PRIu32 ",\"delivery\":100,\"loss\":0}",
         self->name, self->id, self->eligible ? 1 : 0,
         role_word(self),
         s_self_seq, (uint32_t)(now / 1000000),
         s_generation, s_req_count, s_req_lat_us, s_req_count - s_req_at_claim,
         s_times_elected, s_req_err, s_req_err_srv, (unsigned)swarm_avail_x10(), s_rx_dropped,
         mask_link_up() ? 1 : 0, serve_is_ready() ? 1 : 0, tj_self,
         lat_percentile_us(50), lat_percentile_us(95));
    /* Boot cause and heap are ADMIN-PLANE ONLY, alongside the addresses. Two
     * reasons, and the second is the one that matters: the public buffer is 2 KB
     * for six members and RAPP truncates silently, so widening every entry there
     * would quietly emit invalid JSON to the page's own fetch. The kobzar does not
     * need them over HTTP anyway - it gets them in the heartbeat. */
    if (with_ip) {
        esp_ip4_addr_t me = { .addr = mask_get_ip() };
        n -= 1;   /* step back over the closing brace and add the private fields */
        /* on_trial: D8 says this image is still unproven and the next reboot
         * would roll it back. deploy.sh polls this instead of sleeping blind. */
        RAPP(",\"ip\":\"" IPSTR "\",\"boot\":\"%s\",\"on_trial\":%s,"
             "\"free_heap\":%" PRIu32 ",\"min_free_heap\":%" PRIu32 ","
             "\"pcb_active\":%u,\"pcb_tw\":%u,\"pcb_bound\":%u,\"pcb_listen\":%u}",
             IP2STR(&me), swarm_reset_reason_str(s_reset_reason),
             selftest_on_trial() ? "true" : "false",
             (uint32_t)esp_get_free_heap_size(), (uint32_t)esp_get_minimum_free_heap_size(),
             s_pcb_active, s_pcb_tw, s_pcb_bound, s_pcb_listen);
        /* if this node splices, its ledger is admin-visible too - the
         * headless N4 testbed's only window once it leaves the USB bench */
        splice_stats_t sp;
        if (splice_stats_get(&sp)) {
            n -= 1;
            RAPP(",\"splice\":{\"backends\":%u,\"busy\":%u,\"accepted\":%" PRIu32 ","
                 "\"done\":%" PRIu32 ",\"refused\":%" PRIu32 ",\"killed\":%" PRIu32 ","
                 "\"idle_kills\":%" PRIu32 ",\"conn_ms\":%u,\"first_ms\":%u,"
                 "\"total_ms\":%u,\"total_p95_ms\":%u}}",
                 sp.backends, sp.busy, sp.accepted, sp.completed, sp.refused,
                 sp.killed, sp.timeouts, (unsigned)sp.p50_conn_ms,
                 (unsigned)sp.p50_first_ms, (unsigned)sp.p50_total_ms,
                 (unsigned)sp.p95_total_ms);
        }
    }
    const uint32_t now_ms = (uint32_t)(now / 1000);
    for (size_t i = 0; i < s_roster_n; i++) {
        /* a consistent copy, same as swarm_snapshot: the monitor writes ~20
         * fields per entry under this lock, and a JSON render racing it could
         * otherwise quote a half-updated peer */
        roster_entry_t snap;
        portENTER_CRITICAL(&s_roster_mux);
        snap = s_roster[i];
        portEXIT_CRITICAL(&s_roster_mux);
        roster_entry_t *r = &snap;
        bool alive = peer_alive(r, now_ms);
        uint32_t deliv = bohun_delivery_pct(r->first_seq, r->last_seq, r->rx_win);
        unsigned ravail = peer_avail_x10(r->last_req_count, r->last_req_err,
                                         r->last_err_kind);
        char tj[12];
        temp_json(r->last_temp_c10, tj, sizeof(tj));
        RAPP(",{\"name\":\"%s\",\"id\":%u,\"eligible\":%d,\"self\":false,\"state\":\"%s\","
             "\"seq\":%" PRIu32 ",\"uptime_s\":%" PRIu32 ",\"rssi\":%d,\"rx\":%" PRIu32 ","
             "\"generation\":%" PRIu32 ",\"req_count\":%" PRIu32 ",\"req_lat_us\":%" PRIu32 ","
             "\"req_err\":%" PRIu32 ",\"avail_x10\":%u,\"link_up\":%d,\"serving\":%d,\"temp_c\":%s,"
             "\"role\":\"%s\",\"updating\":%d,\"update_pct\":%u,\"delivery\":%" PRIu32 ",\"loss\":%" PRIu32 ","
             "\"p50_lat_us\":%" PRIu32 ",\"p95_lat_us\":%" PRIu32 ","
             "\"pcb_active\":%u,\"pcb_tw\":%u,\"pcb_bound\":%u,\"pcb_listen\":%u}",
             r->member->name, r->member->id, r->member->eligible ? 1 : 0,
             alive ? "alive" : "lost", r->last_seq, r->last_uptime_s, r->last_rssi, r->rx_count,
             r->last_generation, r->last_req_count, r->last_req_lat_us,
             r->last_req_err, ravail, r->last_link_up ? 1 : 0, r->last_serving ? 1 : 0, tj,
             role_word(r->member), r->last_updating ? 1 : 0, r->last_update_pct, deliv, 100 - deliv,
             r->last_p50_us, r->last_p95_us,
             r->last_pcb_active, r->last_pcb_tw, r->last_pcb_bound, r->last_pcb_listen);
        if (with_ip) {
            esp_ip4_addr_t pip = { .addr = r->last_ip };
            n -= 1;
            RAPP(",\"ip\":\"" IPSTR "\",\"boot\":\"%s\","
                 "\"free_heap\":%" PRIu32 ",\"min_free_heap\":%" PRIu32 "}",
                 IP2STR(&pip), swarm_reset_reason_str(r->last_reset_reason),
                 r->last_free_heap, r->last_min_free_heap);
        }
    }
    RAPP("]}\n");
#undef RAPP
    /* RAPP clamps silently; bohun_json_close_clamped (logic.h) walks back to
     * the last complete member object and closes the document so a truncated
     * roster still parses. */
    if (n >= len) {
        size_t fixed = bohun_json_close_clamped(buf, len);
        if (fixed) {
            n = fixed;
            ESP_LOGW(TAG, "roster JSON clamped at %u bytes - members omitted",
                     (unsigned)len);
        }
    }
    return (int)n;
}

int swarm_snapshot(swarm_snap_entry_t *e, int max, uint8_t *leader_out)
{
    const swarm_member_t *self = swarm_self();
    int64_t now = esp_timer_get_time();
    int n = 0;
    if (leader_out) {
        *leader_out = s_leader_id;
    }
    if (self && n < max) {
        e[n] = (swarm_snap_entry_t){ .id = self->id, .name = self->name, .shortname = self->shortname, .alive = true, .self = true,
            .leader = (s_leader_id == self->id), .eligible = self->eligible, .ip = mask_get_ip(),
            .uptime_s = (uint32_t)(now / 1000000), .rssi = 0, .generation = s_generation, .fw_ver = s_fw_ver,
            .req_count = s_req_count, .req_lat_us = s_req_lat_us, .req_err = s_req_err, .avail_x10 = swarm_avail_x10(),
            .link_up = !mask_link_lost(), .serving = serve_is_ready(),
            .role = role_word(self), .updating = s_updating, .update_pct = s_update_pct, .loss_pct = 0,
            .reset_reason = s_reset_reason,
            .temp_c10 = vitals_temp_c10(),
            .p50_us = lat_percentile_us(50), .p95_us = lat_percentile_us(95),
            .free_heap = (uint32_t)esp_get_free_heap_size(),
            .min_free_heap = (uint32_t)esp_get_minimum_free_heap_size() };
        memcpy(e[n].err_kind, serve_err_counts(), sizeof(e[n].err_kind));
        const uint32_t *ck = serve_click_counts();
        for (int k = 0; k < SERVE_CLICK_KINDS; k++) {
            e[n].clicks[k] = ck[k] > 65535 ? 65535 : (uint16_t)ck[k];
        }
        n++;
    }
    for (size_t i = 0; i < s_roster_n && n < max; i++) {
        /* take a CONSISTENT copy: the monitor writes these ~20 fields under the
         * same lock, so a render task can no longer catch an entry half-updated
         * (the chronicle's stale-reset-reason bug) */
        roster_entry_t snap;
        portENTER_CRITICAL(&s_roster_mux);
        snap = s_roster[i];
        portEXIT_CRITICAL(&s_roster_mux);
        roster_entry_t *r = &snap;
        e[n] = (swarm_snap_entry_t){ .id = r->member->id, .name = r->member->name, .shortname = r->member->shortname,
            .alive = peer_alive(r, (uint32_t)(now / 1000)), .self = false,
            .leader = (s_leader_id == r->member->id), .eligible = r->member->eligible, .ip = r->last_ip,
            .uptime_s = r->last_uptime_s, .rssi = r->last_rssi, .generation = r->last_generation, .fw_ver = r->last_fw_ver,
            .req_count = r->last_req_count, .req_lat_us = r->last_req_lat_us,
            .req_err = r->last_req_err,
            .avail_x10 = peer_avail_x10(r->last_req_count, r->last_req_err, r->last_err_kind),
            .link_up = r->last_link_up, .serving = r->last_serving,
            .role = role_word(r->member), .updating = r->last_updating, .update_pct = r->last_update_pct,
            .loss_pct = (uint8_t)(100 - bohun_delivery_pct(r->first_seq, r->last_seq, r->rx_win)),
            .reset_reason = r->last_reset_reason,
            .temp_c10 = r->last_temp_c10,
            .p50_us = r->last_p50_us, .p95_us = r->last_p95_us,
            .free_heap = r->last_free_heap,
            .min_free_heap = r->last_min_free_heap };
        memcpy(e[n].err_kind, r->last_err_kind, sizeof(e[n].err_kind));
        memcpy(e[n].clicks, r->last_clicks, sizeof(e[n].clicks));
        n++;
    }
    return n;
}

void swarm_start(void)
{
    /* FIRST thing, before anything can obscure it: why did we just die?
     * The value is valid for this boot only, so it has to be latched now. */
    s_reset_reason = (uint8_t)esp_reset_reason();
    s_fw_ver = (uint16_t)atoi(esp_app_get_description()->version);
    ESP_LOGW(TAG, "boot cause: %s (esp_reset_reason %u), free heap %u B",
             swarm_reset_reason_str(s_reset_reason), (unsigned)s_reset_reason,
             (unsigned)esp_get_free_heap_size());

    /* lifetime election count survives reboots (eth_main inits NVS before us) */
    if (nvs_open("swarm", NVS_READWRITE, &s_nvs) == ESP_OK) {
        nvs_get_u32(s_nvs, "elected", &s_times_elected);
    }
    const swarm_member_t *self = swarm_self();
    if (!self) {
        ESP_LOGE(TAG, "this board is not in the fleet ledger - swarm layer NOT started");
        return;
    }
    ESP_LOGI(TAG, "swarm identity: %s (id %u)", self->name, self->id);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(SWARM_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));   /* radio always listening - heartbeats must not nap */

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_set_pmk(PMK));
    for (size_t i = 0; i < swarm_fleet_size(); i++) {
        const swarm_member_t *m = swarm_member(i);
        if (m->id == self->id) {
            continue;
        }
        esp_now_peer_info_t peer = { .channel = SWARM_CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = true };
        memcpy(peer.peer_addr, m->base_mac, 6);
        memcpy(peer.lmk, LMK, 16);
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    }


    /* queue BEFORE the callback: a peer frame arriving in between would have hit
     * xQueueSend on a NULL handle and panicked a blade rebooting into a live fleet */
    s_rx_queue = xQueueCreate(16, sizeof(hb_event_t));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(swarm_recv_cb));
    xTaskCreate(swarm_monitor_task, "swarm_monitor", 4096, NULL, 5, NULL);
    xTaskCreate(swarm_hb_tx_task, "swarm_hb_tx", 3072, NULL, 6, NULL);
    ESP_LOGI(TAG, "control plane up: ch %d, %u peers, encrypted, hb %d ms",
             SWARM_CHANNEL, (unsigned)(swarm_fleet_size() - 1), HB_PERIOD_MS);
}
