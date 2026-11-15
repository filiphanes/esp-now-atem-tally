#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// vMix TCP client (port of vmix.cpp): subscribes to a vMix host and turns its
// "TALLY OK ..." lines into the shared program/preview state.
//
// Runs in its own task with a 1 s -> 30 s reconnect backoff, like the Arduino
// build's vmix_loop(); the difference is that a dead host can no longer stall
// anything else, because nothing here blocks another task.

// Starts the client task against g_config's target.
esp_err_t vmix_start(void);

#ifdef __cplusplus
}
#endif
