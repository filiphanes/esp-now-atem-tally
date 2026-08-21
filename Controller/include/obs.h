#pragma once

#include <esp_websocket_client.h>
#include <esp_log.h>
#include <esp32-hal-log.h>

#include "ArduinoJson.h"
#include "main.h"

void obs_setup();
void obs_loop();
void obs_broadcast_signal(uint64_t bits, uint8_t signal);

// autoTransition=true requires OBS studio mode (set preview + trigger transition).
void obs_switch_scene(uint8_t tallyNum, bool autoTransition);