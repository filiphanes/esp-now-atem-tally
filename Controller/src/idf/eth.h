#pragma once

#include "esp_err.h"
#include "esp_netif.h"

// Seeed Studio XIAO W5500 Ethernet Adapter wiring (same map as the Arduino
// build in include/configWebserver.h): CS = D1/GPIO2, SCK = D8/GPIO7,
// MISO = D9/GPIO8, MOSI = D10/GPIO9. RST is not wired. INTn is not wired on
// the "Early Build" adapters but is from V1.2 on (pad D16 = GPIO10) -- see
// the XIAO_W5500_INT_GPIO block in eth.c.
#define XIAO_W5500_SCK   7
#define XIAO_W5500_MISO  8
#define XIAO_W5500_MOSI  9
#define XIAO_W5500_CS    2

// Brings up the SPI bus, the W5500 MAC/PHY pair, the lwIP netif and DHCP.
// safe to call once from app_main() after esp_netif_init() and
// esp_event_loop_create_default().
esp_err_t eth_w5500_start(void);

// Netif created for the W5500, valid after eth_w5500_start() succeeds.
esp_netif_t *eth_w5500_netif(void);
