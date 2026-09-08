#pragma once

#include <Arduino.h>
#include <ETH.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Preferences.h>
#include "espnow.h"
#include "main.h"

#ifdef CONTROLLER_ETH_W5500
// Seeed Studio XIAO W5500 Ethernet Adapter: WIZnet W5500 on the XIAO
// ESP32-S3 SPI bus. Pin numbers per the Seeed wiki wiring of that board:
// CS = D1/GPIO2, SCK = D8/GPIO7, MISO = D9/GPIO8, MOSI = D10/GPIO9.
// IRQ and RST are not wired to the XIAO. PHY address 1.
#include <SPI.h>
#define W5500_SCK      7
#define W5500_MISO     8
#define W5500_MOSI     9
#define W5500_CS       2
#define W5500_IRQ     -1
#define W5500_RST     -1
#define W5500_PHY_ADDR 1
#endif

#define DEBUG_ETHERNET_WEBSERVER_PORT Serial

#define _ETHERNET_WEBSERVER_LOGLEVEL_ 3

void setupWebserver();
void webserverLoop();
void ws_tally();
