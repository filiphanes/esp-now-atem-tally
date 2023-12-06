#include <Arduino.h>
#include <ATEMbase.h>
#include <ATEMstd.h>
#include <EEPROM.h>
#include <mdns.h>

#include "espNow.h"
#include "configWebserver.h"
#include "atem.h"
// #include "obs.h"

struct controller_config config;

void readConfig()
{
  Serial.println("readConfig");
  EEPROM.begin(512);
  EEPROM.get(0, config);
  if (config.atemIP == 0xFFFFFFFF) {
    // Set defaults
    config.protocol = 1;
    // config.atemIP = IPAddress(192,168,2,240);
    config.atemIP = (uint32_t)IPAddress(192,168,88,240);
    config.atemPort = 9910;
    // config.obsIP = IPAddress(192,168,2,24);
    config.obsIP = (uint32_t)IPAddress(192,168,88,21);
    config.obsPort = 4455;
  }
  EEPROM.end();
}	

void writeConfig()
{
  Serial.println("writeConfig");
  EEPROM.begin(512);
  EEPROM.put(0, config);
  EEPROM.commit();
  EEPROM.end();
}

void setup()
{
  Serial.begin(115200);
  while (!Serial)
    delay(5);

  // Set LED pin to output mode
  // pinMode(ERROR_LED_PIN, OUTPUT);
  // digitalWrite(ERROR_LED_PIN, HIGH);
  readConfig();
  Serial.printf("protocol=%d\n", config.protocol);
  setupWebserver();
  setupEspNow();
  if (esp_err_t err = mdns_init()) {
    Serial.printf("MDNS Init failed: %d\n", err);
  }
  mdns_hostname_set("tally");
  mdns_instance_name_set("TallyBridge");
  mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

  if (config.protocol == 1) setupATEM();
  // else if (config.protocol == 2) setupOBS();
}

void loop()
{
  if (config.protocol == 1) atemLoop();
  // else if (config.protocol == 2) obsLoop();
  webserverLoop();
  delay(20);
}
