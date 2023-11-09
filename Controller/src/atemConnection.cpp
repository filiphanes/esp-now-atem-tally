#include "atemConnection.h"

#include <Arduino.h>
#include <SkaarhojPgmspace.h>
#include <ATEMbase.h>
#include <ATEMstd.h>

#include "memory.h"
#include "constants.h"

ATEMstd AtemSwitcher;

int PreviewTallyPrevious = 1;
int ProgramTallyPrevious = 1;

boolean atemIsConnected()
{
  return AtemSwitcher.isConnected();
}

void setupAtemConnection()
{
  // Initialize a connection to the switcher
  AtemSwitcher.begin(atemIP);
  AtemSwitcher.serialOutput(1);
  AtemSwitcher.connect();
}

void atemLoop()
{
   AtemSwitcher.runLoop();
}

boolean checkForAtemChanges()
{
  int ProgramTally = AtemSwitcher.getProgramInput();
  int PreviewTally = AtemSwitcher.getPreviewInput();

  if ((ProgramTallyPrevious != ProgramTally) || (PreviewTallyPrevious != PreviewTally))
  {
    Serial.print("Program: ");
    Serial.println(ProgramTally);
    Serial.print("Preview: ");
    Serial.println(PreviewTally);
    ProgramTallyPrevious = ProgramTally;
    PreviewTallyPrevious = PreviewTally;
    return true;
  }
  return false;
}

inline bool getBit(const uint64_t &bits, int i) {
  return bits & (1 << i);
}

inline uint64_t setBit(uint64_t bits, int i) {
  return bits | (1 << i);
}

uint64_t getProgramBits()
{
  uint64_t bits = 0;
  for (int i=1; i <= TALLY_COUNT; i++)
    if (AtemSwitcher.getProgramTally(i))
      bits |= 1 << (i-1);
  return bits;
}

uint64_t getPreviewBits()
{
  uint64_t bits = 0;
  for (int i=1; i <= TALLY_COUNT; i++)
    if (AtemSwitcher.getPreviewTally(i))
      bits |= 1 << (i-1);
  return bits;
}

String getATEMInformation()
{
  String s = F("ATEM Information:\n\n");
  s += F("getProgramInput: ");
  s += String(AtemSwitcher.getProgramInput());
  s += F("\ngetPreviewInput: ");
  s += String(AtemSwitcher.getPreviewInput());
  for (int i=1; i<21; i++)
  {
    s += "\ngetProgramTally " + String(i) + F(": ");
    s += String(AtemSwitcher.getProgramTally(i));
    s += "\ngetPreviewTally " + String(i) + F(": ");
    s += String(AtemSwitcher.getPreviewTally(i));
  }
  s += F("\ngetTransitionPosition: ");
  s += String(AtemSwitcher.getTransitionPosition());
  s += F("\ngetTransitionPreview: ");
  s += String(AtemSwitcher.getTransitionPreview());
  s += F("\ngetTransitionType: ");
  s += String(AtemSwitcher.getTransitionType());
  s += F("\ngetTransitionFramesRemaining0: ");
  s += String(AtemSwitcher.getTransitionFramesRemaining(0));
  s += F("\ngetTransitionMixTime: ");
  s += String(AtemSwitcher.getTransitionMixTime());
  s += F("\ngetFadeToBlackState: ");
  s += String(AtemSwitcher.getFadeToBlackState());
  s += F("\ngetFadeToBlackFrameCount: ");
  s += String(AtemSwitcher.getFadeToBlackFrameCount());
  s += F("\ngetFadeToBlackTime: ");
  s += String(AtemSwitcher.getFadeToBlackTime());

  return s;
}