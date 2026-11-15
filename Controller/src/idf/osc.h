#pragma once

#include "esp_err.h"

// OSC UDP listener on port 57110, ported from src/osc.cpp.
esp_err_t osc_start(void);
