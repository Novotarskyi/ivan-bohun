/* thermal.c - see thermal.h. The sampler is an esp_timer callback, not a task:
 * the blades' internal RAM is the scarce thing (one TLS session pins ~20 KB)
 * and a 1 Hz loop needs no stack of its own. The ring itself lives in PSRAM. */
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "swarm.h"
#include "vitals.h"
#include "thermal.h"

static const char *TAG = "bohun_thermal";

static bohun_temp_hist_t *s_hist;      /* PSRAM; NULL until thermal_start */
static SemaphoreHandle_t  s_mux;       /* the sampler writes, the public server reads */
static esp_timer_handle_t s_tick;

static void tick_cb(void *arg)
{
    (void)arg;
    uint32_t now_s = (uint32_t)time(NULL);
    if (!bohun_time_sane(now_s)) {
        return;                        /* no clock yet - nothing to file a reading under */
    }
    static swarm_snap_entry_t snap[8]; /* one timer task, one buffer - off its small stack */
    uint8_t leader;
    int n = swarm_snapshot(snap, 8, &leader);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    for (int i = 0; i < n; i++) {
        /* a lost node's last word is not a reading - it would replay for hours */
        if (!snap[i].alive || snap[i].temp_c10 == VITALS_TEMP_NONE) {
            continue;
        }
        int slot = bohun_temp_slot(snap[i].id);
        if (slot >= 0) {
            bohun_temp_record(s_hist, (unsigned)slot, now_s, snap[i].temp_c10);
        }
    }
    xSemaphoreGive(s_mux);
}

void thermal_start(void)
{
    s_hist = heap_caps_calloc(1, sizeof(*s_hist), MALLOC_CAP_SPIRAM);
    if (!s_hist) {
        ESP_LOGE(TAG, "no PSRAM for the thermal history - /temps will answer 503");
        return;
    }
    s_mux = xSemaphoreCreateMutex();
    const esp_timer_create_args_t args = {
        .callback = tick_cb, .name = "thermal", .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick, 1000000));
    ESP_LOGI(TAG, "thermal history up: %u nodes x %u buckets of %u s, %u B in PSRAM",
             (unsigned)BOHUN_TEMP_NODES, (unsigned)BOHUN_TEMP_BUCKETS,
             (unsigned)BOHUN_TEMP_BUCKET_S, (unsigned)sizeof(*s_hist));
}

bool thermal_ready(void)
{
    return s_hist != NULL;
}

size_t thermal_series_json(unsigned slot, bohun_temp_stat_t stat, uint32_t now_s,
                           char *out, size_t cap)
{
    if (!s_hist) {
        return 0;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    size_t len = bohun_temp_json_series(s_hist, slot, now_s, stat, out, cap);
    xSemaphoreGive(s_mux);
    return len;
}
