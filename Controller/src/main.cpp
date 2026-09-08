#include <Arduino.h>
#include <esp_task_wdt.h>
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

// ---- Connectionless ESP-NOW power-save tunables (separate NVS key) ----
// Stored as "wi=<int>&ww=<win>&wb=<burst>&wg=<gap>". Kept out of the
// protocol:// config string so the two evolve independently.

static long kvInt(const String& s, const char* key) {
  String pat = String(key) + "=";
  int i = s.indexOf(pat);
  if (i < 0) return -1;
  int j = i + pat.length();
  long v = 0; bool any = false;
  while (j < (int)s.length() && isDigit(s[j])) { v = v * 10 + (s[j] - '0'); any = true; j++; }
  return any ? v : -1;
}

String readPowerConfig() {
  Preferences prefs;
  prefs.begin("tally", false);
  String s = prefs.getString("power", "");
  prefs.end();
  return s;
}

void writePowerConfig(const String& s) {
  Preferences prefs;
  prefs.begin("tally", false);
  prefs.putString("power", s);
  prefs.end();
}

// Coordination contract enforced here so the system is always consistent
// regardless of what the web UI submitted: gap <= burst. sleep_ms is
// unrestricted (0 disables sleeping = receivers always-on).
void applyPowerConfigStr(const String& s) {
  long v;
  v = kvInt(s, "ws"); if (v >= 0) config.sleep_ms        = constrain((uint16_t)v, 0, 60000);
  v = kvInt(s, "wb"); if (v > 0)  config.tally_burst_ms  = constrain((uint16_t)v, 20, 65535);
  v = kvInt(s, "wg"); if (v > 0)  config.tally_burst_gap = constrain((uint16_t)v, 5,   config.tally_burst_ms);
}

void parsePowerConfig() {
  applyPowerConfigStr(readPowerConfig());
}

String powerConfigStr() {
  return "ws=" + String(config.sleep_ms)
       + "&wb=" + String(config.tally_burst_ms)
       + "&wg=" + String(config.tally_burst_gap);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(5);

  // Watchdog the loop task: a hung module (protocol parser, TCP stall,
  // webserver...) reboots the controller instead of silently dropping all
  // tally. Fed from loop() below.
  enableLoopWDT();

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

  // ESP-NOW power-save: load tunables (sleep_ms + burst). Receivers are
  // always-on by default and only sleep when explicitly told via SLEEP after
  // each burst, so there is nothing to push to them at boot.
  parsePowerConfig();
}

void loop() {
  if (config.protocol == PROTOCOL_ATEM) atem_loop();
  else if (config.protocol == PROTOCOL_OBS) obs_loop();
  else if (config.protocol == PROTOCOL_VMIX) vmix_loop();
  espnow_loop();   // drives the SET_TALLY burst pump + periodic refresh
  osc_loop();
  vmixServerLoop();
  webserverLoop();
  delay(20);       // 20ms tick == default burst gap
  esp_task_wdt_reset();   // feed the loop watchdog
}
