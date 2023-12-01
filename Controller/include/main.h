#pragma once

#include <Arduino.h>

struct controller_config {
    IPAddress atemIP = IPAddress(0,0,0,0);
    uint16_t atemPort = 9910;
    bool atemEnabled = true;
    IPAddress obsIP = IPAddress(0,0,0,0);
    uint16_t obsPort = 4455;
    bool obsEnabled = true;
};

extern struct controller_config config;

void readConfig();
void writeConfig();