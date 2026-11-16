#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

// Starts the HTTP server on port 80 and registers the embedded UI pages plus
// the read-only JSON/text endpoints the pages poll at load time.
esp_err_t web_start(void);

// The running server handle, for ws_start() to attach the tally feed to it.
// NULL until web_start() has succeeded.
httpd_handle_t web_httpd(void);
