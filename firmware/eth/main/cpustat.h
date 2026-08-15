/* cpustat.h - live per-task, per-core CPU utilisation on the admin plane.
 * GET https://<node>:8443/cpu  (never forwarded; LAN only). */
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

void cpustat_start(void);              /* start the background sampler */
esp_err_t cpustat_http(httpd_req_t *req);
