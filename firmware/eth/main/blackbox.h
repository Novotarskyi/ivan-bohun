/* blackbox.h - the fleet's flight recorder (blades only).
 *
 * Answers the question counters cannot: one 500 in a day of clean serving -
 * which request, which handler, how much heap was left at that moment?
 * The counters count; they do not remember. This module remembers.
 *
 * A raw ring of fixed 64-byte records in its own flash partition ("blackbox",
 * 256 KB = 4096 records), written at the moment of the event, surviving reboot,
 * OTA and panic. No filesystem: a sector-erase-ahead ring over esp_partition is
 * ~150 lines, wear-friendly, and has no mount step to fail. Read back over the
 * admin plane: GET https://<node>:8443/blackbox (text, newest first).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum {
    BBX_BOOT     = 1,   /* one per boot: reset reason + the previous life's floor is gone, note the fresh start */
    BBX_ERR5XX   = 2,   /* httpd emitted a 5xx - code + URI + heap truth at that instant */
    BBX_OOM      = 3,   /* one of our explicit malloc guards fired */
    BBX_HEAPLOW  = 4,   /* free heap crossed below the amber line (edge-triggered) */
    BBX_MASK_ON  = 5,   /* this node claimed the mask */
    BBX_MASK_OFF = 6,   /* this node lost/dropped the mask */
    BBX_OTA_IN   = 7,   /* a release started landing on this node */
    BBX_OTA_OK   = 8,   /* release verified, about to reboot into it */
    BBX_CHRON    = 9,   /* mission only: a chronicle line, made persistent */
    BBX_TRIAL    = 10,  /* D8: this image is on trial and must earn permanence */
    BBX_VALID    = 11,  /* D8: it earned it - rollback cancelled */
    BBX_VARIANT  = 12,  /* D8: an OTA was REFUSED - wrong firmware variant */
    BBX_TEMPHI   = 13,  /* die temperature crossed a rack alarm line (edge-triggered) */
    BBX_SPLICE   = 14,  /* the splicer came up / its backend set changed */
};

/* Log one event. Cheap-path safe: fixed stack buffer, one 64 B flash write under
 * a mutex (a sector erase happens once per 64 records). fmt/args become the
 * record's 35-char text field - put the URI or the detail there. */
void blackbox_event(uint8_t kind, uint16_t code, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Mount the ring (find the write head), count the boot, write the BOOT record,
 * start the 1 Hz heap watch. Call once, early, before serve_start(). */
void blackbox_init(void);

/* Admin-plane handlers (8443): /blackbox = the readable ring, /coredump = the
 * raw coredump partition for espcoredump.py. Registered from serve.c. */
struct httpd_req;
int blackbox_http_dump(struct httpd_req *req);
int blackbox_http_coredump(struct httpd_req *req);
