#include "atem.h"
#include "espNow.h"
#include "main.h"

ATEMstd AtemSwitcher;
boolean lastAtemIsConnected = false;
long lastMessageAt = 0;

uint64_t getProgramBits()
{
  uint64_t bits = 0;
  for (int i = 1; i <= TALLY_COUNT; i++)
    if (AtemSwitcher.getProgramTally(i))
      bits |= 1 << (i - 1);
  return bits;
}

uint64_t getPreviewBits()
{
  uint64_t bits = 0;
  for (int i = 1; i <= TALLY_COUNT; i++)
    if (AtemSwitcher.getPreviewTally(i))
      bits |= 1 << (i - 1);
  return bits;
}

void setupATEM() {
  Serial.print("setupATEM IP:");
  Serial.println(IPAddress(config.atemIP).toString());
  AtemSwitcher.begin(config.atemIP);
  AtemSwitcher.serialOutput(1);
  AtemSwitcher.connect();
  AtemSwitcher.setAtemTallyCallback(broadcastTally);
}

void atemLoop() {
  AtemSwitcher.runLoop();
  if (AtemSwitcher.isConnected()) {
    if (millis() - lastMessageAt > TALLY_UPDATE_EACH) {
      broadcastLastTally();
      lastMessageAt = millis();
    }
    lastAtemIsConnected = true;
  } else if (lastAtemIsConnected) {
    lastAtemIsConnected = false;
  }
}