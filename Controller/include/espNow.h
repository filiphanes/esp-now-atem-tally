#pragma once

#include <Arduino.h>
#include <esp_now.h>

extern uint64_t programBits;
extern uint64_t previewBits;
extern long lastMessageTime;

enum espnow_command : uint8_t {
  SET_TALLY = 1,
  GET_TALLY = 2,
  SWITCH_CAMID = 3,
  HEARTBEAT = 4,
  SET_CAMID = 5,
  SET_COLOR = 6,
  SET_BRIGHTNESS = 7,
  SET_SIGNAL = 8,
};

typedef struct esp_now_tally_info {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    uint8_t id;
    unsigned long last_seen;
} espnow_tally_info_t;

espnow_tally_info_t * espnow_tallies();

void espnow_setup();
void espnow_loop();
void espnow_brightness(uint8_t brightness, uint64_t *bits);
void espnow_camid(uint8_t camId, uint64_t *bits);
void espnow_color(uint32_t, uint64_t *bits);
void espnow_signal(uint8_t signal, uint64_t *bits);
void espnow_tally();
void espnow_tally(uint64_t *program, uint64_t *preview);
void espnow_tally_test(int pgm, int pvw);
