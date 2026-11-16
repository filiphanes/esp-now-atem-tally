#include "peers.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"

#include "eth.h"

static const char *TAG = "peers";

#define PEERS_QUERY_MS 800
#define PEERS_REFRESH_MS 8000
#define PEERS_STALE_MS 30000

typedef struct {
    char ip[16];
    char host[64];
    uint16_t port;
    int64_t seen_us;
} peer_t;

static peer_t s_peers[PEERS_MAX];
static SemaphoreHandle_t s_lock;

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t j = 0;
    if (src != NULL) {
        for (size_t i = 0; src[i] != '\0' && j + 1 < cap; i++) {
            unsigned char c = (unsigned char)src[i];
            dst[j++] = (c >= 0x20 && c <= 0x7e && c != '"' && c != '\\') ? (char)c : '?';
        }
    }
    dst[j] = '\0';
}

static uint32_t self_ip(void)
{
    esp_netif_t *netif = eth_netif();
    esp_netif_ip_info_t info;
    if (netif == NULL || esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        return 0;
    }
    return info.ip.addr;
}

static void peers_scan(void)
{
    mdns_result_t *results = NULL;
    esp_err_t qerr = mdns_query_ptr("_tally", "_tcp", PEERS_QUERY_MS,
                                    PEERS_MAX, &results);
    if (qerr != ESP_OK) {
        ESP_LOGW(TAG, "query: %s", esp_err_to_name(qerr));
        return;
    }

    const uint32_t own = self_ip();
    const int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PEERS_MAX; i++) {
        if (s_peers[i].ip[0] != '\0' &&
            now - s_peers[i].seen_us > (int64_t)PEERS_STALE_MS * 1000) {
            s_peers[i].ip[0] = '\0';
        }
    }

    for (mdns_result_t *r = results; r != NULL; r = r->next) {
        char ip[16] = {0};
        for (mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4 &&
                a->addr.u_addr.ip4.addr != own) {
                esp_ip4addr_ntoa(&a->addr.u_addr.ip4, ip, sizeof(ip));
                break;
            }
        }
        if (ip[0] == '\0') {
            continue;
        }

        int slot = -1;
        int free_slot = -1;
        for (int i = 0; i < PEERS_MAX; i++) {
            if (s_peers[i].ip[0] == '\0') {
                if (free_slot < 0) {
                    free_slot = i;
                }
            } else if (strcmp(s_peers[i].ip, ip) == 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            slot = free_slot;
        }
        if (slot < 0) {
            break;
        }
        snprintf(s_peers[slot].ip, sizeof(s_peers[slot].ip), "%s", ip);
        copy_str(s_peers[slot].host, sizeof(s_peers[slot].host), r->hostname);
        s_peers[slot].port = r->port;
        s_peers[slot].seen_us = now;
    }
    xSemaphoreGive(s_lock);

    mdns_query_results_free(results);
}

static void peers_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (self_ip() != 0) {
            peers_scan();
        }
        vTaskDelay(pdMS_TO_TICKS(PEERS_REFRESH_MS));
    }
}

esp_err_t peers_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(peers_task, "peers", 5120, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

size_t peers_snapshot(peer_info_t *out, size_t max)
{
    if (s_lock == NULL || out == NULL || max == 0) {
        return 0;
    }
    size_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PEERS_MAX && n < max; i++) {
        if (s_peers[i].ip[0] == '\0') {
            continue;
        }
        snprintf(out[n].ip, sizeof(out[n].ip), "%.*s",
                 (int)sizeof(out[n].ip) - 1, s_peers[i].ip);
        snprintf(out[n].host, sizeof(out[n].host), "%.*s",
                 (int)sizeof(out[n].host) - 1, s_peers[i].host);
        out[n].port = s_peers[i].port;
        n++;
    }
    xSemaphoreGive(s_lock);
    return n;
}