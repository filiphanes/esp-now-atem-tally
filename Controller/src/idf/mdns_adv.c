// mDNS advertisement for the pure-IDF controller.
//
// Mirrors the Arduino setup in src/main.cpp:
//   mdns_init();
//   mdns_hostname_set("tally");
//   mdns_instance_name_set("TallyBridge");
//   mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
//
// IDF 6.0 no longer ships the mDNS stack in the core tree; it comes from the
// component registry as espressif/mdns (see src/idf_component.yml).

#include "mdns_adv.h"

#include <stdio.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"

static const char *TAG = "mdns";

esp_err_t mdns_advertise_start(void)
{
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns_init failed");
    ESP_RETURN_ON_ERROR(mdns_hostname_set("tally"), TAG,
                        "mdns_hostname_set failed");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set("TallyBridge"), TAG,
                        "mdns_instance_name_set failed");
    ESP_RETURN_ON_ERROR(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0),
                        TAG, "mdns_service_add failed");

    uint8_t mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG,
                        "read MAC failed");
    char instance[32];
    snprintf(instance, sizeof(instance), "Tally %02X%02X", mac[4], mac[5]);
    ESP_RETURN_ON_ERROR(mdns_service_add(instance, "_tally", "_tcp", 80,
                                         NULL, 0),
                        TAG, "tally service add failed");

    ESP_LOGI(TAG, "advertising tally.local (_http._tcp, _tally._tcp:80)");
    return ESP_OK;
}
