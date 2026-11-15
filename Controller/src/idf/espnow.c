// ESP-NOW tally pump for the pure-IDF controller (port of src/espNow.cpp).
//
// The burst/sleep behaviour is intentionally identical to the Arduino build:
// every tally change arms a SET_TALLY burst (one packet per burst_gap_ms for
// burst_ms), and receivers are told to SLEEP after a burst when sleep_ms > 0.
//
// Differences from the Arduino source:
//   * tally updates from other tasks only set a dirty flag; the packet pump
//     runs in its own task, so nothing races the burst bookkeeping.
//   * last-seen timestamps use esp_timer_get_time() and are protected by a
//     critical section because the ESP-NOW callback runs in the WiFi task.
//   * an idle keepalive sends one packet instead of a full burst (2 s window);
//     a tally change still gets the full burst.

#include "espnow.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "vmix_server.h"
#include "ws.h"

static const char *TAG = "espnow";

#define ESPNOW_CHANNEL 1
// Idle keepalive only needs to keep receivers from blanking (5 s timeout) and
// resync a missed change; one packet every 2 s leaves room for a lost packet.
#define ESPNOW_KEEPALIVE_MIN_MS 2000u
#define ESPNOW_KEEPALIVE_MAX_MS 4000u
#define ESPNOW_STALE_US (60LL * 1000 * 1000)
#define ESPNOW_RESYNC_STREAK 3

static const uint8_t k_broadcast_mac[ESPNOW_MAC_LEN] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

// Shared between the pump task, the OSC task, the HTTP tasks and the WiFi
// callback, so all multi-field access goes through this lock.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_program_bits;
static uint64_t s_preview_bits;
static espnow_tally_info_t s_tallies[ESPNOW_MAX_TALLY];
static espnow_power_t s_power = {
    .sleep_ms = 0,
    .burst_ms = 100,
    .burst_gap_ms = 20,
};

// Pump-task-only state.
static uint32_t s_burst_until;
static uint32_t s_burst_next;
static uint8_t s_burst_payload[1 + 2 * sizeof(uint64_t)];
static uint8_t s_burst_len;
static uint32_t s_sleep_until;
static uint32_t s_last_message_at;
static uint32_t s_last_prune_ms;
static bool s_burst_active_prev;

// The task that runs espnow_loop(); espnow_wake() pokes it so a tally change
// goes out immediately instead of waiting for the pump's next deadline.
static TaskHandle_t s_pump_task;

// Shared flags. Byte-sized, so plain loads/stores are atomic on both cores.
static volatile bool s_tally_dirty;
// Set whenever the tally state changes, so the pump task can push it to the
// WebSocket and vMix sinks. Kept separate from s_tally_dirty because the radio
// burst has its own sleep/retry rules (a keepalive re-bursts without a state
// change and must not re-push to the UI).
static volatile bool s_sinks_dirty;
static volatile uint8_t s_send_fail_streak;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// 0 when the deadline has already passed, so callers can treat that as "now".
static uint32_t ms_until(uint32_t deadline, uint32_t now)
{
    int32_t delta = (int32_t)(deadline - now);
    return delta > 0 ? (uint32_t)delta : 0u;
}

