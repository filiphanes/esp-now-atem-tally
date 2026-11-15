// Pure ESP-IDF entry point for [env:xiao-w5500-idf].
//
// Slices 1-7: W5500 Ethernet, the embedded UI pages, the WebSocket tally feed
// on port 81, the ESP-NOW tally pump, the OSC listener on UDP 57110, mDNS,
// NVS-backed config/power settings, and the ATEM, vMix and OBS clients (the
// vMix side also serves the tally on TCP 8099).

#include "atem.h"
#include "config.h"
#include "espnow.h"
#include "eth.h"
#include "mdns_adv.h"
#include "obs_client.h"
#include "osc.h"
#include "vmix_client.h"
#include "vmix_server.h"
#include "watchdog.h"
#include "web.h"
#include "ws.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tally";

// Drives the ESP-NOW burst/sleep/keepalive pump. Everything else only updates
// the shared tally state and arms a burst (and calls espnow_wake()), so all
// timing stays in one task. The task sleeps until the pump's next deadline
// instead of ticking on a fixed cadence, so a tally change goes out to the
// receivers immediately rather than at the next tick.
static void pump_task(void *arg)
{
    (void)arg;
    watchdog_subscribe();
    for (;;) {
        watchdog_feed();
        uint32_t sleep_ms = espnow_loop();
        TickType_t ticks = pdMS_TO_TICKS(sleep_ms);
        // pdMS_TO_TICKS() truncates against a 10 ms tick; never spin on 0.
        if (ticks == 0) {
            ticks = 1;
        }
        ulTaskNotifyTake(pdTRUE, ticks);
    }
}

// Connectionless ESP-NOW power-save tunables (NVS key "power"). Mirrors
// parsePowerConfig(): a fresh flash persists the struct defaults so NVS holds
// the key and boot stops logging NOT_FOUND.
static void power_load(void)
{
    static const char *const DEFAULTS = "ws=0&wb=300&wg=20";
    char buf[64];
    esp_err_t err = config_get_power_raw(buf, sizeof(buf));
    if (err == ESP_ERR_NOT_FOUND || (err == ESP_OK && buf[0] == '\0')) {
        ESP_LOGI(TAG, "no saved power config, storing defaults");
        config_set_power_raw(DEFAULTS);
        espnow_apply_power_str(DEFAULTS);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power config read failed: %s", esp_err_to_name(err));
        return;
    }
    espnow_apply_power_str(buf);
}

void app_main(void)
{
    ESP_LOGI(TAG, "tally controller starting (IDF build)");

    // ESP-NOW and the WiFi driver keep state in NVS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Arm the task watchdog before anything long-running starts, so a hung
    // protocol task or pump reboots the controller instead of silently
    // dropping tally.
    ESP_ERROR_CHECK(watchdog_start());

    config_load();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(eth_w5500_start());
    ESP_ERROR_CHECK(mdns_advertise_start());
    ESP_ERROR_CHECK(espnow_setup());
    ESP_ERROR_CHECK(web_start());
    ESP_ERROR_CHECK(ws_start());
    ESP_ERROR_CHECK(osc_start());
    // Serves the tally to vMix-protocol clients whatever the switcher is,
    // exactly like vmixServerSetup() in the Arduino build.
    ESP_ERROR_CHECK(vmix_server_start());

    // Same order as setup() in the Arduino build: config decides which
    // switcher client to start, power tunables come last.
    switch (g_config.protocol) {
    case PROTOCOL_ATEM:
        ESP_ERROR_CHECK(atem_setup());
        break;
    case PROTOCOL_VMIX:
        ESP_ERROR_CHECK(vmix_start());
        break;
    case PROTOCOL_OBS:
        ESP_ERROR_CHECK(obs_start());
        break;
    default:
        ESP_LOGE(TAG, "unknown switcher protocol %d", g_config.protocol);
        break;
    }

    power_load();

    BaseType_t ok = xTaskCreate(pump_task, "pump", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "pump task create failed");
        return;
    }

    ESP_LOGI(TAG, "waiting for DHCP lease on the W5500 link");
}
