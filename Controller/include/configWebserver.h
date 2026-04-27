#pragma once

#include <Arduino.h>
#include <ETH.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Preferences.h>
#include "espnow.h"
#include "main.h"

#define DEBUG_ETHERNET_WEBSERVER_PORT Serial

#define _ETHERNET_WEBSERVER_LOGLEVEL_ 3

void setupWebserver();
void webserverLoop();
void ws_tally();
