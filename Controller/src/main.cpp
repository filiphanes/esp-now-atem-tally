#include <Arduino.h>

#include "memory.h"
#include "atemConnection.h"
#include "espNow.h"
#include "configWebserver.h"
#include "constants.h"

#define ERROR_LED_PIN 2

void setup()
{
  Serial.begin(115200);
  while (!Serial)
    delay(5);

  // Set LED pin to output mode
  // pinMode(ERROR_LED_PIN, OUTPUT);
  // digitalWrite(ERROR_LED_PIN, HIGH);
  Serial.print("readAtemIP()");
  readAtemIP();
  Serial.print("atemIP: ");
  Serial.println(atemIP.toString());
  Serial.println("setupWebserver()");
  setupWebserver();
  Serial.println("setupEspNow()");
  setupEspNow();
  Serial.println("setupAtemConnection()");
  setupAtemConnection();
}

long lastMessageAt = 0;
boolean lastAtemIsConnected = false;

void loop()
{
  webserverLoop();
  atemLoop();

  if (atemIsConnected()) {
    if (checkForAtemChanges() || (millis() - lastMessageAt > TALLY_UPDATE_EACH)) {
      broadcastTally();
      lastMessageAt = millis();
    }
    digitalWrite(ERROR_LED_PIN, LOW);
    lastAtemIsConnected = true;
  } else if (lastAtemIsConnected) {
    digitalWrite(ERROR_LED_PIN, HIGH);
    lastAtemIsConnected = false;
  }

  delay(5);
}