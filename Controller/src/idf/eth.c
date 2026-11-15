// W5500 Ethernet bring-up for the Seeed XIAO ESP32-S3 + XIAO W5500 adapter.
//
// Pure ESP-IDF equivalent of the Arduino ETH.begin(ETH_PHY_W5500, ...) call in
// src/configWebserver.cpp. Under IDF 6 the SPI Ethernet controllers were moved
// out of esp_eth into the component registry, so the MAC/PHY constructors used
// here come from espressif/w5500 (see src/idf_component.yml).

#include "eth.h"

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "driver/gpio.h"

static const char *TAG = "eth";

// W5500 PHY address is fixed at 1 (same value the Arduino build passes).
#define W5500_PHY_ADDR 1
// W5500 INTn goes to pad D16 = GPIO10 on the XIAO ESP32-S3 Plus: that is the
// XIAO W5500 Ethernet Adapter V1.2 and later (Seeed's "INT pin wiring fix,
// D16 -> W5500 INTn", 2025/12/01). The driver then wakes its RX task from the
// ISR instead of reading the chip on a timer, which removes the up-to-10 ms
// receive latency that polling costs. "Early Build" adapters leave INTn
// floating; build those with -DXIAO_W5500_POLLING=1 to fall back to a timed
// poll (the driver requires exactly one of the two modes).
#ifndef XIAO_W5500_POLLING
#define XIAO_W5500_POLLING 0
#endif
#if XIAO_W5500_POLLING
#define XIAO_W5500_INT_GPIO -1
#else
#define XIAO_W5500_INT_GPIO 10
#endif
#define W5500_POLL_PERIOD_MS 10
#define W5500_SPI_CLOCK_HZ (20 * 1000 * 1000)

static esp_eth_handle_t s_eth_handle;
static esp_netif_t *s_netif;

static void eth_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "W5500 started");
        break;
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "link down");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGW(TAG, "stopped");
        break;
    default:
        break;
    }
}

static void got_ip_handler(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_id;

    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "IPv4 " IPSTR " -> http://" IPSTR "/",
             IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.ip));
}

esp_err_t eth_w5500_start(void)
{
    // SPI2/FSPI: the XIAO's D8..D10 pins are plain GPIOs, not a dedicated SPI
    // peripheral, so any free host works. 4 KiB transfer size covers the
    // largest W5500 socket buffer burst the driver issues.
    spi_bus_config_t buscfg = {
        .mosi_io_num = XIAO_W5500_MOSI,
        .miso_io_num = XIAO_W5500_MISO,
        .sclk_io_num = XIAO_W5500_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init failed");

    // command_bits/address_bits = 0 lets the W5500 driver install its own
    // 16-bit address + 8-bit control frame format.
    static spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = W5500_SPI_CLOCK_HZ,
        .spics_io_num = XIAO_W5500_CS,
        .queue_size = 20,
    };

    eth_w5500_config_t w5500_config =
        ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &spi_devcfg);
    w5500_config.base.int_gpio_num = XIAO_W5500_INT_GPIO;
    // Exactly one of interrupt/polling, per the driver's own config check.
    w5500_config.base.poll_period_ms =
        XIAO_W5500_INT_GPIO < 0 ? W5500_POLL_PERIOD_MS : 0;
    ESP_LOGI(TAG, "W5500 RX mode: %s",
             XIAO_W5500_INT_GPIO < 0
                 ? "polling (build flag XIAO_W5500_POLLING)"
                 : "interrupt on GPIO10 (adapter V1.2+)");

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    // The W5500 driver's RX task is deeper than the built-in MAC's; 4 KiB is
    // the value the driver README recommends for WiFi-less SPI parts.
    mac_config.rx_task_stack_size = 4096;

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = W5500_PHY_ADDR;
    phy_config.reset_gpio_num = -1;

    // The W5500 driver attaches its ISR to the INTn pin during MAC init but
    // never installs the GPIO ISR service itself, so install it first.
    // ESP_ERR_INVALID_STATE only means someone else already did.
    if (XIAO_W5500_INT_GPIO >= 0) {
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "gpio isr service: %s", esp_err_to_name(err));
            return err;
        }
    }

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    ESP_RETURN_ON_FALSE(mac && phy, ESP_ERR_NO_MEM, TAG,
                        "W5500 MAC/PHY allocation failed");

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &s_eth_handle),
                        TAG, "ethernet driver install failed");

    // W5500 has no factory MAC of its own (its SHAR register resets to zero),
    // so seed it from the ESP32-S3 eFuse the same way the Arduino ETH class
    // does. Without this DHCP clients go out with 00:00:00:00:00:00.
    uint8_t mac_addr[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac_addr, ESP_MAC_ETH), TAG,
                        "read eFuse MAC failed");
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR,
                                      mac_addr),
                        TAG, "set MAC failed");
    ESP_LOGI(TAG, "MAC %02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5]);

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_netif, ESP_ERR_NO_MEM, TAG, "netif alloc failed");

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_RETURN_ON_FALSE(glue, ESP_ERR_NO_MEM, TAG, "netif glue alloc failed");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, glue), TAG,
                        "netif attach failed");

    // Same hostname the Arduino build advertises; mdns_adv.c publishes it on
    // the LAN as tally.local.
    esp_netif_set_hostname(s_netif, "tally");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(
                            ETH_EVENT, ESP_EVENT_ANY_ID,
                            eth_event_handler, NULL),
                        TAG, "ETH event handler register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
                            IP_EVENT, IP_EVENT_ETH_GOT_IP,
                            got_ip_handler, NULL),
                        TAG, "IP event handler register failed");

    return esp_eth_start(s_eth_handle);
}

esp_netif_t *eth_w5500_netif(void)
{
    return s_netif;
}
