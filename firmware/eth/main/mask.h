#pragma once

/* The mask's FIXED address - the one the router forwards to. It must sit
 * outside the router's DHCP pool. Like every other deployment specific, it
 * lives in the gitignored secrets.h, never in the tree. */
#include "secrets.h"
#ifndef BOHUN_MASK_IP
#error "add BOHUN_MASK_IP (the mask's reserved LAN address) to secrets.h"
#endif
#ifndef BOHUN_MASK_GW
#error "add BOHUN_MASK_GW (the LAN gateway) to secrets.h"
#endif
#define MASK_STATIC_IP  BOHUN_MASK_IP
#include <stdbool.h>
#include <stdint.h>
#include "esp_eth_driver.h"
#include "esp_netif.h"

/* The mask: the swarm's single immortal wire identity (the vMAC). The elected
 * leader wears it; followers keep their own identity. See docs/2_firmware/23_node_swarm.md. */

void mask_init(esp_eth_handle_t eth_handle, esp_netif_t *eth_netif);
void mask_set_leader(bool i_am_leader);  /* election calls this on a leadership transition */
void mask_tick(void);                    /* called periodically - drives the safety fallback */
void mask_on_got_ip(void);               /* called from the eth GOT_IP handler */
void mask_on_link_up(void);              /* called from the eth CONNECTED handler */
void mask_on_link_down(void);            /* called from the eth DISCONNECTED handler */
const uint8_t *mask_vmac(void);          /* the 6-byte vMAC, for logging */

/* the node's current eth IPv4 (esp_ip4 .addr, 0 if none) - shared into heartbeats */
void mask_set_ip(uint32_t ip);
uint32_t mask_get_ip(void);
bool mask_is_wearing(void);
/* Do we currently hold an address at all? A blade between the mask and a fresh
 * DHCP lease is reachable by nobody, and must not claim to be serving. */
bool mask_has_ip(void);
bool mask_link_up(void);
bool mask_link_lost(void);   /* down for longer than a transition bounce */     /* Ethernet PHY link state - the data plane's own opinion */
void mask_abdicate(void);    /* drop the mask now (link died under us) */
