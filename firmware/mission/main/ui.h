#pragma once
#include "lvgl.h"
#include "swarm.h"

/* mission-control dashboard for the 800x480 kobzar panel.
 * ui_init builds the static tree (call once, under lvgl_port_lock);
 * ui_update repaints it from a fresh swarm snapshot (locked internally). */
void ui_init(lv_display_t *disp);
void ui_update(const swarm_snap_entry_t *e, int n, uint8_t leader_id);
