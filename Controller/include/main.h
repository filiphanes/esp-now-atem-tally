#pragma once

#include <Arduino.h>

struct controller_config {
    // 1 = atem
    // 2 = obs
    uint8_t protocol = 1;
    uint32_t atemIP = (192<<24)+(168<<16)+(2<<8)+240;
    uint16_t atemPort = 9910;
    uint32_t obsIP = (192<<24)+(168<<16)+(2<<8)+18;
    uint16_t obsPort = 4455;
    uint8_t group = 1;
};

extern struct controller_config config;

void readConfig();
void writeConfig();