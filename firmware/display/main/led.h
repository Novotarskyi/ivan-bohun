#pragma once
#include "swarm.h"

/* The SK6812 RGBW rail: six pixels chained off GPIO4 through the 74AHCT125
 * shifter. px0..px3 = N1..N4, px4 = the pysar's own pixel (deepest blue while
 * heartbeats arrive, red in silence), px5 = the kobzar. The colour law lives
 * in led.c's header comment.
 *
 * The rail has six pixels for six members; a ledger id with no pixel (a shelf
 * spare joining the radio) would be invisible here.
 * Electrically a harmless no-op if the rail is not wired. */
void led_init(void);      /* driver + the four-colour boot parade */
void led_render(const swarm_snap_entry_t *e, int n, uint8_t leader_id);
typedef struct {
    bool up;              /* driver initialised */
    bool dma;             /* RMT DMA active (false = fallback) */
    bool blackout;        /* maintenance blackout latched */
    uint8_t rgb[6][3];    /* what each pixel currently shows (w folded in) */
} led_status_t;
void led_get_status(led_status_t *out);

void led_blackout(void);     /* rail dark + held dark (maintenance) */
void led_ota(bool active);   /* OTA window: whole rail slow-blinks amber */
void led_wake(void);         /* resume normal rendering */
