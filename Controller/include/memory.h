#pragma once

#include <Arduino.h>

extern IPAddress atemIP;

void readAtemIP();
void writeAtemIP(String newIp);