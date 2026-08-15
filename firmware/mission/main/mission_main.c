/* bohun mission: the 4.3" kobzar (Waveshare ESP32-S3-Touch-LCD-4.3B).
 * Second witness on the ESP-NOW control plane (fleet id 7, eligible=false) -
 * exactly the display node's role, with a mission-control UI and touch.
 * Reuses swarm.c/swarm_identity.c/mask.c from the node firmware unchanged;
 * ineligible means the mask code is never exercised here.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "blackbox.h"
#include "vitals.h"
#include "esp_log.h"
#include "swarm.h"
#include "ota_window.h"
#include "panel.h"
#include "ui.h"

static const char *TAG = "bohun_mission";

static void render_task(void *arg)
{
    (void)arg;
    static swarm_snap_entry_t snap[8];   /* one render task, one buffer - off the stack */
    uint8_t leader_id = 0;

    for (;;) {
        int n = swarm_snapshot(snap, 8, &leader_id);

        ui_update(snap, n, leader_id);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "bohun mission: 4.3in kobzar (ESP-NOW witness + touch dashboard)");

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    blackbox_init();   /* reboots get names here too */
    vitals_start();    /* same on-die sensor as the blades - the kobzar runs warm */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    lv_display_t *disp = panel_init();   /* expander, RGB, touch, LVGL */
    ui_init(disp);
    swarm_start();                       /* radio + roster as witness (fleet id 7) */
    ota_window_boot_trial();             /* pending-verify images must earn permanence */

    xTaskCreate(render_task, "ui_render", 6144, NULL, 4, NULL);
}
