#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

// Registers GET/POST /ota on the UI's httpd instance.
//
// POST takes the raw application image (the .pio build's firmware.bin), streams
// it into the inactive OTA slot, validates it and reboots into it. GET serves a
// minimal upload page. Both boards' Ethernet interfaces work: the handler only
// uses lwIP sockets through the shared httpd.
esp_err_t ota_http_register(httpd_handle_t server);

// Confirms the running image after app_main() has initialized everything, so
// the bootloader does not roll back a healthy update on the next reboot.
void ota_mark_valid(void);