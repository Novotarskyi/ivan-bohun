/* selftest.c - D8: an image must EARN permanence.
 * An OTA'd image boots PENDING_VERIFY; this module refuses to mark it valid
 * until the TLS server is listening, the wire is up, and both STAY that way
 * for the soak. Fail, and the bootloader restores the previous slot - the
 * node comes back reachable with its OTA door open, no cables involved.
 * Written after two real bricks: both were clean-
 * building config one-liners that no review caught. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "serve.h"
#include "mask.h"
#include "splice.h"
#include "blackbox.h"
#include "selftest.h"

static const char *TAG = "bohun_selftest";

/* Long enough to outlive a boot-time crash and a first real handshake; short
 * enough that a good release settles quickly. The dynamic-buffer panic hit
 * within a minute of taking traffic, so this window would have caught it. */
#define SOAK_S          90
#define POLL_MS         2000
/* THE DEADLINE - D8 is toothless without it: rollback needs a REBOOT, and
 * the worst failure mode is perfectly stable while serving nothing. If the
 * trial is not won by the deadline, reboot ourselves and let the bootloader
 * do its job. */
#define DEADLINE_S      300

static void selftest_task(void *arg)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        /* already-trusted image (USB flash, or a previously verified OTA) */
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "image is ON TRIAL from %s - %d s to prove it serves",
             running->label, SOAK_S);
    blackbox_event(BBX_TRIAL, SOAK_S, "on trial from %s", running->label);

    int healthy_s = 0, elapsed_s = 0;
    while (healthy_s < SOAK_S) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        elapsed_s += POLL_MS / 1000;
        if (elapsed_s >= DEADLINE_S) {
            blackbox_event(BBX_TRIAL, (uint16_t)healthy_s,
                           "FAILED trial - rolling back");
            ESP_LOGE(TAG, "trial FAILED after %d s (best healthy run %d s) - "
                          "rebooting so the bootloader restores the last good image",
                     elapsed_s, healthy_s);
            vTaskDelay(pdMS_TO_TICKS(500));   /* let the record reach flash */
            esp_restart();
        }
        /* mask_link_up(), NOT !mask_link_lost(): the latter is false for a
         * link that never came up, and httpd binds before any cable talks -
         * together they would certify an image with no working wire. */
        /* splice_is_up() joins the gate: on a balancing fleet
         * the PUBLIC port belongs to the splicer, so an image with a healthy
         * httpd and a dead splicer serves nobody - the exact stable-but-serving-
         * nothing shape D8 was written after. Weak-stubbed true where there is
         * no splicer, so this is a no-op for observer-style builds. */
        if (serve_is_ready() && mask_link_up() && splice_is_up()) {
            healthy_s += POLL_MS / 1000;
        } else {
            /* any stumble restarts the clock: we want SOAK_S of continuous
             * health, not SOAK_S of intermittent luck */
            if (healthy_s) {
                ESP_LOGW(TAG, "trial reset (serving=%d link=%d)",
                         serve_is_ready(), mask_link_up());
            }
            healthy_s = 0;
        }
    }

    esp_err_t r = esp_ota_mark_app_valid_cancel_rollback();
    blackbox_event(BBX_VALID, (uint16_t)r, "verified after %d s (%s)",
                   SOAK_S, esp_err_to_name(r));
    if (r == ESP_OK) {
        ESP_LOGW(TAG, "image VERIFIED and marked valid - rollback cancelled");
    } else {
        /* Do not claim success we did not get: if this call failed the image is
         * still PENDING_VERIFY and the NEXT reboot rolls it back, however well
         * it has been serving. Saying "VERIFIED" here would leave the operator
         * reading a log that asserts the opposite of what is about to happen. */
        ESP_LOGE(TAG, "trial passed but mark-valid FAILED (%s) - this image is "
                      "still on trial and the next reboot will roll it back",
                 esp_err_to_name(r));
    }
    vTaskDelete(NULL);
}

void selftest_start(void)
{
    xTaskCreate(selftest_task, "selftest", 3072, NULL, 2, NULL);
}

bool selftest_on_trial(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return false;
    }
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}
