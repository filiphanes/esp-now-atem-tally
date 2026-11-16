// ATEM LAN discovery for GET /discover.
//
// Two sources are merged:
//   * the same 20-byte handshake opener the ATEM client sends (flags = hello,
//     length = 20, provisional session 0x53AB, byte 9 = 0x3A, payload[0] =
//     0x01) when it is sent as a broadcast on UDP 9910. The reply is the
//     switcher's hello, which carries its session but no product name, so
//     UDP-only results are listed as "ATEM".
//   * the `_blackmagic._tcp` mDNS service that Blackmagic's own software uses.
//     It provides the instance name and an A record, and can list a switcher
//     whose IP is on another subnet as long as it shares the L2 segment.
// We do not ACK the hello, so the switcher never starts its state dump into
// this socket.
//
// vMix and OBS are not discoverable: vMix has no Zeroconf/SSDP advertisement
// and obs-websocket only accepts clients on its configured TCP port.

#include "discovery.h"

#include <errno.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "mdns.h"

#include "eth.h"

static const char *TAG = "discovery";

#define ATEM_PORT 9910
#define ATEM_FLAG_HELLO 0x02

// Handshake opener, byte-for-byte the packet Blackmagic's own software
// broadcasts to locate switchers.
static const uint8_t k_atem_hello[20] = {
    0x10, 0x14, 0x53, 0xAB, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x3A, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

static void send_probe(int sock, uint32_t addr)
{
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(ATEM_PORT);
    dst.sin_addr.s_addr = addr;

    if (sendto(sock, k_atem_hello, sizeof(k_atem_hello), 0,
               (const struct sockaddr *)&dst, sizeof(dst)) < 0) {
        ESP_LOGW(TAG, "ATEM probe failed: errno %d", errno);
    }
}

// mDNS instance names are display strings; keep them printable ASCII and
// JSON-safe (web.c interpolates the name into the /discover response).
static void copy_name(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return;
    }
    size_t j = 0;
    if (src != NULL) {
        for (size_t i = 0; src[i] != '\0' && j + 1 < cap; i++) {
            unsigned char c = (unsigned char)src[i];
            dst[j++] = (c >= 0x20 && c <= 0x7e && c != '"' && c != '\\')
                           ? (char)c
                           : '?';
        }
    }
    dst[j] = '\0';
}

static void add_device(discovery_device_t *out, size_t max, size_t *count,
                       const char *ip, uint16_t port, const char *name)
{
    const char *label = (name != NULL && name[0] != '\0') ? name : "ATEM";

    for (size_t i = 0; i < *count; i++) {
        if (strcmp(out[i].ip, ip) == 0) {
            // The UDP hello carries no product name; prefer the mDNS name.
            if (strcmp(out[i].name, "ATEM") == 0 &&
                strcmp(label, "ATEM") != 0) {
                copy_name(out[i].name, sizeof(out[i].name), label);
            }
            return;   // the switcher answered more than once
        }
    }
    if (*count >= max) {
        return;
    }

    strlcpy(out[*count].ip, ip, sizeof(out[*count].ip));
    out[*count].port = port;
    copy_name(out[*count].name, sizeof(out[*count].name), label);
    (*count)++;
    ESP_LOGI(TAG, "ATEM switcher at %s (%s)", ip, out[*count - 1].name);
}

static bool mdns_is_atem(const mdns_result_t *result)
{
    if (result->port != ATEM_PORT) {
        return false;
    }
    for (size_t i = 0; i < result->txt_count; i++) {
        if (result->txt[i].key != NULL &&
            strcmp(result->txt[i].key, "class") == 0) {
            return result->txt[i].value != NULL &&
                   strcmp(result->txt[i].value, "AtemSwitcher") == 0;
        }
    }
    // Older firmware may omit the class TXT; the control port is the fallback.
    return true;
}

