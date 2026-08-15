#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "serve.h"   /* SERVE_ERR_KINDS - the per-code error tally shared with observers */

void swarm_start(void);
int  swarm_roster_json(char *buf, size_t len);   /* public: no addresses, ever */
/* admin plane (8443, never forwarded): same view WITH each node's DHCP address.
 * The swarm already gossips these in the heartbeat - this just stops tooling from
 * having to sweep the subnet to rediscover what the fleet already knows. */
int  swarm_roster_json_ex(char *buf, size_t len, bool with_ip);
void swarm_record_request(uint32_t lat_us);   /* serve.c pushes per-request timing (a 2xx/3xx served) */
void swarm_record_refusal(bool our_fault);     /* serve.c: a non-2xx. our_fault = 5xx */
void swarm_record_error(void);                 /* deprecated alias - counts as theirs */
uint16_t swarm_avail_x10(void);               /* availability in tenths of a % (1000 = 100.0%) */

/* Wipe every counter this node keeps, including the NVS-persisted election tally.
 * Exists so the fleet can start its public life from zero rather than carrying a
 * fortnight of bench noise. */
void swarm_reset_stats(void);
void swarm_set_ota(bool on);                   /* ota.c flags an in-progress release */
void swarm_set_ota_progress(uint8_t pct);      /* ota.c reports image received, 0-100 */
bool    swarm_own_updating(void);              /* our own OTA-in-progress flag */
uint8_t swarm_own_update_pct(void);
int     swarm_peers_heard(void);               /* peers currently alive on the radio */
void    swarm_repin_channel(void);             /* restore the pinned ESP-NOW channel */

/* the mask's fixed identity, for the display to show */
/* ONE definition of the mask, in both shapes callers need. mask.c held the
 * bytes and swarm.h the string; changing one silently left the other lying. */
#define SWARM_VMAC_BYTES { 0x02, 0x41, 0x43, 0x4c, 0x44, 0x01 }
#define SWARM_VMAC_STR "02:41:43:4c:44:01"

typedef struct {
    uint8_t id;
    const char *name;
    const char *shortname;   /* for narrow glass */
    bool alive;
    bool self;
    bool leader;
    bool eligible;
    uint32_t ip;          /* esp_ip4 .addr */
    uint32_t uptime_s;
    int8_t rssi;
    uint32_t generation;
    uint32_t req_count;
    uint32_t req_lat_us;
    uint32_t req_err;     /* every non-2xx (theirs + ours) - the refusal ledger */
    uint32_t req_err_srv; /* the 5xx subset: the only ones that are OUR failure */
    uint16_t avail_x10;   /* tenths of a %: (served + their-fault refusals)
                           * over everything asked. Only OUR 5xx count against
                           * it - a 404 to a scanner is a correct answer. Peers
                           * derive the same figure from err_kind[0..2]. */
    bool link_up;         /* Ethernet is up (not merely the radio) */
    bool serving;         /* the TLS server is listening - if false, this node is BOOTING */
    uint16_t err_kind[SERVE_ERR_KINDS];   /* which errors, for the kobzar's table */
    const char *role;     /* "otaman" / "kozak" / "pysar" */
    bool updating;        /* this node is receiving an OTA right now */
    uint8_t update_pct;   /* OTA image received, 0-100 */
    uint8_t loss_pct;     /* heartbeat loss %% (0 = perfect delivery) */
    /* v8 - why this node last died, and how close it is to dying again.
     * Without this a fleet-wide reboot is unexplainable: any panic goes out a
     * UART with no listener, and nothing else records the cause. */
    uint8_t reset_reason;    /* esp_reset_reason() from this node's CURRENT boot */
    uint32_t free_heap;      /* bytes free right now */
    uint32_t min_free_heap;  /* low-water mark since boot - this is the leak detector:
                              * a leak walks it down over hours, a brownout leaves it
                              * flat and healthy right up to the instant of death */
    /* v9 - die temperature, tenths of a degree C, VITALS_TEMP_NONE if this
     * node did not say. The instrument behind the rack's thermal design - DIE
     * temperature, not air, so only the differences mean anything. */
    int16_t temp_c10;
    /* v11 - the node's own request percentiles, for the observers' fleet-wide
     * latency view. 0 = never served (or a pre-v11 image): render as "-". */
    uint32_t p50_us, p95_us;
    /* v15 - the node's firmware version (its variant's version.txt integer).
     * 0 = a pre-v15 image: render as "-", never as a plausible v0. */
    uint16_t fw_ver;
    /* v16 - the conversion ledger, serve.h's canonical slug order */
    uint16_t clicks[SERVE_CLICK_KINDS];
} swarm_snap_entry_t;

/* THE OTAMAN'S LEDGER, as seen from anywhere. On the leader it is read live
 * from the splicer; everywhere else (including both observers) it is whatever
 * the leader last gossiped (v10/v12). valid=false means nobody is balancing
 * or the leader is still on a pre-v10 image. */
typedef struct {
    bool     valid;
    bool     live;           /* true = our own splicer, not gossip */
    uint8_t  backends, known;
    uint8_t  benched_mask;   /* bit (id & 7) set = that blade is on the bench */
    uint32_t accepted, done, refused;
    uint16_t killed, idle_kills;
    uint16_t conn_ms, first_ms, total_ms, p95_ms;
} swarm_lb_view_t;
void swarm_lb_view(swarm_lb_view_t *out);

/* THE RADIO TAIL - every heartbeat heard, newest first, for the kobzar's
 * gossip page. gap = heartbeats missed from that sender since the previous
 * one heard (seq arithmetic, capped at 99). Ring covers the last ~45 s of a
 * six-member fleet at 1 Hz. */
typedef struct {
    uint32_t ms;       /* esp_timer ms at reception */
    uint32_t seq;
    uint8_t  id;
    int8_t   rssi;
    uint8_t  gap;
    bool     serving;
} swarm_radio_evt_t;
int swarm_radio_tail(swarm_radio_evt_t *out, int max);   /* newest first */

/* short uppercase name for an esp_reset_reason_t, for the kobzar's chronicle */
const char *swarm_reset_reason_str(uint8_t r);

/* fill entries[] (up to max) with the full fleet view (self first, then peers).
 * *leader_id_out = current leader id (0 if none). returns entry count. */
int swarm_snapshot(swarm_snap_entry_t *entries, int max, uint8_t *leader_id_out);
