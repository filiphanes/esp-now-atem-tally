#pragma once
#include <Arduino.h>
#include "main.h"

// maximum number of ATEM inputs tracked (one TlIn tally packet carries up to
// 64 per-input flag bytes)
#define TALLY_COUNT 64
#define TALLY_UPDATE_EACH 2000

bool atem_isConnected();
uint64_t getProgramBits();
uint64_t getPreviewBits();
void atem_setup();
void atem_loop();

// Cut: put the input straight on program. Auto: set the preview bus then run
// an AUTO transition on M/E 0 (uses the switcher's configured mix time).
void atem_switch_scene(uint8_t tallyNum, bool autoTransition);