void espnow_wake(void)
{
    TaskHandle_t task = s_pump_task;
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

static void send_raw(const uint8_t *payload, size_t len)
{
    esp_err_t err = esp_now_send(k_broadcast_mac, payload, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
    }
}

// ---- Burst pump -----------------------------------------------------------

static void start_burst(const uint8_t *payload, uint8_t len,
                        uint32_t duration_ms)
{
    if (len > sizeof(s_burst_payload)) {
        len = sizeof(s_burst_payload);
    }
    memcpy(s_burst_payload, payload, len);
    s_burst_len = len;
    s_burst_until = now_ms() + duration_ms;
    // First packet goes out immediately for receivers that are already awake.
    send_raw(s_burst_payload, s_burst_len);
    s_burst_next = now_ms() + s_power.burst_gap_ms;
}

// Sends at most one packet per tick. Returns true while a burst is active.
static bool burst_tick(void)
{
    if (s_burst_len == 0) {
        return false;
    }
    if ((int32_t)(now_ms() - s_burst_until) >= 0) {
        s_burst_len = 0;
        return false;
    }
    if ((int32_t)(now_ms() - s_burst_next) < 0) {
        return true;
    }
    s_burst_next += s_power.burst_gap_ms;
    if ((int32_t)(now_ms() - s_burst_next) > 0) {
        s_burst_next = now_ms();  // fell behind: catch up without a burst
    }
    send_raw(s_burst_payload, s_burst_len);
    return true;
}

// Fills a SET_TALLY payload with the current bitmaps. `payload` must hold
// 1 + 2 * sizeof(uint64_t) bytes.
static void fill_tally_payload(uint8_t payload[1 + 2 * sizeof(uint64_t)])
{
    payload[0] = ESPNOW_SET_TALLY;
    taskENTER_CRITICAL(&s_lock);
    memcpy(payload + 1, &s_program_bits, sizeof(uint64_t));
    memcpy(payload + 1 + sizeof(uint64_t), &s_preview_bits,
           sizeof(uint64_t));
    taskEXIT_CRITICAL(&s_lock);
}

static void flush_tally(void)
{
    if (!s_tally_dirty) {
        return;
    }
    if ((int32_t)(now_ms() - s_sleep_until) < 0) {
        return;  // receivers are asleep; the change stays latched
    }
    s_tally_dirty = false;
    s_last_message_at = now_ms();

    uint8_t payload[1 + 2 * sizeof(uint64_t)];
    fill_tally_payload(payload);
    start_burst(payload, sizeof(payload), s_power.burst_ms);
}

static uint32_t keepalive_window_ms(void)
{
    uint32_t cycle = (uint32_t)s_power.sleep_ms + s_power.burst_ms;
    if (cycle > ESPNOW_KEEPALIVE_MAX_MS) {
        cycle = ESPNOW_KEEPALIVE_MAX_MS;
    }
    if (cycle < ESPNOW_KEEPALIVE_MIN_MS) {
        cycle = ESPNOW_KEEPALIVE_MIN_MS;
    }
    return cycle;
}

// ---- ESP-NOW callbacks ----------------------------------------------------

static void on_data_sent(const wifi_tx_info_t *tx_info,
                         esp_now_send_status_t status)
{
    (void)tx_info;
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_send_fail_streak = 0;
    } else if (s_send_fail_streak < 255) {
        s_send_fail_streak++;
    }
}

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data,
                         int len)
{
    if (len <= 0 || data == NULL || info == NULL) {
        return;
    }

    switch ((enum espnow_command)data[0]) {
    case ESPNOW_HEARTBEAT: {
        if (len < 2) {
            break;
        }
        const int64_t now = esp_timer_get_time();
        taskENTER_CRITICAL(&s_lock);
        for (int i = 0; i < ESPNOW_MAX_TALLY; i++) {
            if (memcmp(s_tallies[i].mac_addr, info->src_addr,
                       ESPNOW_MAC_LEN) == 0) {
                s_tallies[i].id = data[1];
                s_tallies[i].last_seen_us = now;
                break;
            }
            if (s_tallies[i].id == 0) {
                memcpy(s_tallies[i].mac_addr, info->src_addr, ESPNOW_MAC_LEN);
                s_tallies[i].id = data[1];
                s_tallies[i].last_seen_us = now;
                break;
            }
        }
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGD(TAG, "heartbeat cam %u", data[1]);
        break;
    }
    case ESPNOW_GET_TALLY:
        ESP_LOGI(TAG, "GET_TALLY from receiver");
        s_tally_dirty = true;
        espnow_wake();
        break;
    default:
        break;
    }
}

// ---- Public API -----------------------------------------------------------

esp_err_t espnow_setup(void)
{
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "wifi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(ESPNOW_CHANNEL,
                                             WIFI_SECOND_CHAN_NONE),
                        TAG, "channel failed");

    // Long-range only, matching the Receiver and the Arduino controller.
    esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WIFI_PROTOCOL_LR unsupported: %s",
                 esp_err_to_name(err));
    }

    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "esp_now_init failed");
    ESP_RETURN_ON_ERROR(esp_now_register_send_cb(on_data_sent), TAG,
                        "send cb failed");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(on_data_recv), TAG,
                        "recv cb failed");

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, k_broadcast_mac, ESPNOW_MAC_LEN);
    peer.channel = 0;  // current home channel (1)
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_RETURN_ON_ERROR(esp_now_add_peer(&peer), TAG, "add peer failed");

    memset(s_tallies, 0, sizeof(s_tallies));
    s_last_message_at = now_ms() - keepalive_window_ms() - 1;

    ESP_LOGI(TAG, "ESP-NOW up on channel %d (LR), broadcast peer added",
             ESPNOW_CHANNEL);
    return ESP_OK;
}

void espnow_get_tally(uint64_t *program, uint64_t *preview)
{
    taskENTER_CRITICAL(&s_lock);
    if (program) {
        *program = s_program_bits;
    }
    if (preview) {
        *preview = s_preview_bits;
    }
    taskEXIT_CRITICAL(&s_lock);
}

