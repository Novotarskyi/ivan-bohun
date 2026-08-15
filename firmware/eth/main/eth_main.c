/* bohun - the boot sequencer: W5500 up, then the whole node stack.
 * Init glue = espressif/ethernet_init component (pinned); handlers mirror
 * ESP-IDF v6.0.2 examples/ethernet/basic. Nothing here is original except
 * the boot banner - which is the point: reuse the vendor-proven path.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "ethernet_init.h"
#include "nvs_flash.h"
#include "swarm_identity.h"
#include "swarm.h"
#include "serve.h"
#include "mask.h"
#include "blackbox.h"
#include "vitals.h"
#include "selftest.h"
#include "splice.h"
#include "sdkconfig.h"

static const char *TAG = "bohun_eth";

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        mask_on_link_up();
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        mask_set_ip(0);
        mask_on_link_down();   /* the election must see this, or a dead leader keeps the mask */
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    mask_set_ip(ip_info->ip.addr);
    mask_on_got_ip();
}

void app_main(void)
{
    ESP_LOGI(TAG, "bohun node firmware (eth + TLS serve + swarm control plane)");

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    /* event loop must exist before ethernet_init_all - it registers handlers */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles;
    ESP_ERROR_CHECK(ethernet_init_all(&eth_handles, &eth_port_cnt));

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[0])));
    const swarm_member_t *self = swarm_self();
    ESP_ERROR_CHECK(esp_netif_set_hostname(eth_netif, self ? self->hostname : CONFIG_BOHUN_HOSTNAME));

    /* the mask module needs the handle+netif; own identity captured here.
     * boot proceeds on the OWN identity (reachable) - the mask is worn later,
     * only if/when this node is elected leader. */
    mask_init(eth_handles[0], eth_netif);

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    ESP_ERROR_CHECK(esp_eth_start(eth_handles[0]));

    /* Radio first, server second: the fleet gets a truthful serving=0 window
     * while TLS binds (the rail's amber "booting"). Safe only because the
     * election gates on serve_is_ready() - exposure without candidacy. */
    /* the recorder mounts before anything can fail interestingly */
    blackbox_init();
    vitals_start();   /* before the swarm: the first heartbeat carries a reading */
    swarm_start();
    serve_start();

    /* D8: if this image arrived by OTA it is on trial until it proves it can
     * serve. Nothing else in the boot path may call mark_app_valid. */
    selftest_start();

    /* EVERY blade runs the splicer, permanently. It binds 0.0.0.0:443, but
     * only the blade wearing the mask holds the address the router forwards
     * to - so the mask alone decides who receives public traffic, and a
     * leadership change moves the load with ZERO listener churn. No moving
     * parts in this path: starting and stopping listeners on election once
     * produced a 272-election flap. */
    splice_start();
}
