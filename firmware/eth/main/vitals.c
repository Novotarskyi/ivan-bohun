/* vitals.c - on-die temperature, sampled 1 Hz into a cached int16_t.
 * Cached: JSON and heartbeat must never block on the peripheral, and must
 * agree on one number. Range -10..80: the narrow ranges are ~1 C accurate
 * (the wide are ~3), and inter-blade deltas are the signal - if a reading
 * pins at a range end, widen the range, never average it away. No sensor is
 * non-fatal: report VITALS_TEMP_NONE, not a plausible zero. */
#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "blackbox.h"
#include "vitals.h"

static const char *TAG = "bohun_vitals";

static temperature_sensor_handle_t s_tsens;
static volatile int16_t s_temp_c10 = VITALS_TEMP_NONE;

/* Blackbox thresholds, in whole degrees. Same shape as the heap tiers: an amber
 * that says "worth knowing" and a red that says "something is wrong with the
 * air". The S3 is rated to 85 C ambient and throttles far above these, so these
 * are RACK alarms, not chip alarms - by the time a blade in open air reads 70 C
 * the airflow assumption has already failed. */

/* One-shot latches, so a blade sitting at 61 C does not fill the ring with
 * identical records. Re-arm 3 C below, which is wider than the sensor's noise. */
static bool s_warned, s_hot;

void vitals_start(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no die thermometer (%s) - temperature reports as unknown",
                 esp_err_to_name(err));
        s_tsens = NULL;
        return;
    }
    err = temperature_sensor_enable(s_tsens);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "thermometer installed but would not enable (%s)",
                 esp_err_to_name(err));
        temperature_sensor_uninstall(s_tsens);
        s_tsens = NULL;
        return;
    }
    vitals_sample();
    ESP_LOGI(TAG, "die thermometer up, -10..80 C, first reading %d.%d C",
             s_temp_c10 / 10, (s_temp_c10 < 0 ? -s_temp_c10 : s_temp_c10) % 10);
}

void vitals_sample(void)
{
    if (!s_tsens) {
        return;
    }
    float c;
    if (temperature_sensor_get_celsius(s_tsens, &c) != ESP_OK) {
        return;                      /* keep the last good value, do not zero it */
    }
    int16_t c10 = (int16_t)(c * 10.0f);
    s_temp_c10 = c10;

    int whole = c10 / 10;
    if (whole >= TEMP_HOT_C && !s_hot) {
        s_hot = true;
        blackbox_event(BBX_TEMPHI, (uint16_t)whole, "die %d C - check the airflow", whole);
    } else if (whole < TEMP_HOT_C - 3) {
        s_hot = false;
    }
    if (whole >= TEMP_WARN_C && !s_warned) {
        s_warned = true;
        blackbox_event(BBX_TEMPHI, (uint16_t)whole, "die %d C - warm", whole);
    } else if (whole < TEMP_WARN_C - 3) {
        s_warned = false;
    }
}

int16_t vitals_temp_c10(void)
{
    return s_temp_c10;
}
