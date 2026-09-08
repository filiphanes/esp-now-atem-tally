#pragma once

#include <esp32-hal-log.h>

#include "main.h"
#include "memory.h"

void vmixServerSetup();
void vmixServerLoop();
void vmix_tally(uint64_t *program, uint64_t *preview);