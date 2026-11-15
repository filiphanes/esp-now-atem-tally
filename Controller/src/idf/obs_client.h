#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// OBS WebSocket client (port of src/obs.cpp).
//
// Connects to OBS Studio's obs-websocket v5 endpoint and derives the tally
// from scene names: a scene called "T3 Camera 1" is input 3, and a name may
// carry several tags ("T1,T2"). Switching finds the scene tagged T<tally> and
// cuts it to program, or (auto_transition) to preview followed by
// TriggerTransition -- that one needs OBS Studio Mode.
//
// The ATEM and vMix clients are driven from the tally task; this one lives
// inside esp_websocket_client's own task instead, and requests from the HTTP
// task go straight through it (the component serialises sends with a mutex).
//
// No authentication, same as the Arduino build: the OBS websocket server must
// accept the connection without a password.

// Starts the client against g_config's target.
esp_err_t obs_start(void);

// Cuts/auto-transitions to the scene tagged T<tally_num>. Logged and dropped
// while the connection is down, like the Arduino build.
void obs_request_switch(uint8_t tally_num, bool auto_transition);

#ifdef __cplusplus
}
#endif
