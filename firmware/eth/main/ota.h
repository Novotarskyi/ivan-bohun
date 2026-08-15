#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

esp_err_t ota_post_handler(httpd_req_t *req);
/* the shared admin-door check - one key, one comparison, no second copy */
bool ota_key_ok(const char *given);
