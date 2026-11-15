#pragma once

#include <stdint.h>

#include "esp_err.h"

// WebSocket tally feed, matching the Arduino build's port-81 protocol:
// a text frame "TALLY <64 chars>" where each char is the per-tally state
// ('0' none, '1' program, '2' preview, '3' both). Trailing all-zero entries
// are omitted, exactly like ws_tally() in src/configWebserver.cpp.
esp_err_t ws_start(void);

// Replaces the current tally bits and pushes them to every connected client.
// Safe to call from any task; the transmit itself is handed to the HTTP
// server's work queue.
void ws_set_tally(uint64_t program_bits, uint64_t preview_bits);

// Broadcasts an Arduino-style "<KEY> <value> <csv>" frame (CAMID, COLOR,
// BRIGHTNESS, SIGNAL). The web tally page uses these to update its local
// display when a director changes a setting.
void ws_broadcast_set(const char *key, const char *value, const char *csv);
