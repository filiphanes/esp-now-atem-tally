#include "atem.h"
#include "espnow.h"
#include "main.h"

ATEMstd AtemSwitcher;
boolean lastAtemIsConnected = false;

uint64_t getProgramBits()
{
  uint64_t bits = 0;
  for (int i = 1; i <= TALLY_COUNT; i++)
    if (AtemSwitcher.getProgramTally(i))
      bits |= (uint64_t)1 << (i - 1);
  return bits;
}

uint64_t getPreviewBits()
{
  uint64_t bits = 0;
  for (int i = 1; i <= TALLY_COUNT; i++)
    if (AtemSwitcher.getPreviewTally(i))
      bits |= (uint64_t)1 << (i - 1);
  return bits;
}

void atem_setup() {
  Serial.print("atem_setup IP:");
  Serial.println(config.ip.toString());
  AtemSwitcher.begin(config.ip);
  AtemSwitcher.serialOutput(1);
  AtemSwitcher.connect();
  AtemSwitcher.setAtemTallyCallback(espnow_tally);
}

void atem_switch_scene(uint8_t tallyNum, bool autoTransition) {
  if (tallyNum == 0) return;
  if (!AtemSwitcher.isConnected()) {
    Serial.println("ATEM switch: not connected");
    return;
  }
  if (autoTransition) {
    Serial.printf("ATEM auto -> %u\n", tallyNum);
    AtemSwitcher.changePreviewInput(tallyNum);
    AtemSwitcher.doAuto(0);
  } else {
    Serial.printf("ATEM cut -> %u\n", tallyNum);
    AtemSwitcher.changeProgramInput(tallyNum);
  }
}

void atem_loop() {
  AtemSwitcher.runLoop();
  if (AtemSwitcher.isConnected()) {
    lastAtemIsConnected = true;
  } else if (lastAtemIsConnected) {
    lastAtemIsConnected = false;
  }
}