void espnow_tally(uint64_t *program, uint64_t *preview)
{
    taskENTER_CRITICAL(&s_lock);
    if (program) {
        s_program_bits = *program;
    }
    if (preview) {
        s_preview_bits = *preview;
    }
    taskEXIT_CRITICAL(&s_lock);

    // Only latches the change and wakes the pump: that task owns every output
    // (the ESP-NOW burst, the WebSocket feed and the vMix TCP server), so no
    // protocol task ever runs a sink. It also coalesces rapid changes into one
    // push instead of one per intermediate state.
    s_tally_dirty = true;
    s_sinks_dirty = true;
    espnow_wake();
}

void espnow_brightness(uint8_t brightness, const uint64_t *bits)
{
    uint8_t payload[2 + sizeof(uint64_t)];
    payload[0] = ESPNOW_SET_BRIGHTNESS;
    payload[1] = brightness;
    memcpy(payload + 2, bits, sizeof(*bits));
    send_raw(payload, sizeof(payload));
}

void espnow_camid(uint8_t cam_id, const uint64_t *bits)
{
    uint8_t payload[2 + sizeof(uint64_t)];
    payload[0] = ESPNOW_SET_CAMID;
    payload[1] = cam_id;
    memcpy(payload + 2, bits, sizeof(*bits));
    send_raw(payload, sizeof(payload));
}

void espnow_color(uint32_t color, const uint64_t *bits)
{
    uint8_t payload[4 + sizeof(uint64_t)];
    payload[0] = ESPNOW_SET_COLOR;
    payload[1] = (color >> 16) & 0xff;
    payload[2] = (color >> 8) & 0xff;
    payload[3] = color & 0xff;
    memcpy(payload + 4, bits, sizeof(*bits));
    send_raw(payload, sizeof(payload));
}

void espnow_signal(uint8_t signal, const uint64_t *bits)
{
    // The signal number is itself the command byte (see tally.html).
    uint8_t payload[1 + sizeof(uint64_t)];
    payload[0] = signal;
    memcpy(payload + 1, bits, sizeof(*bits));
    send_raw(payload, sizeof(payload));
}

void espnow_sleep_all(uint16_t ms)
{
    uint8_t payload[3];
    payload[0] = ESPNOW_SLEEP;
    payload[1] = ms & 0xff;
    payload[2] = (ms >> 8) & 0xff;
    send_raw(payload, sizeof(payload));
}

static void prune_stale_tallies(void)
{
    uint32_t now = now_ms();
    // The pump wakes for every burst packet; walking 64 slots at 50 Hz buys
    // nothing, so prune on a 1 s cadence instead.
    if ((int32_t)(now - s_last_prune_ms) < 1000) {
        return;
    }
    s_last_prune_ms = now;

    const int64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < ESPNOW_MAX_TALLY; i++) {
        if (s_tallies[i].id != 0 &&
            now_us - s_tallies[i].last_seen_us > ESPNOW_STALE_US) {
            s_tallies[i].id = 0;
            memset(s_tallies[i].mac_addr, 0, ESPNOW_MAC_LEN);
        }
    }
    taskEXIT_CRITICAL(&s_lock);
}

// How long the pump may sleep before it has something to do: the burst pacing,
// the receiver sleep window, the idle keepalive and stale-receiver pruning.
// The 1 s ceiling keeps pruning at most a second late.
static uint32_t next_sleep_ms(void)
{
    uint32_t now = now_ms();
    uint32_t best = 1000;

    if (s_burst_len != 0) {
        uint32_t d = ms_until(s_burst_next, now);
        if (d < best) {
            best = d;
        }
    }
    uint32_t d = ms_until(s_sleep_until, now);
    if ((int32_t)(s_sleep_until - now) > 0 && d < best) {
        best = d;
    }
    d = ms_until(s_last_message_at + keepalive_window_ms(), now);
    if (d < best) {
        best = d;
    }
    return best != 0 ? best : 1u;
}

// Fan-out to the non-radio sinks. Runs on the pump task so a protocol task
// never does output work; the dirty flag means at most one push per pump wake.
static void push_sinks(void)
{
    if (!s_sinks_dirty) {
        return;
    }
    s_sinks_dirty = false;

    uint64_t program;
    uint64_t preview;
    taskENTER_CRITICAL(&s_lock);
    program = s_program_bits;
    preview = s_preview_bits;
    taskEXIT_CRITICAL(&s_lock);

    ws_set_tally(program, preview);
    // Same downstream hook as the Arduino build: the vMix TCP tally server
    // pushes the new state to its subscribers.
    vmix_server_tally(program, preview);
}

