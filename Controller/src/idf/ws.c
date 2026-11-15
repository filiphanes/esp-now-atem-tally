// WebSocket tally feed on port 81 for the pure-IDF controller.
//
// Mirrors ws_tally()/webserverLoop() from the Arduino build:
//   * every client gets the current "TALLY ..." frame immediately on connect
//   * the same frame is re-broadcast every 2 s as an idle keepalive, because
//     the tally UI treats 3 s of silence as a dead controller
//   * each tally change (ws_set_tally) pushes an immediate update
//
// The shared data/*.html pages expect exactly this format and also rely on the
// keepalive to avoid their onclose reload loop.

#include "ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ws";

#define WS_PORT 81
// Same cadence as webserverLoop() in configWebserver.cpp.
#define WS_KEEPALIVE_US (2000 * 1000)
// Longest possible frame: "TALLY " + one char per tally + NUL.
#define WS_MSG_MAX (6 + 64 + 1)

static httpd_handle_t s_server;
static esp_timer_handle_t s_keepalive;

// Tally bits can be updated from protocol tasks and are read by the HTTP
// server task, so keep the 64-bit pairs behind a spinlock.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_program_bits;
static uint64_t s_preview_bits;

// Same encoding as ws_tally(): all-zero trailing entries are trimmed, so an
// idle controller sends the literal "TALLY " (trailing space included).
static size_t format_tally(char *buf, size_t cap)
{
    uint64_t program;
    uint64_t preview;
    taskENTER_CRITICAL(&s_lock);
    program = s_program_bits;
    preview = s_preview_bits;
    taskEXIT_CRITICAL(&s_lock);

    size_t pos = (size_t)snprintf(buf, cap, "TALLY ");
    while ((program || preview) && pos + 1 < cap) {
        char c = '0';
        if (program & 1) {
            c += 1;
        }
        if (preview & 1) {
            c += 2;
        }
        buf[pos++] = c;
        program >>= 1;
        preview >>= 1;
    }
    if (pos < cap) {
        buf[pos] = '\0';
    }
    return pos;
}

static esp_err_t send_tally(httpd_req_t *req)
{
    char msg[WS_MSG_MAX];
    size_t len = format_tally(msg, sizeof(msg));
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)msg,
        .len = len,
    };
    return httpd_ws_send_frame(req, &frame);
}

// Runs in the HTTP server's task (via httpd_queue_work), so frames are sent
// from the same context that owns the sockets.
static void send_text_to_all(const char *msg, size_t len)
{
    // Must be at least max_open_sockets from the server config.
    int fds[8];
    size_t count = sizeof(fds) / sizeof(fds[0]);
    if (httpd_get_client_list(s_server, &count, fds) != ESP_OK) {
        return;
    }

    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)msg,
        .len = len,
    };

    for (size_t i = 0; i < count; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) !=
            HTTPD_WS_CLIENT_WEBSOCKET) {
            continue;
        }
        esp_err_t err = httpd_ws_send_frame_async(s_server, fds[i], &frame);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "fd %d send failed: %s", fds[i],
                     esp_err_to_name(err));
        }
    }
}

static void broadcast_tally_work(void *arg)
{
    (void)arg;
    char msg[WS_MSG_MAX];
    size_t len = format_tally(msg, sizeof(msg));
    send_text_to_all(msg, len);
}

// Owns a heap copy of the message because httpd_queue_work() takes a single
// void* and the caller may return before the work runs.
static void broadcast_text_work(void *arg)
{
    char *msg = arg;
    send_text_to_all(msg, strlen(msg));
    free(msg);
}

static void keepalive_cb(void *arg)
{
    (void)arg;
    // Drops the tick if the server queue is busy; the 2 s timer soon retries.
    httpd_queue_work(s_server, broadcast_tally_work, NULL);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    // GET without an Upgrade header is the WebSocket handshake, which
    // esp_http_server has already completed by the time we get here.
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "client %d connected", fd);
        return send_tally(req);
    }

    // The pages are receive-only, but any inbound frame must be drained or
    // its payload would be parsed as the next frame header.
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = NULL,
        .len = 0,
    };
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fd %d recv failed: %s", fd, esp_err_to_name(err));
        return err;
    }

    uint8_t scratch[32];
    if (frame.len > sizeof(scratch)) {
        ESP_LOGW(TAG, "fd %d oversized frame (%u bytes), closing", fd,
                 (unsigned)frame.len);
        return ESP_FAIL;
    }
    if (frame.len > 0) {
        frame.payload = scratch;
        err = httpd_ws_recv_frame(req, &frame, sizeof(scratch));
        if (err != ESP_OK) {
            return err;
        }
    }
    ESP_LOGD(TAG, "fd %d sent %u-byte type %d frame", fd,
             (unsigned)frame.len, (int)frame.type);
    return ESP_OK;
}

esp_err_t ws_start(void)
{
    // The UI server on port 80 already owns the default control port (32768),
    // so the second httpd instance needs its own.
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WS_PORT;
    cfg.ctrl_port = 32769;
    // A tally page holds one WebSocket; 4 concurrent clients (plus the 3
    // internal sockets) fits the lwIP pool budget documented in
    // sdkconfig.defaults.
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t route = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
    };
    err = httpd_register_uri_handler(s_server, &route);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = keepalive_cb,
        .name = "ws_keepalive",
    };
    err = esp_timer_create(&timer_args, &s_keepalive);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timer create failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_timer_start_periodic(s_keepalive, WS_KEEPALIVE_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timer start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WebSocket server on port %d", WS_PORT);
    return ESP_OK;
}

void ws_set_tally(uint64_t program_bits, uint64_t preview_bits)
{
    taskENTER_CRITICAL(&s_lock);
    s_program_bits = program_bits;
    s_preview_bits = preview_bits;
    taskEXIT_CRITICAL(&s_lock);

    if (s_server != NULL) {
        httpd_queue_work(s_server, broadcast_tally_work, NULL);
    }
}

void ws_broadcast_set(const char *key, const char *value, const char *csv)
{
    if (s_server == NULL || key == NULL || value == NULL || csv == NULL) {
        return;
    }

    char msg[128];
    int len = snprintf(msg, sizeof(msg), "%s %s %s", key, value, csv);
    if (len <= 0 || (size_t)len >= sizeof(msg)) {
        return;
    }

    char *copy = strdup(msg);
    if (copy == NULL) {
        return;
    }
    if (httpd_queue_work(s_server, broadcast_text_work, copy) != ESP_OK) {
        free(copy);
    }
}
