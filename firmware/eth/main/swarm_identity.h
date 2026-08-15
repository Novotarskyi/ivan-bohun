#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t id;             /* 1..4 = rack blades, 5 = the bench spare (eligible when plugged),
                             * 6..7 = witnesses (eligible=false - observe, never wear the mask) */
    const char *name;       /* the ceremonial name, shown to the world */
    const char *shortname;  /* what fits on the 2.8" glass */
    const char *hostname;   /* DHCP-safe: lowercase, no spaces */
    uint8_t base_mac[6];    /* eFuse base = Wi-Fi STA MAC = the ESP-NOW address */
    bool eligible;          /* can be elected leader? witnesses (WS, display) cannot */
    const char *witness_role; /* ineligible members' role word (pysar/kobzar); NULL for blades */
} swarm_member_t;

const swarm_member_t *swarm_self(void);                       /* NULL if not in the ledger */
size_t swarm_fleet_size(void);
const swarm_member_t *swarm_member(size_t idx);
const swarm_member_t *swarm_member_by_mac(const uint8_t mac[6]);
