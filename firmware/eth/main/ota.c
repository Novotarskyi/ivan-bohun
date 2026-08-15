/* bohun OTA: firmware arrives over the fabric, no cable involved.
 * POST the raw .bin to /ota with the shared-secret header; the image lands in
 * the inactive OTA slot, boot flips, node reboots. Trust model: a compile-time
 * shared secret over TLS, on a door that never leaves the LAN.
 * Update ritual per node (build variant must match the board!):
 *   curl -k -H "X-Bohun-Key: $BOHUN_OTA_KEY" --data-binary @build.lilygo/bohun_eth.bin https://<node>:8443/ota
 *   (admin port 8443 only - 443 never carries /ota)
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_log.h"
#include "swarm.h"
#include "serve.h"
#include "blackbox.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"

static const char *TAG = "bohun_ota";
#include "secrets.h"
static const char *OTA_KEY = BOHUN_OTA_KEY;

/* Compare in constant time. strcmp bails at the first wrong byte, so response
 * timing leaks how many leading bytes an attacker guessed right - over a LAN that
 * is a real, if slow, oracle. Always walk the whole key. */
bool ota_key_ok(const char *given)
{
    size_t want = strlen(OTA_KEY);
    size_t got  = strlen(given);
    unsigned diff = (unsigned)(want ^ got);
    for (size_t i = 0; i < want; i++) {
        diff |= (unsigned)(OTA_KEY[i] ^ (i < got ? given[i] : 0));
    }
    return diff == 0;
}

esp_err_t ota_post_handler(httpd_req_t *req)
{
    char key[BOHUN_KEY_BUF] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Bohun-Key", key, sizeof(key)) != ESP_OK ||
        !ota_key_ok(key)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "bad key\n");
        return ESP_FAIL;   /* close: the unread image body must not be parsed as requests */
    }

    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (!dst) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "no OTA slot\n");
        return ESP_FAIL;   /* close: the unread image body must not be parsed as requests */
    }
    ESP_LOGI(TAG, "OTA begin: %d bytes -> %s", req->content_len, dst->label);

    /* VARIANT GUARD, fails CLOSED: parse the esp_app_desc_t from the first
     * chunk and decide BEFORE esp_ota_begin writes anything. A guard that
     * cannot verify must refuse - the shrugging version once let display
     * firmware onto a blade. Unparseable = rejected. */
    static char head[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)
                     + sizeof(esp_app_desc_t)];
    int got = 0;
    while (got < (int)sizeof(head)) {
        int n = httpd_req_recv(req, head + got, sizeof(head) - got);
        if (n <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "short image\n");
            return ESP_FAIL;   /* close: the unread image body must not be parsed as requests */
        }
        got += n;
    }
    {
        const esp_app_desc_t *theirs = (const esp_app_desc_t *)
            (head + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
        const esp_app_desc_t *mine = esp_app_get_description();
        bool sane = (theirs->magic_word == ESP_APP_DESC_MAGIC_WORD);
        bool same = sane && strncmp(mine->project_name, theirs->project_name,
                                    sizeof(theirs->project_name)) == 0;
        if (!same) {
            char nm[33] = "<unreadable>";
            if (sane) {
                snprintf(nm, sizeof(nm), "%.32s", theirs->project_name);
            }
            blackbox_event(BBX_VARIANT, 0, "refused %.24s", nm);
            ESP_LOGE(TAG, "OTA REFUSED: image is '%s', this node runs '%s'",
                     nm, mine->project_name);
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "wrong firmware variant - refused\n");
            return ESP_FAIL;   /* close: the unread image body must not be parsed as requests */
        }
        ESP_LOGI(TAG, "variant guard: '%s' accepted", theirs->project_name);
    }

    esp_ota_handle_t handle;
    if (esp_ota_begin(dst, OTA_SIZE_UNKNOWN, &handle) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "ota_begin failed\n");
        return ESP_OK;
    }
    swarm_set_ota(true);   /* the Pysar will announce RELEASE ONGOING while we take the image */
    blackbox_event(BBX_OTA_IN, 0, "%d KB inbound", req->content_len / 1024);

    /* the guard already consumed the head - write it before the rest */
    if (esp_ota_write(handle, head, got) != ESP_OK) {
        esp_ota_abort(handle);
        swarm_set_ota(false);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "ota_write failed\n");
        return ESP_OK;
    }

    static char buf[2048];
    int remaining = req->content_len - got;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (n <= 0) {
            esp_ota_abort(handle);
            swarm_set_ota(false);
            ESP_LOGE(TAG, "OTA recv failed with %d bytes left", remaining);
            return ESP_FAIL;
        }
        if (esp_ota_write(handle, buf, n) != ESP_OK) {
            esp_ota_abort(handle);
            swarm_set_ota(false);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "ota_write failed\n");
            return ESP_OK;
        }
        remaining -= n;
        swarm_set_ota_progress((uint8_t)(((int64_t)(req->content_len - remaining) * 100) / req->content_len));
    }

    if (esp_ota_end(handle) != ESP_OK) {   /* validates image magic + checksum */
        swarm_set_ota(false);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "image invalid\n");
        return ESP_OK;
    }
    /* NOT ESP_ERROR_CHECK: a transient otadata write failure at the very last
     * step would abort a node that is serving perfectly well. Answer like every
     * other failure in this handler and stay up on the current image. */
    esp_err_t br = esp_ota_set_boot_partition(dst);
    if (br != ESP_OK) {
        swarm_set_ota(false);
        ESP_LOGE(TAG, "set_boot_partition failed (%s) - staying on the current image",
                 esp_err_to_name(br));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "set_boot failed\n");
        return ESP_OK;
    }
    blackbox_event(BBX_OTA_OK, 0, "-> %s, rebooting", dst->label);
    ESP_LOGI(TAG, "OTA complete -> %s, rebooting", dst->label);
    httpd_resp_sendstr(req, "ok, rebooting\n");
    vTaskDelay(pdMS_TO_TICKS(300));   /* let the response flush */
    esp_restart();
    return ESP_OK;
}
