/* vitals.h - the ESP32-S3's on-die temperature sensor.
 *
 * The instrument behind the rack's thermal design ("positive vertical draw,
 * no recirculation pocket, the charger sits at the coolest point") - the
 * claim is only real while something measures it.
 *
 * READ THE UNITS BEFORE TRUSTING A NUMBER. This is DIE temperature, not room
 * temperature: it runs well above ambient and it says nothing about the W5500,
 * nothing about the charger, and nothing about the air. Its value is entirely
 * RELATIVE - blade against blade at different heights, and the same blade
 * before and after a change. Treat an absolute reading as a bad thermometer.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* tenths of a degree C, so it crosses the wire as one int16_t.
 * VITALS_TEMP_NONE means "this node did not tell us" - either it is still on a
 * pre-v9 build, or its sensor failed to install. It is NOT a reading of zero. */
/* Operating norms, shared with every glass that colours a reading: below
 * TEMP_WARN_C is normal, at it the blackbox starts logging TEMPHI, at
 * TEMP_HOT_C the log message escalates. Same numbers, one place. */
#define TEMP_WARN_C   60
#define TEMP_HOT_C    70

#define VITALS_TEMP_NONE  ((int16_t)-32768)

void    vitals_start(void);

/* latest die temperature in tenths of a degree C, or VITALS_TEMP_NONE.
 * Cheap: returns a cached value sampled by the swarm monitor, never blocks. */
int16_t vitals_temp_c10(void);

/* sample the sensor. Called on the swarm monitor's 1 Hz tick - the sensor is
 * not free to read, and nothing here needs it faster than the heartbeat. */
void    vitals_sample(void);
