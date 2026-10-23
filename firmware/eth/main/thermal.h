/* thermal.h - the fleet's day of die temperatures, as THIS blade remembers it.
 *
 * Every blade hears every heartbeat, so each keeps the same 24-hour ring the
 * kobzar draws (logic.h's thermal history: six nodes, 5-minute UTC buckets, in
 * PSRAM) and serve.c hands it out at GET /temps for the page's chart. The clock
 * is the one SNTP set (eth v20); nothing is recorded before it is. A bucket this
 * blade did not hear - it remembers only since its own boot - is a JSON null on
 * the wire and a gap on the page, never a plausible number. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "logic.h"

void   thermal_start(void);      /* the ring in PSRAM + a 1 Hz sampler off the roster */
bool   thermal_ready(void);      /* false only if PSRAM refused the ring */

/* One node's statistic over the 288-bucket window ending at now_s, as a JSON
 * array (bohun_temp_json_series), taken under the ring's lock. 0 = did not fit. */
size_t thermal_series_json(unsigned slot, bohun_temp_stat_t stat, uint32_t now_s,
                           char *out, size_t cap);
