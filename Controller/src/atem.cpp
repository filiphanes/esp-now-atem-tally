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
  Serial.println(IPAddress(config.atemIP).toString());
  AtemSwitcher.begin(config.atemIP);
  AtemSwitcher.serialOutput(1);
  AtemSwitcher.connect();
  AtemSwitcher.setAtemTallyCallback(espnow_tally);
}

void atem_loop() {
  AtemSwitcher.runLoop();
  if (AtemSwitcher.isConnected()) {
    espnow_loop();
    lastAtemIsConnected = true;
  } else if (lastAtemIsConnected) {
    lastAtemIsConnected = false;
  }
}