#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// A loopback UDP datagram pair that breaks a select()-ing task out of its wait
// from another task's context. The sleeping task select()s rx_fd; any other
// task calls wake_pair_poke() (never blocks); the sleeper drains the pokes
// once awake. The payload bytes are meaningless -- only the readability of
// rx_fd matters.
//
// This used to be copy-pasted in atem.cpp and vmix_server.c. It exists because
// select() cannot be interrupted by FreeRTOS primitives; where no select() is
// involved, xTaskNotifyGive() is the better tool (see espnow_wake()).
typedef struct {
    int rx_fd;   // select() this; -1 when setup failed
    int tx_fd;   // poke this from any task; -1 when setup failed
} wake_pair_t;

// Creates the pair on 127.0.0.1 with both ends nonblocking. Returns ESP_FAIL
// and leaves both fds at -1 when sockets run out. A failed pair costs wake-up
// latency only, never correctness, so callers log and carry on.
esp_err_t wake_pair_init(wake_pair_t *pair);

// Sends one wake byte; a no-op on a failed pair.
void wake_pair_poke(const wake_pair_t *pair);

// Empties rx_fd; call once per wake-up, before re-entering select().
void wake_pair_drain(const wake_pair_t *pair);

#ifdef __cplusplus
}
#endif
