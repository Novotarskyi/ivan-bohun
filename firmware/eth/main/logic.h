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
