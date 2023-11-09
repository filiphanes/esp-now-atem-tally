#pragma once

#include <Arduino.h>

boolean atemIsConnected();
void setupAtemConnection();
void atemLoop();
boolean checkForAtemChanges();
String getATEMInformation();
uint64_t getProgramBits();
uint64_t getPreviewBits();
inline uint64_t setBit(uint64_t bits, int i);
inline bool getBit(const uint64_t &bits, int i);