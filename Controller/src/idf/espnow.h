#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ESP-NOW tally pump, ported from src/espNow.cpp.
//
// Wire protocol (must match Receiver/main.c):
//   SET_TALLY      [1][program u64][preview u64]
//   GET_TALLY      [2]
//   HEARTBEAT      [4][camId]              receiver -> controller
//   SET_CAMID      [5][camId][bits u64]
//   SET_COLOR      [6][r][g][b][bits u64]
//   SET_BRIGHTNESS [7][brightness][bits u64]
//   SET_SIGNAL     [n][bits u64]           n is the signal number itself
//   SLEEP          [11][dur_lo][dur_hi]
//
// All multi-byte fields are host-endian: both ends are ESP32s.

#define ESPNOW_MAX_TALLY 64
#define ESPNOW_MAC_LEN 6

enum espnow_command {
    ESPNOW_SET_TALLY = 1,
    ESPNOW_GET_TALLY = 2,
    ESPNOW_SWITCH_CAMID = 3,
    ESPNOW_HEARTBEAT = 4,
    ESPNOW_SET_CAMID = 5,
    ESPNOW_SET_COLOR = 6,
    ESPNOW_SET_BRIGHTNESS = 7,
    ESPNOW_SET_SIGNAL = 8,
    // 10 was SET_WAKE (receiver-side implicit duty-cycle, removed).
    ESPNOW_SLEEP = 11,
};

typedef struct {
    uint8_t mac_addr[ESPNOW_MAC_LEN];
    uint8_t id;
    int64_t last_seen_us;
} espnow_tally_info_t;

// Power-save tunables normally persisted in NVS (NVS storage lands with the
// config slice; until then they live in RAM and reset on reboot).
typedef struct {
    uint16_t sleep_ms;
    uint16_t burst_ms;
    uint16_t burst_gap_ms;
} espnow_power_t;

// WiFi STA + LR protocol + ESP-NOW init. Requires nvs_flash_init(),
// esp_netif_init() and the default event loop to be up already.
esp_err_t espnow_setup(void);

// One pump tick. Returns how long the caller may sleep (ms) before the pump
// next has something to do, so the driving task can be event-driven instead of
// ticking on a fixed cadence. Call from one task only.
uint32_t espnow_loop(void);

// Wakes the task running espnow_loop() so a pending tally change goes out
// immediately instead of waiting for its next deadline. Safe from any task.
void espnow_wake(void);

// Current program/preview bitmaps. Either pointer may be NULL.
void espnow_get_tally(uint64_t *program, uint64_t *preview);

// Updates non-NULL bitmaps, pushes them to the WebSocket clients and arms a
// SET_TALLY burst. Mirrors espnow_tally() in the Arduino build.
void espnow_tally(uint64_t *program, uint64_t *preview);

// One-shot receiver commands. `bits` must be non-NULL.
void espnow_brightness(uint8_t brightness, const uint64_t *bits);
void espnow_camid(uint8_t cam_id, const uint64_t *bits);
void espnow_color(uint32_t color, const uint64_t *bits);
void espnow_signal(uint8_t signal, const uint64_t *bits);
void espnow_sleep_all(uint16_t ms);

// Copies up to `max` receiver records and returns how many slots were filled
// (including empty ones; callers skip entries with id == 0).
size_t espnow_tallies_snapshot(espnow_tally_info_t *out, size_t max);

void espnow_get_power(espnow_power_t *out);
// Clamps exactly like applyPowerConfigStr() in the Arduino build.
void espnow_set_power(uint16_t sleep_ms, uint16_t burst_ms, uint16_t gap_ms);
// Parses "ws=..&wb=..&wg=.." with the Arduino build's loose key scan: keys
// that are absent keep their current values, present keys are clamped.
void espnow_apply_power_str(const char *str);

#ifdef __cplusplus
}
#endif
