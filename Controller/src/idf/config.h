#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

// NVS-backed controller settings, ported from the Arduino build.
//
// The protocol URI is stored verbatim in namespace "tally", key "cfg", and the
// ESP-NOW power tunables under key "power". These are the same namespace/key
// names Preferences uses in the Arduino build, so a board keeps its settings
// when it moves between the two firmwares.
#define CONFIG_STR_MAX 2048

typedef enum {
    PROTOCOL_ATEM = 1,
    PROTOCOL_OBS = 2,
    PROTOCOL_VMIX = 3,
} switcher_protocol_t;

typedef struct {
    switcher_protocol_t protocol;
    esp_ip4_addr_t ip;
    uint16_t port;
    uint8_t group;   // kept for parity with the Arduino struct (unused)
    uint32_t bg;     // tally page background, parsed from ?bg= (unused)
} controller_config_t;

// Defaults: ATEM at 192.168.88.240:9910, matching the Arduino build.
extern controller_config_t g_config;

// Loads "cfg" from NVS, takes its first line and parses it into g_config.
// A missing or blank value leaves the defaults untouched.
void config_load(void);

// Parses "atem://192.168.88.240:9910", "obs://host:4455?bg=101010",
// "vmix://host:8099" and bare "host[:port]" strings, like parseUri() in the
// Arduino build. Invalid addresses keep the previous value.
void config_parse_uri(controller_config_t *cfg, const char *uri);

// Raw NVS access. get returns ESP_ERR_NOT_FOUND when the key has never been
// written and ESP_ERR_NVS_INVALID_LENGTH when cap is too small for it.
esp_err_t config_get_raw(char *buf, size_t cap);
esp_err_t config_set_raw(const char *str);
esp_err_t config_get_power_raw(char *buf, size_t cap);
esp_err_t config_set_power_raw(const char *str);

#ifdef __cplusplus
}
#endif
