#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Task watchdog: a hang in any subscribed task panics the chip and resets it,
// the ESP-IDF equivalent of enableLoopWDT() for the Arduino build's loop().
//
// CONFIG_ESP_TASK_WDT_INIT already starts the watchdog during startup, so this
// only pins the timeout/panic policy and lets each task subscribe and feed. A
// subscribed task must get back to its loop at least every WATCHDOG_TIMEOUT_MS
// -- anything that waits indefinitely has to bound that wait first.
#define WATCHDOG_TIMEOUT_MS 5000

// Configures the watchdog policy. Call once from app_main().
esp_err_t watchdog_start(void);

// Subscribes the calling task. Call once at the top of the task function.
void watchdog_subscribe(void);

// Marks the calling task as healthy. Call once per loop iteration.
void watchdog_feed(void);

#ifdef __cplusplus
}
#endif
