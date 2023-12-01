#include <Arduino.h>
#include <ATEMbase.h>
#include <ATEMstd.h>
#include <EEPROM.h>
#include <mdns.h>

#include "espNow.h"
#include "configWebserver.h"
#include "atem.h"
#include "obs.h"

struct controller_config config;

void readConfig()
{
  EEPROM.begin(512);
  EEPROM.get(0, config);
  if (config.atemIP == IPAddress(255,255,255,255)) {
    // Set defaults
    config.atemIP.fromString("192.168.88.240");
    config.atemPort = 9910;
    config.atemEnabled = true;
    config.obsIP.fromString("192.168.88.24");
    config.obsPort = 4455;
    config.obsEnabled = true;
  }
  EEPROM.end();
}	

void writeConfig()
{
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
  Serial.print("readAtemIP");
  readConfig();
  if (config.atemEnabled) setupATEM();
  if (config.obsEnabled)  setupOBS();

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
}

void loop()
{
  if (config.atemEnabled) atemLoop();
  if (config.obsEnabled)  obsLoop();
  webserverLoop();
  delay(20);
}
