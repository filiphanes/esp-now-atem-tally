#pragma once

#include "ArduinoJson.h"
#include "main.h"

#include <esp32-hal-log.h>

#if ESP_ARDUINO_VERSION_MAJOR >= 3
// Arduino core 3.x (pioarduino, IDF 5.x) no longer ships the IDF
// esp_websocket_client component with the framework's prebuilt libraries,
// so the OBS connection uses the WebSockets library client instead (the
// same Links2004/WebSockets dependency that serves the tally UI socket).
#include <WebSocketsClient.h>
#else
#include <esp_websocket_client.h>
#include <esp_log.h>
#endif

void obs_setup();
void obs_loop();
void obs_broadcast_signal(uint64_t bits, uint8_t signal);

// autoTransition=true requires OBS studio mode (set preview + trigger transition).
void obs_switch_scene(uint8_t tallyNum, bool autoTransition);
