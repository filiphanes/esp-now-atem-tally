#include <ETH.h>
#include <WiFiClient.h>
#include "espnow.h"
#include "vmix.h"

WiFiClient tcpclient;
static char buffer[1024];
static size_t bufferLen = 0;

inline uint64_t bitn(uint8_t n) {
  return (uint64_t)1 << (n-1);
}

static bool vmix_connect() {
  Serial.printf("VMIX connecting %s:%u\n", config.ip.toString().c_str(), config.port);
  if (!tcpclient.connect(config.ip, config.port)) return false;
  tcpclient.print("SUBSCRIBE TALLY\r\n");
  Serial.println("VMIX connected");
  return true;
}

void vmix_setup() {
  Serial.print("VMIX IP:");
  Serial.println(config.ip.toString());
  vmix_connect();
}

// Handle one complete line from vMix.
static void vmix_handle_line(const char *line) {
  if (strncmp(line, "TALLY OK ", 9) == 0) {
    // Each following character is one input: '0' off, '1' program,
    // '2' preview, '3' both. First char = input 1 = bit 0.
    const char *t = line + 9;
    uint64_t pgm = 0, pvw = 0;
    for (uint8_t i = 0; i < 64 && t[i] >= '0' && t[i] <= '3'; i++) {
      if (t[i] & 1) pgm |= bitn(i + 1);
      if (t[i] & 2) pvw |= bitn(i + 1);
    }
    programBits = pgm;
    previewBits = pvw;
    espnow_tally(&programBits, &previewBits);
  } else if (strncmp(line, "SUBSCRIBE OK TALLY", 18) == 0) {
    Serial.println("SUBSCRIBE OK TALLY");
  }
}

// Reconnect backoff so a down vMix host is not hammered every ~20ms tick.
// Starts at 1s, doubles up to 30s, resets on a successful connect.
static unsigned long nextRetryAt = 0;
static unsigned long retryBackoffMs = 1000;
static const unsigned long RETRY_BACKOFF_MAX_MS = 30000;

void vmix_loop() {
  // Non-blocking line assembly. Never use readBytesUntil() here: with no
  // newline buffered it stalls for the Stream timeout (default 1s), which
  // freezes the whole main loop (ESP-NOW bursts, OSC, webserver).
  while (tcpclient.available()) {
    char c = tcpclient.read();
    if (c == '\n') {
      buffer[bufferLen] = '\0';
      vmix_handle_line(buffer);
      bufferLen = 0;
    } else if (c != '\r' && bufferLen < sizeof(buffer) - 1) {
      buffer[bufferLen++] = c;
    }
  }

  if (!tcpclient.connected()) {
    unsigned long now = millis();
    if ((long)(now - nextRetryAt) >= 0) {
      if (vmix_connect()) {
        retryBackoffMs = 1000;   // recovered: retry immediately next time
        nextRetryAt = now;
      } else {
        Serial.printf("VMIX connect failed, retrying in %lu s\n", retryBackoffMs / 1000);
        nextRetryAt = now + retryBackoffMs;
        retryBackoffMs = min<unsigned long>(retryBackoffMs * 2, RETRY_BACKOFF_MAX_MS);
      }
    }
  }
}
