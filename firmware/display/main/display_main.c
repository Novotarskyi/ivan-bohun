/* bohun: the display node.
 * A DevKitC (no Ethernet, no serving, no mask) that joins the ESP-NOW control
 * plane as a WITNESS (fleet id 6, eligible=false) and paints the swarm roster
 * on a 2.8" TFT. Reuses swarm.c/mask.c/swarm_identity.c from the node firmware
 * unchanged - the display is ineligible so it never wears the mask, and the
 * mask code is simply never exercised here.
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
#include "tft.h"
#include "led.h"
#include "ota_window.h"

static const char *TAG = "bohun_display";

static void render_task(void *arg)
{
    static swarm_snap_entry_t snap[8];   /* one render task, one buffer - off the stack */
    uint8_t leader_id = 0;
    uint32_t tick = 0;

    for (;;) {
        int n = swarm_snapshot(snap, 8, &leader_id);

        /* The rail samples 4x faster than the glass. A rebooting blade is only
         * "lost" for the second or two between the 3 s silence threshold and its
         * return, and booting lasts under a second - at one sample per second
         * those states land between ticks and never appear. Latching
         * them would overstate how long they lasted; sampling faster shows
         * what is actually there. The TFT keeps its 1 Hz: redrawing LVGL labels
         * four times a second costs real work to say the same thing. */
        led_render(snap, n, leader_id);
        if (++tick % 4 == 0) {
            tft_render(snap, n, leader_id);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "bohun display node (ESP-NOW witness + TFT)");

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    blackbox_init();   /* reboots get names here too */
    vitals_start();    /* same on-die sensor as the blades - a witness reports too */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    tft_init();          /* panel + LVGL, shows "waiting for the swarm" */
    led_init();          /* the SK6812 rail; dark no-op until wired */
    swarm_start();       /* radio + roster as a witness */
    ota_window_boot_trial();   /* pending-verify images must earn permanence */

    xTaskCreate(render_task, "tft_render", 6144, NULL, 4, NULL);
}
