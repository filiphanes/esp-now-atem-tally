// vMix TCP client (port of src/vmix.cpp).
//
// Connects to the vMix host from the config, sends "SUBSCRIBE TALLY" and turns
// the resulting "TALLY OK ..." lines into the shared program/preview state via
// espnow_tally(). Reconnect backoff matches the Arduino build: 1 s doubling to
// 30 s, reset by a successful connect.
//
// Deliberate differences from the Arduino build:
//   * the client runs in its own task instead of loop(), so a dead or slow
//     host cannot stall the ESP-NOW burst pump, the ATEM client or HTTP;
//   * connect() is bounded by a 3 s timeout (nonblocking + select), where
//     WiFiClient::connect() blocks the caller until the TCP stack gives up.

#include "vmix_client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "espnow.h"
#include "watchdog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "vmix";

#define VMIX_LINE_MAX 1024
// How long the client blocks when vMix is quiet. An incoming line wakes
// select() immediately; the timeout only bounds how often the watchdog is fed,
// so a second is plenty and keeps the idle task asleep.
#define VMIX_WAIT_MS 1000
#define VMIX_RETRY_MIN_MS 1000u
#define VMIX_RETRY_MAX_MS 30000u
#define VMIX_CONNECT_TIMEOUT_S 3
#define VMIX_TASK_STACK 4096

static int s_sock = -1;
static char s_line[VMIX_LINE_MAX];
static size_t s_line_len;
static uint32_t s_next_retry_ms;
static uint32_t s_retry_backoff_ms = VMIX_RETRY_MIN_MS;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint64_t bitn(uint8_t n)
{
    return (uint64_t)1 << (n - 1);
}

// One complete line from vMix.
static void handle_line(const char *line)
{
    if (strncmp(line, "TALLY OK ", 9) == 0) {
        // Each following character is one input: '0' off, '1' program,
        // '2' preview, '3' both. First char = input 1 = bit 0.
        const char *t = line + 9;
        uint64_t program = 0;
        uint64_t preview = 0;
        for (uint8_t i = 0; i < 64 && t[i] >= '0' && t[i] <= '3'; i++) {
            if (t[i] & 1) {
                program |= bitn(i + 1);
            }
            if (t[i] & 2) {
                preview |= bitn(i + 1);
            }
        }
        espnow_tally(&program, &preview);
    } else if (strncmp(line, "SUBSCRIBE OK TALLY", 18) == 0) {
        ESP_LOGI(TAG, "SUBSCRIBE OK TALLY");
    }
}

static void disconnect(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    s_line_len = 0;
}

static bool send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, data + sent, len - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

// Nonblocking connect with a bounded timeout: never more than
// VMIX_CONNECT_TIMEOUT_S is spent in this task before it re-checks the clock.
static bool vmix_connect(void)
{
    char ip[16];
    esp_ip4addr_ntoa(&g_config.ip, ip, sizeof(ip));
    ESP_LOGI(TAG, "connecting %s:%u", ip, g_config.port);

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGW(TAG, "socket failed: errno %d", errno);
        return false;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGW(TAG, "fcntl O_NONBLOCK failed: errno %d", errno);
    }

    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = g_config.ip.addr;   // already network order
    peer.sin_port = htons(g_config.port);

    if (connect(fd, (const struct sockaddr *)&peer, sizeof(peer)) < 0) {
        if (errno != EINPROGRESS) {
            ESP_LOGW(TAG, "connect failed: errno %d", errno);
            close(fd);
            return false;
        }
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(fd, &write_fds);
        struct timeval tv = {
            .tv_sec = VMIX_CONNECT_TIMEOUT_S,
            .tv_usec = 0,
        };
        int ready = select(fd + 1, NULL, &write_fds, NULL, &tv);
        if (ready <= 0) {
            ESP_LOGW(TAG, "connect timed out");
            close(fd);
            return false;
        }
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 ||
            so_error != 0) {
            ESP_LOGW(TAG, "connect failed: errno %d", so_error);
            close(fd);
            return false;
        }
    }

    if (!send_all(fd, "SUBSCRIBE TALLY\r\n", 17)) {
        ESP_LOGW(TAG, "SUBSCRIBE send failed: errno %d", errno);
        close(fd);
        return false;
    }

    s_sock = fd;
    s_line_len = 0;
    ESP_LOGI(TAG, "connected");
    return true;
}

// Blocks until the server sends something. The receive path is event-driven;
// the timeout only bounds how often the watchdog gets fed.
static void wait_for_rx(void)
{
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(s_sock, &read_fds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = VMIX_WAIT_MS * 1000 };
    select(s_sock + 1, &read_fds, NULL, NULL, &tv);
}

// Nonblocking line assembly. Never use a blocking read here: with no newline
// buffered it would stall this task for the socket timeout.
static void drain_rx(void)
{
    char chunk[256];
    for (;;) {
        int n = recv(s_sock, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                char c = chunk[i];
                if (c == '\n') {
                    s_line[s_line_len] = '\0';
                    if (s_line_len > 0) {
                        handle_line(s_line);
                    }
                    s_line_len = 0;
                } else if (c != '\r' && s_line_len < sizeof(s_line) - 1) {
                    s_line[s_line_len++] = c;
                }
            }
            continue;
        }
        if (n == 0) {
            ESP_LOGW(TAG, "server closed the connection");
            disconnect();
            return;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "recv failed: errno %d", errno);
            disconnect();
        }
        return;
    }
}

static void vmix_client_task(void *arg)
{
    (void)arg;
    watchdog_subscribe();
    for (;;) {
        watchdog_feed();
        if (s_sock < 0) {
            if ((int32_t)(now_ms() - s_next_retry_ms) >= 0) {
                if (vmix_connect()) {
                    s_retry_backoff_ms = VMIX_RETRY_MIN_MS;
                    s_next_retry_ms = now_ms();
                } else {
                    ESP_LOGW(TAG, "connect failed, retrying in %u s",
                             s_retry_backoff_ms / 1000);
                    s_next_retry_ms = now_ms() + s_retry_backoff_ms;
                    if (s_retry_backoff_ms < VMIX_RETRY_MAX_MS / 2) {
                        s_retry_backoff_ms *= 2;
                    } else {
                        s_retry_backoff_ms = VMIX_RETRY_MAX_MS;
                    }
                }
                continue;
            }
            // Nothing to do but wait for the next attempt, so sleep that long
            // instead of polling for it (the cap keeps a bad deadline from
            // parking the task).
            uint32_t sleep_ms = (uint32_t)(int32_t)(s_next_retry_ms - now_ms());
            if (sleep_ms < 10) {
                sleep_ms = 10;
            }
            if (sleep_ms > 1000) {
                sleep_ms = 1000;
            }
            vTaskDelay(pdMS_TO_TICKS(sleep_ms));
            continue;
        }

        wait_for_rx();
        drain_rx();
    }
}

esp_err_t vmix_start(void)
{
    char ip[16];
    esp_ip4addr_ntoa(&g_config.ip, ip, sizeof(ip));
    ESP_LOGI(TAG, "target %s:%u", ip, g_config.port);

    s_next_retry_ms = now_ms();   // try immediately
    BaseType_t ok = xTaskCreate(vmix_client_task, "vmix", VMIX_TASK_STACK,
                                NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
