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
};

extern struct controller_config config;

String readConfig();
void writeConfig(const String& s);
