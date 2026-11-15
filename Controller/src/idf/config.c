// NVS-backed settings for the pure-IDF controller.
//
// Mirrors src/main.cpp parseUri()/readConfig()/writeConfig() so the same
// "atem://host:port" config string yields the same protocol/address/port on
// both firmwares. Divergences, both on purpose:
//   * a port outside 1..65535 is ignored instead of stored as 0 (an invalid
//     destination would make the ATEM client unusable until re-configured);
//   * an invalid address is ignored instead of becoming 0.0.0.0.
//
// NVS layout: namespace "tally", string keys "cfg" and "power".

#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"

static const char *TAG = "config";

static const char *NVS_NAMESPACE = "tally";
static const char *KEY_CFG = "cfg";
static const char *KEY_POWER = "power";

controller_config_t g_config = {
    .protocol = PROTOCOL_ATEM,
    .ip = {.addr = ESP_IP4TOADDR(192, 168, 88, 240)},
    .port = 9910,
    .group = 1,
    .bg = 0x000000,
};

static void trim_in_place(char *s)
{
    char *start = s;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static esp_err_t nvs_get_string(const char *key, char *buf, size_t cap)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;   // namespace (or key) never written
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(handle, key, buf, &cap);
    nvs_close(handle);
    // Keep the NVS specifics inside this file: callers only care that the key
    // is absent, which is the generic ESP_ERR_NOT_FOUND.
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

static esp_err_t nvs_set_string(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t config_get_raw(char *buf, size_t cap)
{
    return nvs_get_string(KEY_CFG, buf, cap);
}

esp_err_t config_set_raw(const char *str)
{
    return nvs_set_string(KEY_CFG, str);
}

esp_err_t config_get_power_raw(char *buf, size_t cap)
{
    return nvs_get_string(KEY_POWER, buf, cap);
}

esp_err_t config_set_power_raw(const char *str)
{
    return nvs_set_string(KEY_POWER, str);
}

void config_parse_uri(controller_config_t *cfg, const char *uri)
{
    if (cfg == NULL || uri == NULL) {
        return;
    }

    const char *host = uri;
    if (strncmp(host, "atem://", 7) == 0) {
        cfg->protocol = PROTOCOL_ATEM;
        cfg->port = 9910;
        host += 7;
    } else if (strncmp(host, "obs://", 6) == 0) {
        cfg->protocol = PROTOCOL_OBS;
        cfg->port = 4455;
        host += 6;
    } else if (strncmp(host, "vmix://", 7) == 0) {
        cfg->protocol = PROTOCOL_VMIX;
        cfg->port = 8099;
        host += 7;
    }

    // Query string: the Arduino build only looks at a leading "bg=".
    const char *query = strchr(host, '?');
    if (query != NULL && strncmp(query + 1, "bg=", 3) == 0) {
        cfg->bg = (uint32_t)strtoul(query + 3, NULL, 16);
    }

    // Copy just the "host[:port]" part: the full URI can be CONFIG_STR_MAX
    // bytes and this runs on the main task's stack.
    char hostbuf[64];
    size_t host_len =
        (query != NULL) ? (size_t)(query - host) : strlen(host);
    if (host_len == 0 || host_len >= sizeof(hostbuf)) {
        ESP_LOGW(TAG, "unusable host in \"%s\", keeping previous", uri);
        return;
    }
    memcpy(hostbuf, host, host_len);
    hostbuf[host_len] = '\0';

    // Last colon separates host and port; a leading colon means "no host".
    char *colon = strrchr(hostbuf, ':');
    char *port_str = NULL;
    if (colon != NULL && colon != hostbuf) {
        *colon = '\0';
        port_str = colon + 1;
    }

    esp_ip4_addr_t parsed;
    if (esp_netif_str_to_ip4(hostbuf, &parsed) == ESP_OK) {
        cfg->ip = parsed;
    } else {
        ESP_LOGW(TAG, "invalid IPv4 address \"%s\", keeping previous", hostbuf);
    }

    if (port_str != NULL) {
        long port = strtol(port_str, NULL, 10);
        if (port > 0 && port <= 65535) {
            cfg->port = (uint16_t)port;
        } else {
            ESP_LOGW(TAG, "invalid port \"%s\", keeping %u", port_str,
                     cfg->port);
        }
    }
}

void config_load(void)
{
    // Heap allocated: the string can be CONFIG_STR_MAX bytes and this runs on
    // the main task's stack.
    char *raw = malloc(CONFIG_STR_MAX);
    if (raw == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }

    esp_err_t err = config_get_raw(raw, CONFIG_STR_MAX);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "config read failed: %s", esp_err_to_name(err));
        }
    } else {
        char *newline = strchr(raw, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }
        trim_in_place(raw);
        if (raw[0] != '\0') {
            config_parse_uri(&g_config, raw);
        }
    }
    free(raw);

    char ip[16];
    esp_ip4addr_ntoa(&g_config.ip, ip, sizeof(ip));
    ESP_LOGI(TAG, "switcher protocol=%d target=%s:%u", g_config.protocol, ip,
             g_config.port);
}
