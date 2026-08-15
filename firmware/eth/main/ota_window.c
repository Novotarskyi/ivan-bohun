/* ota_window.c - the on-demand WiFi OTA window for radio-only observers.
 * See ota_window.h for the contract.
 * Compiled into the display and mission firmwares only - blades have a
 * standing admin plane and never need this. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_https_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "swarm.h"
#include "ota.h"
#include "blackbox.h"
#include "secrets.h"
#include "ota_window.h"

/* older secrets.h files predate the WiFi fields - build anyway, refuse at
 * runtime with a message on the glass rather than a compile error */
#ifndef BOHUN_WIFI_SSID
#define BOHUN_WIFI_SSID ""
#endif
#ifndef BOHUN_WIFI_PASS
#define BOHUN_WIFI_PASS ""
#endif

#define WINDOW_S    600     /* the window always expires - standby's law */
#define JOIN_S      45      /* association + DHCP budget before FAILED - the rack
                             * itself is an RF furnace, DHCP may need retries */
#define TRIAL_S     60      /* pending-verify: seconds alive before valid */
#define DEADLINE_S  300     /* unmarked past this -> reboot -> rollback */

static const char *TAG = "bohun_otaw";

extern const uint8_t servercert_start[] asm("_binary_servercert_pem_start");
extern const uint8_t servercert_end[]   asm("_binary_servercert_pem_end");
extern const uint8_t prvtkey_start[]    asm("_binary_prvtkey_pem_start");
extern const uint8_t prvtkey_end[]      asm("_binary_prvtkey_pem_end");

static volatile ota_window_state_t s_state = OTAW_IDLE;
static char     s_ip[16];
static int64_t  s_expiry_us;
static httpd_handle_t s_srv;
static esp_netif_t *s_netif;

/* ---- /page: raw write into the kobzar's strip partition ------------------ */

static esp_err_t page_post_handler(httpd_req_t *req)
{
    char key[BOHUN_KEY_BUF] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Bohun-Key", key, sizeof(key)) != ESP_OK ||
        !ota_key_ok(key)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "bad key\n");
        return ESP_FAIL;
    }
    const esp_partition_t *page =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "page");
    if (!page) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "no page partition on this board\n");
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || (uint32_t)req->content_len > page->size) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "does not fit the page partition\n");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "page write: %d KB -> %s", req->content_len / 1024, page->label);
    blackbox_event(BBX_OTA_IN, 1, "page %d KB inbound", req->content_len / 1024);
    swarm_set_ota(true);

    /* erase must cover whole 4 KB sectors */
    size_t erase = (req->content_len + 0xFFF) & ~(size_t)0xFFF;
    if (esp_partition_erase_range(page, 0, erase) != ESP_OK) {
        swarm_set_ota(false);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "erase failed\n");
        return ESP_OK;
    }
    static char buf[4096];
    int off = 0, remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf,
                               remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (n <= 0) {
            swarm_set_ota(false);
            ESP_LOGE(TAG, "page recv failed with %d left", remaining);
            return ESP_FAIL;
        }
        if (esp_partition_write(page, off, buf, n) != ESP_OK) {
            swarm_set_ota(false);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "write failed\n");
            return ESP_OK;
        }
        off += n;
        remaining -= n;
        swarm_set_ota_progress((uint8_t)(((int64_t)off * 100) / req->content_len));
    }
    swarm_set_ota(false);
    blackbox_event(BBX_OTA_OK, 1, "page written");
    httpd_resp_sendstr(req, "page written - restart to see it\n");
    return ESP_OK;
}

/* ---- the window server ---------------------------------------------------- */

static void server_start(void)
{
    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.servercert     = servercert_start;
    cfg.servercert_len = servercert_end - servercert_start;
    cfg.prvtkey_pem    = prvtkey_start;
    cfg.prvtkey_len    = prvtkey_end - prvtkey_start;
    cfg.port_secure    = 8443;
    cfg.httpd.max_open_sockets = 3;
    cfg.httpd.lru_purge_enable = true;
    if (httpd_ssl_start(&s_srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "window server failed to start");
        s_state = OTAW_FAILED;
        return;
    }
    const httpd_uri_t ota_uri  = { .uri = "/ota",  .method = HTTP_POST,
                                   .handler = ota_post_handler };
    const httpd_uri_t page_uri = { .uri = "/page", .method = HTTP_POST,
                                   .handler = page_post_handler };
    httpd_register_uri_handler(s_srv, &ota_uri);
    httpd_register_uri_handler(s_srv, &page_uri);
    s_state = OTAW_READY;
    ESP_LOGW(TAG, "OTA WINDOW OPEN: https://%s:8443 for %d s", s_ip, WINDOW_S);
    blackbox_event(BBX_OTA_IN, 2, "window open %s", s_ip);
}

static void got_ip_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    /* server start happens in window_task, NOT here: this runs on the system
     * event task, whose stack cannot carry mbedtls parsing the cert pair */
}

/* The netif lifecycle, driven BY HAND. The default wifi-netif glue keys off
 * events relative to a netif that exists from boot; ours is created mid-life,
 * and every step the glue was trusted with has now failed once (STA_START ->
 * the NULL-input panic, STA_CONNECTED -> a DHCP client that never started).
 * So: we connect the netif and start DHCP ourselves. The actions are
 * status-guarded internally, so overlap with the default glue is harmless. */
static void wifi_evt_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "associated - bringing netif up, starting DHCP");
        esp_netif_action_connected(s_netif, base, id, data);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "AP dropped us, reason %d", d->reason);
        esp_netif_action_disconnected(s_netif, base, id, data);
    }
}

