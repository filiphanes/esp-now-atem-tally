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
void setupATEM();
void atemLoop();