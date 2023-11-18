#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include "constants.h"

enum enum_command : uint8_t {
  SET_TALLY = 1,
  GET_TALLY = 2,
  SWITCH_CAMID = 3,
  HEARTBEAT = 4,
  SET_ID = 5,
  SET_COLOR = 6,
  SET_BRIGHTNESS = 7,
};

typedef struct esp_now_tally_info {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    uint8_t id;
    unsigned long last_seen;
} esp_now_tally_info_t;

esp_now_tally_info_t * get_tallies();
void setupEspNow();
void broadcastBrightness(uint8_t id, uint8_t brightness);
void broadcastColor(uint8_t r, uint8_t g, uint8_t b, uint64_t *bits);
void broadcastTally();
void broadcastTally(uint64_t *program, uint64_t *preview);
void broadcastTest(int pgm, int pvw);
