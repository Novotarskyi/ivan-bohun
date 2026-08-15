/* splice.h - the otaman's splicer.
 *
 * An L4 TCP splicer that balances the public port across every OTHER
 * alive+serving blade, fed by the swarm roster. TLS terminates on the
 * backends; this module never parses a byte it moves.
 *
 * EVERY blade runs it, permanently: it binds 0.0.0.0 on the public port, but
 * only the blade wearing the mask holds the address the router forwards to -
 * so the mask alone decides who receives public traffic, and a leadership
 * change moves the load with zero listener churn. CONFIG_BOHUN_SPLICE_TEST
 * adds per-connection receipts and a heap trap, for bench instrumentation.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool     active;         /* the splicer task is up and owns its port */
    uint8_t  backends;       /* blades it can ACTUALLY route to right now (benched excluded) */
    uint8_t  known;          /* blades in the table, healthy or not */
    uint8_t  benched_mask;   /* bit N set = node id N is benched (measured dead) */
    uint8_t  busy;           /* pairs in flight right now */
    uint32_t accepted;
    uint32_t completed;      /* one side closed and every buffered byte was flushed - a clean end */
    uint32_t refused;        /* no capacity / backend connect failed */
    uint32_t killed;         /* error mid-splice */
    uint32_t timeouts;       /* idle pairs reaped */
    /* WHERE THE TIME GOES, per completed pair (medians over the last 32).
     * Phase-split on purpose: "single connections fast, concurrent ones slow"
     * cannot be diagnosed without separating connect, first byte and total. */
    uint32_t p50_conn_ms;    /* accept -> backend TCP established (banked at first byte) */
    uint32_t p50_first_ms;   /* accept -> first byte back from the backend (ditto) */
    /* accept -> pair closed. NOT a response time: a keep-alive browser holds one
     * connection open across the page, its favicon and every roster poll, so
     * most of this is a visitor sitting still. It is a connection LIFETIME, and
     * the page labels it as one. An L4 splicer cannot see request boundaries. */
    uint32_t p50_total_ms;
    uint32_t p95_total_ms;
} splice_stats_t;

void splice_start(void);

/* Is the splicer bound and accepting? This is a SERVING-PATH health question:
 * on a balancing fleet the public port is the splicer's, so a node whose
 * splicer never bound cannot serve the public even with a perfect httpd.
 * D8's trial asks this before marking an image permanent. Weak-stubbed in
 * swarm.c for builds without a splicer. */
bool splice_is_up(void);

/* Called just before this node gives up the mask. Every in-flight pair is about
 * to be severed anyway - the interface goes down and the address changes - so
 * close them cleanly rather than letting them die as resets. Asynchronous by
 * contract: this requests the drain and waits briefly; the splice task itself
 * closes the fds, because only the task that selects on them may close them. */
void splice_drain(const char *why);

/* false = this build has no splicer (observers) or it never started.
 * swarm.c carries a weak stub so observer builds keep linking - the same
 * idiom as vitals/serve. */
bool splice_stats_get(splice_stats_t *out);
