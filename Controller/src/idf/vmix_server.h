#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// vMix TCP tally server on port 8099 (port of vmixServer.cpp).
//
// Speaks the vMix TCP protocol so other tools can subscribe to the tally:
//   TALLY              -> "TALLY OK <64 x 0/1/2/3>\r\n"
//   SUBSCRIBE TALLY    -> "SUBSCRIBE OK TALLY\r\n" + current tally, then updates
//   UNSUBSCRIBE TALLY  -> "UNSUBSCRIBE OK TALLY\r\n"
//   anything else      -> "ER\r\n"
//
// Runs for every switcher protocol, exactly like vmixServerSetup() in the
// Arduino build.

// Starts the listener and its task. Safe to call once, at boot.
esp_err_t vmix_server_start(void);

// Pushes the current tally to every subscribed client. Safe from any task:
// only the two bitmaps and a dirty flag are shared, the socket writes happen
// in the server task.
void vmix_server_tally(uint64_t program, uint64_t preview);

#ifdef __cplusplus
}
#endif