static void window_task(void *arg)
{
    (void)arg;
    s_state = OTAW_JOINING;
    s_ip[0] = '\0';
    s_expiry_us = esp_timer_get_time() + (int64_t)WINDOW_S * 1000000;

    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_sta();
        /* WiFi started at boot for ESP-NOW, so the STA_START event this
         * netif's lifecycle rides on already fired and never will again.
         * Do BOTH of that handler's jobs by hand: without the start, the
         * first frame off the AP jumps through a NULL input fn (PC=0 panic);
         * without the MAC, the netif transmits as 00:00:00:00:00:00 and the
         * AP discards every data frame - DHCP discovers into the void. */
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        esp_netif_set_mac(s_netif, mac);
        esp_netif_action_start(s_netif, NULL, 0, NULL);
    }
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_cb, NULL);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt_cb, NULL);

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, BOHUN_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, BOHUN_WIFI_PASS, sizeof(wc.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();

    int64_t join_deadline = esp_timer_get_time() + (int64_t)JOIN_S * 1000000;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        int64_t now = esp_timer_get_time();
        if (s_state == OTAW_JOINING && s_ip[0]) {
            server_start();   /* lease arrived - the door opens from THIS stack */
        }
        /* narrate the join on the monitor: dhcp state 0=INIT (never started,
         * our wiring), 1=STARTED (discovering - no answer yet), 2=STOPPED */
        static int64_t last_dhcp_log;
        if (s_state == OTAW_JOINING && now - last_dhcp_log > 5000000) {
            last_dhcp_log = now;
            esp_netif_dhcp_status_t dh = ESP_NETIF_DHCP_INIT;
            esp_netif_dhcpc_get_status(s_netif, &dh);
            ESP_LOGI(TAG, "joining: dhcp client state %d", dh);
        }
        if (s_state == OTAW_JOINING && now > join_deadline) {
            esp_netif_dhcp_status_t dh = ESP_NETIF_DHCP_INIT;
            esp_netif_dhcpc_get_status(s_netif, &dh);
            ESP_LOGE(TAG, "wifi join timed out (dhcp client state %d)", dh);
            s_state = OTAW_FAILED;
            vTaskDelay(pdMS_TO_TICKS(4000));   /* let the glass show FAILED */
            break;
        }
        if (s_state == OTAW_FAILED || s_state == OTAW_IDLE) {
            break;
        }
        if (now > s_expiry_us) {
            ESP_LOGW(TAG, "window expired");
            break;
        }
        /* an image in flight extends the window - never cut a transfer */
        if (swarm_own_updating()) {
            s_state = OTAW_RECEIVING;
            s_expiry_us = now + 60 * 1000000LL;
        } else if (s_state == OTAW_RECEIVING) {
            s_state = OTAW_READY;   /* transfer ended without reboot = failed push */
        }
    }

    /* teardown: server down, WiFi off the AP, swarm channel re-pinned */
    if (s_srv) {
        httpd_ssl_stop(s_srv);
        s_srv = NULL;
    }
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_cb);
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt_cb);
    esp_wifi_disconnect();
    swarm_repin_channel();
    s_ip[0] = '\0';
    s_state = OTAW_IDLE;
    ESP_LOGW(TAG, "OTA window closed - radio-only again");
    vTaskDelete(NULL);
}

void ota_window_open(void)
{
    if (s_state != OTAW_IDLE && s_state != OTAW_NOCREDS && s_state != OTAW_FAILED) {
        return;   /* already open */
    }
    if (BOHUN_WIFI_SSID[0] == '\0') {
        s_state = OTAW_NOCREDS;
        ESP_LOGE(TAG, "no BOHUN_WIFI_SSID in secrets.h - window refused");
        return;
    }
    /* 8 KB: this stack carries httpd_ssl_start's mbedtls cert parsing */
    xTaskCreate(window_task, "ota_window", 8192, NULL, 5, NULL);
}

void ota_window_close(void)
{
    if (s_state == OTAW_NOCREDS) {
        s_state = OTAW_IDLE;
        return;
    }
    if (s_state != OTAW_IDLE) {
        s_expiry_us = 0;   /* the task notices within 500 ms and tears down */
    }
}

ota_window_state_t ota_window_status(char ip[16], int *secs_left, int *pct)
{
    if (ip) {
        strlcpy(ip, s_ip, 16);
    }
    if (secs_left) {
        int64_t left = (s_expiry_us - esp_timer_get_time()) / 1000000;
        *secs_left = (s_state == OTAW_IDLE || s_state == OTAW_NOCREDS)
                     ? 0 : (left > 0 ? (int)left : 0);
    }
    if (pct) {
        *pct = swarm_own_update_pct();
    }
    return s_state;
}

/* ---- the boot trial (D8's shape, an observer's proof) --------------------- */

static void trial_task(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        int64_t up_s = (esp_timer_get_time() - t0) / 1000000;
        if (up_s >= TRIAL_S && swarm_peers_heard() > 0) {
            esp_ota_mark_app_valid_cancel_rollback();
            blackbox_event(BBX_BOOT, 1, "trial passed - image valid");
            ESP_LOGW(TAG, "trial passed after %llds - image marked valid",
                     (long long)up_s);
            vTaskDelete(NULL);
            return;
        }
        if (up_s >= DEADLINE_S) {
            /* alive but deaf (or never proved itself): reboot unmarked and
             * let the bootloader restore the previous image */
            blackbox_event(BBX_BOOT, 2, "trial FAILED - rebooting for rollback");
            ESP_LOGE(TAG, "trial deadline passed - rebooting for rollback");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
    }
}

void ota_window_boot_trial(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "image on trial: %d s to prove, %d s deadline",
                 TRIAL_S, DEADLINE_S);
        xTaskCreate(trial_task, "ota_trial", 3072, NULL, 4, NULL);
    }
}
