#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

// WebSocket tally feed, matching the Arduino build's wire protocol:
// a text frame "TALLY <up to 64 chars>" where each char is the per-tally state
// ('0' none, '1' program, '2' preview, '3' both). Trailing all-zero entries
// are omitted, exactly like ws_tally() in src/configWebserver.cpp.
//
// The route (/ws) is registered on the UI's httpd instance instead of a
// second server on port 81 like the Arduino build: the pages try /ws first
// and fall back to :81, so one set of pages serves both firmwares.
esp_err_t ws_start(httpd_handle_t server);

// Replaces the current tally bits and pushes them to every connected client.
// Safe to call from any task; the transmit itself is handed to the HTTP
// server's work queue.
void ws_set_tally(uint64_t program_bits, uint64_t preview_bits);

// Broadcasts an Arduino-style "<KEY> <value> <csv>" frame (CAMID, COLOR,
// BRIGHTNESS, SIGNAL). The web tally page uses these to update its local
// display when a director changes a setting.
void ws_broadcast_set(const char *key, const char *value, const char *csv);
