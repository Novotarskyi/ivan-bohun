#pragma once
#include <stdbool.h>
#include <stdint.h>

void serve_start(void);

/* True once the public TLS server is listening. The mask is not claimed before
 * this: a leader that holds the address but cannot complete a handshake is a
 * black hole - exactly what a boot race would produce. */
bool serve_is_ready(void);
bool serve_is_real(void);   /* true only in a build that HAS the server */

/* How many of each HTTP error we have answered with. Indexed by httpd_err_code_t
 * (13 codes today); kept private to the LAN - the public page never shows it. */
#define SERVE_ERR_KINDS 13

/* One size for the X-Bohun-Key buffer, everywhere it is read. It was 40 in
 * ota.c and 48 in serve.c: a key longer than 39 bytes would have been silently
 * truncated by the OTA door (httpd returns ESP_ERR_HTTPD_RESULT_TRUNC) and the
 * node would refuse every release while /reset still worked. */
#define BOHUN_KEY_BUF 64

/* THE CONVERSION LEDGER's shape. The heartbeat (v16) carries these counters
 * POSITIONALLY, so this order is a wire contract - append, never reorder:
 * savelife, prytula, hospitallers, u24, trust, nomenclature. */
#define SERVE_CLICK_KINDS 6
const uint32_t *serve_click_counts(void);   /* weak zeros on observers */
const uint16_t *serve_err_counts(void);
void serve_reset_errors(void);
