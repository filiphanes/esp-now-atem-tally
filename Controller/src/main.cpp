#include <Arduino.h>
#include <ATEMbase.h>
#include <ATEMstd.h>
#include <Preferences.h>
#include <mdns.h>

#include "espnow.h"
#include "configWebserver.h"
#include "atem.h"
#include "obs.h"
#include "vmix.h"
#include "vmixServer.h"
#include "osc.h"

struct controller_config config;

void parseUri(String s) {
  if (s.length() == 0) return;

  if (s.startsWith("atem://")) {
    config.protocol = PROTOCOL_ATEM;
    config.port =  9910;  // default atem port
    s.remove(0, 7);
  } else if (s.startsWith("obs://")) {
    config.protocol = PROTOCOL_OBS;
    config.port = 4455;  // default obs ws port
    s.remove(0, 6);
  } else if (s.startsWith("vmix://")) {
    config.protocol = PROTOCOL_VMIX;
    config.port = 8099;  // default vmix port
    s.remove(0, 7);
  }
  Serial.println("uri rest: ");
  Serial.println(s);

  int colonPos = s.lastIndexOf(':');
  int queryPos = s.indexOf('?');
  if (queryPos > 0) {
    String qs = s.substring(queryPos + 1);
    s.remove(queryPos);
    if (qs.startsWith("bg=")) {
      qs.remove(0, 3);
      config.bg = strtol(qs.c_str(), NULL, 16);
    }
  }

  if (colonPos > 0) {
    config.ip.fromString(s.substring(0, colonPos));
    config.port = s.substring(colonPos + 1).toInt();
  } else {
    config.ip.fromString(s);
  }
}

void parseConfig() {
  String s = readConfig();
  if (s.length() == 0) return;
  int newlineIdx = s.indexOf('\n');
  if (newlineIdx > 0) s = s.substring(0, newlineIdx);
  s.trim();
  parseUri(s);
}

String readConfig() {
  Serial.printf("readConfig=");
  Preferences prefs;
  prefs.begin("tally", false);
  String s = prefs.getString("cfg", "");
  prefs.end();
  Serial.println(s);
  return s;
}

void writeConfig(const String& s) {
  Serial.printf("saveConfig");
  Preferences prefs;
  prefs.begin("tally", false);
  prefs.putString("cfg", s);
  prefs.end();
  Serial.println(s);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(5);

  setupWebserver();
  vmixServerSetup();
  osc_setup();
  espnow_setup();

  if (esp_err_t err = mdns_init()) {
    Serial.printf("MDNS Init failed: %d\n", err);
  }
  mdns_hostname_set("tally");
  mdns_instance_name_set("TallyBridge");
  mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

  parseConfig();
  if (config.protocol == PROTOCOL_ATEM) atem_setup();
  else if (config.protocol == PROTOCOL_OBS) obs_setup();
  else if (config.protocol == PROTOCOL_VMIX) vmix_setup();
}

void loop() {
  if (config.protocol == PROTOCOL_ATEM) atem_loop();
  else if (config.protocol == PROTOCOL_OBS) obs_loop();
  else if (config.protocol == PROTOCOL_VMIX) vmix_loop();
  osc_loop();
  vmixServerLoop();
  webserverLoop();
  delay(20);
}
