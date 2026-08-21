#pragma once

#include <Arduino.h>

enum switcher_protocol : uint8_t {
    PROTOCOL_ATEM = 1,
    PROTOCOL_OBS  = 2,
    PROTOCOL_VMIX = 3,
};

struct controller_config {
    switcher_protocol protocol = PROTOCOL_ATEM;
    IPAddress ip = {192, 168, 88, 240};
    uint16_t port = 9910;
    uint8_t group = 1;
    uint32_t bg = 0x000000;
    String user;
    String password;
    // Connectionless ESP-NOW power-save tunables.
    // The controller bursts each tally change (tally_burst_ms long, one packet
    // every tally_burst_gap ms) for reliability, then sends SLEEP(sleep_ms) so
    // receivers may power down their radio. sleep_ms == 0 disables sleeping
    // (receivers stay always-on). The controller stays silent for sleep_ms so
    // it does not waste packets a sleeping receiver cannot hear.
    uint16_t sleep_ms        = 0;    // ms receivers may sleep after a burst (0 => always-on, default)
    uint16_t tally_burst_ms  = 300;  // ms, >= a few * tally_burst_gap
    uint16_t tally_burst_gap = 20;   // ms, <= tally_burst_ms
};

extern struct controller_config config;

String readConfig();
void writeConfig(const String& s);
String readPowerConfig();
void writePowerConfig(const String& s);
void parsePowerConfig();            // clamp + populate config.* from NVS
void applyPowerConfigStr(const String& s);  // clamp + populate from a string
String powerConfigStr();            // serialize current config.* tunables
