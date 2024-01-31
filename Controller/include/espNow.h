#pragma once

#include <Arduino.h>
#include <esp_now.h>

extern uint64_t lastProgram;
extern uint64_t lastPreview;

enum enum_command : uint8_t {
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
} esp_now_tally_info_t;

esp_now_tally_info_t * get_tallies();
void setupEspNow();
void broadcastBrightness(uint8_t brightness, uint64_t *bits);
void broadcastCamId(uint8_t camId, uint64_t *bits);
void broadcastColor(uint32_t, uint64_t *bits);
void broadcastSignal(uint8_t signal, uint64_t *bits);
void broadcastLastTally();
void broadcastTally(uint64_t *program, uint64_t *preview);
void broadcastTest(int pgm, int pvw);
