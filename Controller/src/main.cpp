#include <Arduino.h>
#include <ATEMbase.h>
#include <ATEMstd.h>
#include <mdns.h>

#include "memory.h"
#include "espNow.h"
#include "configWebserver.h"
#include "constants.h"
#include "main.h"

#define ERROR_LED_PIN 2

ATEMstd AtemSwitcher;
long lastMessageAt = 0;
boolean lastAtemIsConnected = false;

uint64_t getProgramBits() {
  uint64_t bits = 0;
  for (int i=1; i <= TALLY_COUNT; i++)
    if (AtemSwitcher.getProgramTally(i))
      bits |= 1 << (i-1);
  return bits;
}

uint64_t getPreviewBits() {
  uint64_t bits = 0;
  for (int i=1; i <= TALLY_COUNT; i++)
    if (AtemSwitcher.getPreviewTally(i))
      bits |= 1 << (i-1);
  return bits;
}

void setup() {
  Serial.begin(115200);
  while (!Serial)
    delay(5);

  // Set LED pin to output mode
  // pinMode(ERROR_LED_PIN, OUTPUT);
  // digitalWrite(ERROR_LED_PIN, HIGH);
  Serial.print("readAtemIP");
  readAtemIP();  // Comment if default 192.168.2.240 is needed
  Serial.print("atemIP: ");
  Serial.println(atemIP.toString());
  Serial.println("setupWebserver");
  setupWebserver();
  Serial.println("setupEspNow");
  setupEspNow();
  
  if (esp_err_t err = mdns_init()) {
      Serial.printf("MDNS Init failed: %d\n", err);
  }
  mdns_hostname_set("tally");
  mdns_instance_name_set("Tally bridge");
  mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

  Serial.println("setupAtemConnection");
  AtemSwitcher.begin(atemIP);
  AtemSwitcher.serialOutput(1);
  AtemSwitcher.connect();
  AtemSwitcher.setAtemTallyCallback(broadcastTally);
}

void loop() {
  webserverLoop();
  AtemSwitcher.runLoop();

  if (AtemSwitcher.isConnected()) {
    if (millis() - lastMessageAt > TALLY_UPDATE_EACH) {
      broadcastTally();
      lastMessageAt = millis();
    }
    digitalWrite(ERROR_LED_PIN, LOW);
    lastAtemIsConnected = true;
  } else if (lastAtemIsConnected) {
    digitalWrite(ERROR_LED_PIN, HIGH);
    lastAtemIsConnected = false;
  }

  delay(20);
}
