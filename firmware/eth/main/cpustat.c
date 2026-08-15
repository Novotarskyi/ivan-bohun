/* cpustat.c - where does the CPU actually go?
 *
 * The load-test signature this explains: a fixed ceiling in req/s however
 * many clients ask, with handshake latency scaling linearly in concurrency -
 * one task doing all the work. This module shows it from the inside.
 *
 * A background sampler diffs FreeRTOS run-time counters over a window and
 * keeps the result; the HTTP handler only formats what is already computed.
 * Sampling in the handler would have meant sleeping inside the very task under
 * measurement - the observer changing what it observes.
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "cpustat.h"

static const char *TAG = "bohun_cpu";

#define MAX_TASKS 24
#define WINDOW_MS 2000

typedef struct {
    char name[16];
    uint32_t pct_x10;      /* share of total run time in the window, tenths of a % */
    int core;              /* 0, 1, or -1 for unpinned */
    uint32_t stack_free;   /* high-water mark, bytes */
} cpu_row_t;

static cpu_row_t s_rows[MAX_TASKS];
static int s_nrows;
static uint32_t s_idle_x10[2] = {1000, 1000};   /* per-core idle share */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

static void sampler(void *arg)
{
    static TaskStatus_t a[MAX_TASKS], b[MAX_TASKS];
    for (;;) {
        uint32_t t_a = 0, t_b = 0;
        UBaseType_t na = uxTaskGetSystemState(a, MAX_TASKS, &t_a);
        vTaskDelay(pdMS_TO_TICKS(WINDOW_MS));
        UBaseType_t nb = uxTaskGetSystemState(b, MAX_TASKS, &t_b);

        /* FreeRTOS returns 0 - not a truncated list - when the array cannot hold
         * every task. Silently continuing froze /cpu on stale rows with nothing
         * to say why. Count ~18 today; say so loudly if it is ever exceeded. */
        if (na == 0 || nb == 0) {
            static bool moaned;
            if (!moaned) {
                moaned = true;
                ESP_LOGE(TAG, "task table overflow: more than %d tasks - /cpu is "
                              "now stale; raise MAX_TASKS", MAX_TASKS);
            }
            continue;
        }
        uint32_t span = t_b - t_a;
        if (!span) {
            continue;
        }
        cpu_row_t rows[MAX_TASKS];
        int n = 0;
        uint32_t idle[2] = {0, 0};
        for (UBaseType_t i = 0; i < nb && n < MAX_TASKS; i++) {
            uint32_t prev = 0;
            for (UBaseType_t j = 0; j < na; j++) {
                if (a[j].xHandle == b[i].xHandle) {
                    prev = a[j].ulRunTimeCounter;
                    break;
                }
            }
            uint32_t used = b[i].ulRunTimeCounter - prev;
            /* ESP-IDF's total-run-time is ELAPSED time, not elapsed x 2
             * cores - a task pegging one core reads ~100%, and the two idle
             * tasks sum to ~200% when the chip is free. No doubling. */
            uint32_t x10 = (uint32_t)(((uint64_t)used * 1000) / span);
            int core = -1;
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
            core = (b[i].xCoreID == tskNO_AFFINITY) ? -1 : (int)b[i].xCoreID;
#endif
            if (strncmp(b[i].pcTaskName, "IDLE", 4) == 0) {
                int c = (core >= 0 && core < 2) ? core : 0;
                idle[c] = x10;
                continue;                     /* idle is reported as headroom */
            }
            snprintf(rows[n].name, sizeof(rows[n].name), "%s", b[i].pcTaskName);
            rows[n].pct_x10 = x10;
            rows[n].core = core;
            rows[n].stack_free = b[i].usStackHighWaterMark;
            n++;
        }
        /* biggest consumer first - the answer is usually row 1 */
        for (int i = 1; i < n; i++) {
            cpu_row_t k = rows[i];
            int j = i - 1;
            while (j >= 0 && rows[j].pct_x10 < k.pct_x10) { rows[j + 1] = rows[j]; j--; }
            rows[j + 1] = k;
        }
        portENTER_CRITICAL(&s_mux);
        memcpy(s_rows, rows, sizeof(cpu_row_t) * n);
        s_nrows = n;
        s_idle_x10[0] = idle[0];
        s_idle_x10[1] = idle[1];
        portEXIT_CRITICAL(&s_mux);
    }
}

void cpustat_start(void)
{
    xTaskCreatePinnedToCore(sampler, "cpustat", 3072, NULL, 1, NULL, tskNO_AFFINITY);
}

#else   /* stats not compiled in */
void cpustat_start(void) { }
#endif

esp_err_t cpustat_http(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char line[256];   /* the header line alone runs past 128 */
    int n;

#if !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    httpd_resp_sendstr(req,
        "run-time stats not compiled in\n"
        "(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS + USE_TRACE_FACILITY)\n");
    return ESP_OK;
#else
    cpu_row_t rows[MAX_TASKS];
    int cnt;
    uint32_t idle0, idle1;
    portENTER_CRITICAL(&s_mux);
    cnt = s_nrows;
    memcpy(rows, s_rows, sizeof(cpu_row_t) * (cnt > 0 ? cnt : 0));
    idle0 = s_idle_x10[0];
    idle1 = s_idle_x10[1];
    portEXIT_CRITICAL(&s_mux);

    n = snprintf(line, sizeof(line),
                 "cpu %d MHz x2 cores, %d ms window\n"
                 "core0 idle %3u.%u%%   core1 idle %3u.%u%%   (100%% = that core free)\n"
                 "%-16s %7s %5s %s\n",
                 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, WINDOW_MS,
                 (unsigned)(idle0 / 10), (unsigned)(idle0 % 10),
                 (unsigned)(idle1 / 10), (unsigned)(idle1 % 10),
                 "task", "cpu%", "core", "stack_free");
    httpd_resp_send_chunk(req, line, n);

    for (int i = 0; i < cnt; i++) {
        unsigned one_core = rows[i].pct_x10;   /* already % of one core */
        n = snprintf(line, sizeof(line), "%-16s %4u.%u%% %5d %u\n",
                     rows[i].name, one_core / 10, one_core % 10,
                     rows[i].core, (unsigned)rows[i].stack_free);
        if (httpd_resp_send_chunk(req, line, n) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
#endif
}
