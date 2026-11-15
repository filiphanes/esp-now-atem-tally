#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Minimal ATEM client for the pure-IDF controller, ported from src/atem.cpp.
//
// Speaks the Blackmagic ATEM UDP protocol on the configured switcher port:
// handshake, packet ACKs, missed-init-packet recovery, "TlIn" tally by index
// (pushed downstream via espnow_tally()) and "CPvI"/"CPgI"/"DAut" switching on
// M/E 0. All UDP I/O happens in the client task atem_setup() starts.

// Opens the socket, sends the handshake hello to g_config's target and starts
// the client task. That task blocks on the socket, so an incoming tally packet
// is handled the moment it arrives; the reconnect watchdog and pending switch
// requests run while it waits.
esp_err_t atem_setup(void);

// True once the switcher has answered the hello handshake.
bool atem_is_connected(void);

// Queues a CUT (auto_transition = false, straight to program) or an AUTO
// transition (preview bus + AUTO transition on M/E 0) to `tally_num`.
// Performed by the client task, so the HTTP task never touches the UDP socket.
void atem_request_switch(uint8_t tally_num, bool auto_transition);

#ifdef __cplusplus
}
#endif
