#pragma once
#include <stdbool.h>
#include <stdint.h>

/* THE OTA WINDOW - on-demand WiFi for the radio-only observers.
 *
 * Steady state stays radio-pure. A deliberate touch gesture opens a window:
 * the device joins the flat's WiFi, starts an HTTPS admin server carrying the
 * same key-gated /ota door the blades run (plus /page on the kobzar for its
 * strip partition), shows its address and progress on its own glass, and
 * closes the window on success, cancel, or timeout - dropping WiFi and
 * re-pinning the swarm channel. Requires BOHUN_WIFI_SSID/_PASS in
 * secrets.h; without them the window reports NOCREDS and refuses to open.
 *
 * Rollback: an OTA'd observer boots PENDING_VERIFY. ota_window_boot_trial()
 * (call once from app_main) marks the image valid only after TRIAL_S seconds
 * alive with at least one peer heard on the radio; past DEADLINE_S unmarked,
 * it reboots so the bootloader restores the previous slot. */

typedef enum {
    OTAW_IDLE = 0,     /* radio-only, nothing open */
    OTAW_NOCREDS,      /* trigger refused: no WiFi credentials in secrets.h */
    OTAW_JOINING,      /* WiFi association + DHCP in flight */
    OTAW_READY,        /* server up - waiting for an image */
    OTAW_RECEIVING,    /* image inbound (pct advances) */
    OTAW_FAILED,       /* join failed - window closes shortly */
} ota_window_state_t;

void ota_window_open(void);    /* idempotent; call from the touch trigger */
void ota_window_close(void);   /* cancel; also called internally on timeout */

/* One call for the UIs: state, dotted-quad ip (16 bytes, "" until READY),
 * seconds left in the window, receive percentage. Any out-arg may be NULL. */
ota_window_state_t ota_window_status(char ip[16], int *secs_left, int *pct);

void ota_window_boot_trial(void);   /* pending-verify trial - once, at boot */
