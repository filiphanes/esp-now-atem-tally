#pragma once

#include <Arduino.h>
#include "constants.h"

enum enum_command : uint8_t {
  SET_TALLY = 1,
  GET_TALLY = 2,
  SWITCH_CAMID = 3,
};

// Must match the sender structure
typedef struct struct_message {
  enum_command command;
  uint64_t program = 0;
  uint64_t preview = 0;
} struct_message;

void setupEspNow();
void broadcastTally();
void broadcastTest(int pgm, int pvw);