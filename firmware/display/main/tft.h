#pragma once
#include "swarm.h"

/* the 2.8" TFT (ST7789, 240x320) driven over SPI + LVGL. Pins per the loom in
 * docs/1_hardware/13_data_connectivity.md:
 *   SCLK=12 MOSI=11 CS=10 DC=9 RST=8 BL=7  (MISO unwired; touch INT-only) */
void tft_init(void);

/* render the swarm onto the panel. entries[] = full fleet snapshot (self first). */
void tft_render(const swarm_snap_entry_t *entries, int n, uint8_t leader_id);