// The ATEM TVS advertises _blackmagic._tcp and may not answer the UDP hello at
// all. Querying the same service macOS uses also finds switchers whose IP is on
// a different subnet, as long as they share the L2 segment.
static void scan_mdns(discovery_device_t *out, size_t max, size_t *count,
                      uint32_t timeout_ms)
{
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_blackmagic", "_tcp", timeout_ms,
                                   DISCOVERY_MAX_DEVICES, &results);
    if (err != ESP_OK) {
        if (err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "mDNS query failed: %s", esp_err_to_name(err));
        }
        return;
    }

    for (mdns_result_t *result = results; result != NULL;
         result = result->next) {
        if (!mdns_is_atem(result)) {
            continue;
        }
        const char *name =
            (result->instance_name != NULL) ? result->instance_name : "ATEM";
        for (mdns_ip_addr_t *addr = result->addr; addr != NULL;
             addr = addr->next) {
            if (addr->addr.type != ESP_IPADDR_TYPE_V4) {
                continue;
            }
            char ip[16];
            esp_ip4addr_ntoa(&addr->addr.u_addr.ip4, ip, sizeof(ip));
            add_device(out, max, count, ip, result->port, name);
        }
    }
    mdns_query_results_free(results);
}

size_t discovery_scan(discovery_device_t *out, size_t max, uint32_t timeout_ms)
{
    if (out == NULL || max == 0) {
        return 0;
    }

    esp_netif_t *netif = eth_netif();
    esp_netif_ip_info_t ip_info;
    if (netif == NULL || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK ||
        ip_info.ip.addr == 0) {
        ESP_LOGW(TAG, "no IPv4 address yet");
        return 0;
    }

    size_t count = 0;
    if (timeout_ms > 0) {
        uint32_t mdns_timeout = timeout_ms;
        if (mdns_timeout < 100) {
            mdns_timeout = 100;
        }
        scan_mdns(out, max, &count, mdns_timeout);
    }
    if (count >= max) {
        ESP_LOGI(TAG, "scan finished: %u device(s)", (unsigned)count);
        return count;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "socket failed: errno %d", errno);
        return count;
    }

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    // Bind to an ephemeral port so the switchers reply to this socket.
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (bind(sock, (const struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGW(TAG, "bind failed: errno %d", errno);
        close(sock);
        return count;
    }

    // Probe the directed broadcast and the limited broadcast: some networks
    // pass only one of the two to the switchers.
    uint32_t ip = ntohl(ip_info.ip.addr);
    uint32_t mask = ntohl(ip_info.netmask.addr);
    send_probe(sock, htonl((ip & mask) | ~mask));
    send_probe(sock, htonl(INADDR_BROADCAST));

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (count < max) {
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) {
            break;
        }
        struct timeval tv = {
            .tv_sec = (time_t)(remaining / 1000000),
            .tv_usec = (suseconds_t)(remaining % 1000000),
        };
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sock, &read_fds);
        int ready = select(sock + 1, &read_fds, NULL, NULL, &tv);
        if (ready <= 0) {
            break;
        }

        uint8_t buf[128];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &from_len);
        if (n < 12) {
            continue;
        }

        // Only accept a well-formed ATEM packet: the length in the header has
        // to match the datagram. Accept the switcher's hello (the normal
        // reply) or anything from the ATEM port, in case a firmware variant
        // answers differently.
        uint16_t packet_len = ((uint16_t)(buf[0] & 0x07) << 8) | buf[1];
        uint8_t flags = buf[0] >> 3;
        if (packet_len != (uint16_t)n &&
            ntohs(from.sin_port) != ATEM_PORT) {
            continue;
        }
        if (!(flags & ATEM_FLAG_HELLO) && ntohs(from.sin_port) != ATEM_PORT) {
            continue;
        }

        esp_ip4_addr_t addr = {.addr = from.sin_addr.s_addr};
        char from_ip[sizeof(out[0].ip)];
        esp_ip4addr_ntoa(&addr, from_ip, sizeof(from_ip));
        add_device(out, max, &count, from_ip, ATEM_PORT, "ATEM");
    }

    close(sock);
    ESP_LOGI(TAG, "scan finished: %u device(s)", (unsigned)count);
    return count;
}
