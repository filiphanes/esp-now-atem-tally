#pragma once

#include <esp_websocket_client.h>
#include <esp_log.h>
#include <esp32-hal-log.h>

#include "ArduinoJson.h"
#include "main.h"

void setupOBS();
void obsLoop();