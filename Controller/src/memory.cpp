#include "memory.h"

#include <EEPROM.h>

IPAddress atemIP(192, 168, 2, 240);

const int EEPROM_ADDRESS_SWITCHER_IP = 0;

void readAtemIP()
{
  EEPROM.begin(512);
  EEPROM.get(EEPROM_ADDRESS_SWITCHER_IP, atemIP);
  if (atemIP.toString() == "255.255.255.255") {
    // Set default ATEM IP
    writeAtemIP("192.168.88.240");
  }
  EEPROM.end();
}	

void writeAtemIP(String newIp)
{
  atemIP.fromString(newIp);
  EEPROM.begin(512);
  EEPROM.put(EEPROM_ADDRESS_SWITCHER_IP, atemIP);
  EEPROM.commit();
  EEPROM.end();
}