#pragma once

#include "esp_err.h"

// Starts the HTTP server on port 80 and registers the embedded UI pages plus
// the read-only JSON/text endpoints the pages poll at load time.
esp_err_t web_start(void);
