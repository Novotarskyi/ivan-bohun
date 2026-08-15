/* bohun swarm identity: who am I, who are my peers.
 * Keyed on the eFuse base MAC (= the Wi-Fi STA MAC, base+0) against the
 * fleet ledger below. One binary per BOARD TYPE; per-node identity comes
 * from silicon, not from the build. To adopt a new board, add its MAC here
 * (idf.py monitor prints it on boot) - see README "Build your own", step 3.
 */
#include <string.h>
#include "esp_mac.h"
#include "swarm_identity.h"

static const swarm_member_t FLEET[] = {
    { .id = 1, .name = "N1", .shortname = "N1", .hostname = "n1",
      .base_mac = {0x94, 0xa9, 0x90, 0xca, 0x30, 0x14}, .eligible = true },
    { .id = 2, .name = "N2", .shortname = "N2", .hostname = "n2",
      .base_mac = {0x14, 0xc1, 0x9f, 0x4a, 0xa0, 0x78}, .eligible = true },
    { .id = 3, .name = "N3", .shortname = "N3", .hostname = "n3",
      .base_mac = {0x14, 0xc1, 0x9f, 0x4a, 0xa0, 0xe0}, .eligible = true },
    { .id = 4, .name = "N4", .shortname = "N4", .hostname = "n4",
      .base_mac = {0xa4, 0xcb, 0x8f, 0xcf, 0x2d, 0x68}, .eligible = true },
    { .id = 5, .name = "WS", .shortname = "WS", .hostname = "ws",
      .base_mac = {0xa4, 0xcb, 0x8f, 0xdf, 0x0a, 0x1c}, .eligible = true },
    /* the pysar keeps the record on the small glass */
    { .id = 6, .name = "Display", .shortname = "Display", .hostname = "display",
      .base_mac = {0x44, 0xb1, 0x76, 0xce, 0x29, 0xa0}, .eligible = false, .witness_role = "pysar" },
    /* the kobzar sings the chronicle on the wide glass */
    { .id = 7, .name = "Mission Control", .shortname = "Mission", .hostname = "mission",
      .base_mac = {0xe8, 0xf6, 0x0a, 0x8e, 0x8c, 0x60}, .eligible = false, .witness_role = "kobzar" },
};
#define FLEET_N (sizeof(FLEET) / sizeof(FLEET[0]))

const swarm_member_t *swarm_self(void)
{
    static const swarm_member_t *self;
    static bool looked_up = false;
    if (!looked_up) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        for (size_t i = 0; i < FLEET_N; i++) {
            if (memcmp(mac, FLEET[i].base_mac, 6) == 0) {
                self = &FLEET[i];
                break;
            }
        }
        looked_up = true;
    }
    return self;   /* NULL on a board not in the ledger */
}

size_t swarm_fleet_size(void) { return FLEET_N; }
const swarm_member_t *swarm_member(size_t idx) { return idx < FLEET_N ? &FLEET[idx] : NULL; }

const swarm_member_t *swarm_member_by_mac(const uint8_t mac[6])
{
    for (size_t i = 0; i < FLEET_N; i++) {
        if (memcmp(mac, FLEET[i].base_mac, 6) == 0) {
            return &FLEET[i];
        }
    }
    return NULL;
}