// Idle keepalive: one packet instead of a burst. Receivers blank after 5 s of
// silence, and the 2 s window leaves room for a lost packet. When they are
// duty-cycled (sleep_ms > 0) the SLEEP that a burst edge would normally send is
// re-armed here, or they would stay awake after waking.
static void send_keepalive(void)
{
    uint8_t payload[1 + 2 * sizeof(uint64_t)];
    fill_tally_payload(payload);
    send_raw(payload, sizeof(payload));
    s_last_message_at = now_ms();

    if (s_power.sleep_ms > 0) {
        espnow_sleep_all(s_power.sleep_ms);
        s_sleep_until = now_ms() + s_power.sleep_ms;
    }
}

uint32_t espnow_loop(void)
{
    s_pump_task = xTaskGetCurrentTaskHandle();

    // Sinks first: unlike the radio burst these are not held back while the
    // receivers are asleep.
    push_sinks();

    prune_stale_tallies();

    bool active = burst_tick();
    if (s_burst_active_prev && !active && s_power.sleep_ms > 0) {
        espnow_sleep_all(s_power.sleep_ms);
        s_sleep_until = now_ms() + s_power.sleep_ms;
    }
    s_burst_active_prev = active;

    // While the receivers are asleep everything is held back; the pending
    // change stays latched and goes out when the window closes.
    if ((int32_t)(now_ms() - s_sleep_until) >= 0) {
        // Runs even while a burst is active: a new tally restarts the burst
        // with the fresh payload, exactly like startBurst() in the Arduino
        // build.
        flush_tally();

        if (s_burst_len == 0) {
            if (s_send_fail_streak >= ESPNOW_RESYNC_STREAK) {
                s_send_fail_streak = 0;
                ESP_LOGW(TAG, "send failures, forcing resync burst");
                s_tally_dirty = true;
                flush_tally();
            }
            // Idle keepalive: receivers blank after 5 s of silence. A change
            // starts the full burst above; when the switcher is quiet one
            // packet is enough to refresh the current state.
            if (s_burst_len == 0 &&
                (uint32_t)(now_ms() - s_last_message_at) >
                    keepalive_window_ms()) {
                send_keepalive();
            }
        }
    }

    return next_sleep_ms();
}

size_t espnow_tallies_snapshot(espnow_tally_info_t *out, size_t max)
{
    if (out == NULL || max == 0) {
        return 0;
    }
    if (max > ESPNOW_MAX_TALLY) {
        max = ESPNOW_MAX_TALLY;
    }
    taskENTER_CRITICAL(&s_lock);
    memcpy(out, s_tallies, max * sizeof(s_tallies[0]));
    taskEXIT_CRITICAL(&s_lock);
    return max;
}

void espnow_get_power(espnow_power_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_power;
    taskEXIT_CRITICAL(&s_lock);
}

void espnow_set_power(uint16_t sleep_ms, uint16_t burst_ms, uint16_t gap_ms)
{
    // Same clamps as applyPowerConfigStr(): ws 0..60000, wb 20..65535,
    // wg 5..wb.
    if (sleep_ms > 60000) {
        sleep_ms = 60000;
    }
    if (burst_ms < 20) {
        burst_ms = 20;
    }
    if (gap_ms < 5) {
        gap_ms = 5;
    }
    if (gap_ms > burst_ms) {
        gap_ms = burst_ms;
    }

    taskENTER_CRITICAL(&s_lock);
    s_power.sleep_ms = sleep_ms;
    s_power.burst_ms = burst_ms;
    s_power.burst_gap_ms = gap_ms;
    taskEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "power: sleep=%u burst=%u gap=%u", sleep_ms, burst_ms,
             gap_ms);
    espnow_wake();   // the pacing just changed, so recompute the sleep
}

// Same loose scan as kvInt() in src/main.cpp: find "key=" anywhere, then read
// the digits immediately after it.
static long kv_int(const char *str, const char *key)
{
    char pattern[8];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *p = strstr(str, pattern);
    if (p == NULL) {
        return -1;
    }
    p += strlen(pattern);
    long value = 0;
    bool any = false;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        any = true;
        p++;
    }
    return any ? value : -1;
}

void espnow_apply_power_str(const char *str)
{
    espnow_power_t power;
    espnow_get_power(&power);

    long value = kv_int(str, "ws");
    if (value >= 0) {
        power.sleep_ms = (uint16_t)(value > 60000 ? 60000 : value);
    }
    value = kv_int(str, "wb");
    if (value > 0) {
        power.burst_ms = (uint16_t)(value > 65535 ? 65535 : value);
    }
    value = kv_int(str, "wg");
    if (value > 0) {
        power.burst_gap_ms = (uint16_t)(value > 65535 ? 65535 : value);
    }
    espnow_set_power(power.sleep_ms, power.burst_ms, power.burst_gap_ms);
}
