#include <Arduino.h>
#include "espnow.h"
#include "osc.h"

#define OSC_PORT 57110
#define MAX_OSC_PACKET_SIZE 512

WiFiUDP oscUDP;
char oscBuffer[MAX_OSC_PACKET_SIZE];

inline uint64_t bitn(uint8_t n) {
  return (uint64_t)1 << (n-1);
}

uint64_t parseOscAddress(const char* addr, int len) {
  uint64_t bits = 0;
  int tallyNum = 0;
  bool rangeMode = false;
  int rangeStart = 0;

  for (int i = 0; i < len; i++) {
    char c = addr[i];
    if (c >= '1' && c <= '9') {
      tallyNum = tallyNum * 10 + (c - '0');
    } else if (c == ',') {
      if (tallyNum > 0 && tallyNum <= 64) bits |= bitn(tallyNum);
      tallyNum = 0;
      rangeMode = false;
    } else if (c == '-') {
      if (tallyNum > 0 && tallyNum <= 64) {
        rangeStart = tallyNum;
        rangeMode = true;
        tallyNum = 0;
      }
    } else if (c == '{') {
      rangeStart = tallyNum;
      tallyNum = 0;
      rangeMode = true;
    } else if (c == '}') {
      if (rangeStart > 0 && tallyNum > rangeStart) {
        for (int j = rangeStart; j <= tallyNum && j <= 64; j++) {
          bits |= bitn(j);
        }
      }
      tallyNum = 0;
      rangeStart = 0;
      rangeMode = false;
    } else if (c == '/') {
      if (tallyNum > 0 && tallyNum <= 64) bits |= bitn(tallyNum);
      tallyNum = 0;
    }
  }
  if (tallyNum > 0 && tallyNum <= 64) {
    if (rangeMode && rangeStart > 0) {
      for (int j = rangeStart; j <= tallyNum && j <= 64; j++) {
        bits |= bitn(j);
      }
    } else {
      bits |= bitn(tallyNum);
    }
  }
  return bits;
}

int32_t getOscInt32(const char* p) {
  int32_t val;
  memcpy(&val, p, 4);
  return val;
}

void osc_handleMessage(const char* addr, int addrLen, const char* types, int typeLen, const char* args) {
  if (addrLen < 2) return;

  if (strncmp(addr, "/tally", 6) == 0) {
    uint64_t programBits = 0;
    uint64_t previewBits = 0;

    for (int i = 0; i < typeLen; i++) {
      if (types[i] == 'i' || types[i] == 'h') {
        int32_t val = getOscInt32(args + (i * 4));
        if (val == 0) continue;
        if (val > 0 && val <= 64) {
          if (i == 0) programBits |= bitn(val);
          else previewBits |= bitn(val);
        }
      } else if (types[i] == 'T') {
        if (i == 0) programBits |= bitn(i + 1);
        else previewBits |= bitn(i + 1);
      }
    }

    if (programBits || previewBits) {
      espnow_tally(&programBits, &previewBits);
    }
  } else if (strncmp(addr, "/program", 8) == 0) {
    uint64_t programBits = 0;
    for (int i = 0; i < typeLen; i++) {
      if (types[i] == 'i' || types[i] == 'h') {
        int32_t val = getOscInt32(args + (i * 4));
        if (val > 0 && val <= 64) programBits |= bitn(val);
      }
    }
    if (programBits) {
      espnow_tally(&programBits, (uint64_t*)0);
    }
  } else if (strncmp(addr, "/preview", 8) == 0) {
    uint64_t previewBits = 0;
    for (int i = 0; i < typeLen; i++) {
      if (types[i] == 'i' || types[i] == 'h') {
        int32_t val = getOscInt32(args + (i * 4));
        if (val > 0 && val <= 64) previewBits |= bitn(val);
      }
    }
    if (previewBits) {
      espnow_tally((uint64_t*)0, &previewBits);
    }
  } else if (strncmp(addr, "/color", 6) == 0) {
    uint64_t bits = parseOscAddress(addr + 6, addrLen - 6);
    if (bits == 0) return;
    espnow_color(0xFF0000, &bits);
  } else if (strncmp(addr, "/brightness", 10) == 0 || strncmp(addr, "/brightness/", 11) == 0) {
    uint64_t bits = parseOscAddress(addr + 10, addrLen - 10);
    if (bits == 0) return;
    uint8_t brightness = 255;
    if (typeLen > 0 && types[0] == 'i') {
      brightness = getOscInt32(args) & 0xFF;
    }
    espnow_brightness(brightness, &bits);
  } else if (strncmp(addr, "/id", 3) == 0) {
    uint64_t bits = parseOscAddress(addr + 3, addrLen - 3);
    if (bits == 0) return;
    uint8_t camId = 1;
    if (typeLen > 0 && types[0] == 'i') {
      camId = getOscInt32(args) & 0xFF;
    }
    espnow_camid(camId, &bits);
  }
}

void osc_parsePacket() {
  int len = oscUDP.parsePacket();
  if (len == 0) return;

  len = min(len, MAX_OSC_PACKET_SIZE - 1);
  len = oscUDP.read(oscBuffer, len);
  if (len <= 0) return;
  oscBuffer[len] = '\0';

  int i = 0;
  while (i < len && oscBuffer[i] != '\0') i++;
  int addrLen = i;

  int padLen = addrLen + (4 - (addrLen % 4)) % 4;
  if (padLen >= len - 1) return;

  i = padLen + 1;
  while (i < len && oscBuffer[i] != '\0') i++;
  int typeLen = i - padLen - 1;

  int typePadLen = typeLen + (4 - (typeLen % 4)) % 4;
  const char* args = oscBuffer + padLen + 1 + typePadLen;

  osc_handleMessage(oscBuffer, addrLen, oscBuffer + padLen + 1, typeLen, args);
}

void osc_setup() {
  oscUDP.begin(OSC_PORT);
  Serial.printf("OSC listening on port %d\n", OSC_PORT);
}

void osc_loop() {
  osc_parsePacket();
}
