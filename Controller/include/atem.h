#pragma once
#include <ATEMbase.h>
#include <ATEMstd.h>
#include "main.h"

// maximum number of ATEM inputs
// because ATEM Arduino Library uses a 64 element array
// for atemTallyByIndexTallyFlags
#define TALLY_COUNT 64
#define TALLY_UPDATE_EACH 2000

extern ATEMstd AtemSwitcher;

uint64_t getProgramBits();
uint64_t getPreviewBits();
void atem_setup();
void atem_loop();

// Cut: put the input straight on program. Auto: set the preview bus then run
// an AUTO transition on M/E 0 (uses the switcher's configured mix time).
void atem_switch_scene(uint8_t tallyNum, bool autoTransition);