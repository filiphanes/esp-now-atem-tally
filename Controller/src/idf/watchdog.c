#include "watchdog.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "watchdog";

esp_err_t watchdog_start(void)
{
    esp_task_wdt_config_t cfg = {
        .timeout_ms = WATCHDOG_TIMEOUT_MS,
        // Keep the idle tasks subscribed too: a task spinning at 100% CPU
        // starves them, and that is as fatal as a deadlock.
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true,
    };

    // The TWDT is normally already running, so this is a reconfigure; only
    // fall back to a cold init if CONFIG_ESP_TASK_WDT_INIT is off.
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_init(&cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "watchdog init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "task watchdog armed (%u ms, panic on timeout)",
             (unsigned)WATCHDOG_TIMEOUT_MS);
    return ESP_OK;
}

void watchdog_subscribe(void)
{
    // ESP_ERR_INVALID_STATE just means the task is subscribed already.
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "subscribe failed: %s", esp_err_to_name(err));
    }
}

void watchdog_feed(void)
{
    esp_task_wdt_reset();
}
