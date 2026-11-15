// Minimal ATEM client for the pure-IDF controller.
//
// Port of src/atem.cpp to ESP-IDF: the same state machine (hello handshake,
// packet ACKs, missed-init-packet recovery and a 5 s idle reconnect watchdog)
// and the same wire format via the shared lib/ATEMmini helpers, but on BSD
// nonblocking UDP and esp_timer instead of WiFiUDP/millis(). Deliberate
// differences from the Arduino build:
//   * the switcher port comes from the config (g_config.port) instead of the
//     hardcoded 9910;
//   * the client runs in its own task that blocks in select() on the socket
//     (plus a loopback wake pair for /cut), so a tally packet is handled as
//     soon as it arrives instead of at the next loop() tick;
//   * /cut and /auto never send from the HTTP task: they leave a pending
//     switch request for the client task to perform, so all UDP I/O stays in
//     one task and `s_pkt`/the socket never race the HTTP task.
//
// Everything else the full SKAARHOJ library tracks is ignored.

#include "atem.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ATEMmini.h>

#include "config.h"
#include "espnow.h"
#include "watchdog.h"

using namespace atemmini;

static const char *TAG = "atem";

static const size_t PKT_MAX = 2048;         // rx buffer (>= one MTU datagram)
static const int64_t TIMEOUT_US = 5000 * 1000;  // no traffic => reconnect

// ---- connection state (only touched from the client task) ----
static int s_sock = -1;
static struct sockaddr_in s_peer;
static uint16_t s_session_id = PROVISIONAL_SESSION_ID;
static uint16_t s_local_packet_id = 0;      // our outgoing command packet counter
static uint16_t s_last_remote_packet_id = 0;
static uint8_t s_init_payload_sent_at_id = MAX_INIT_PACKETS;
static bool s_connected = false;            // hello handshake done
static bool s_init_payload_sent = false;    // 12-byte-only packet => dump complete
static bool s_has_initialized = false;      // all init packets received/resent
static bool s_waiting_for_incoming = false; // we asked for a resend, wait for it
static bool s_was_connected = false;        // for connect/disconnect logging
static int64_t s_last_contact_us = 0;
static uint8_t s_missed_init[(MAX_INIT_PACKETS + 7) / 8];  // bitmap of missing ids
static uint8_t s_pkt[PKT_MAX];

// Loopback datagram pair used to break the client task out of select() when
// the HTTP task queues a switch, so the task can block for a second instead of
// polling every 20 ms. Same pattern as vmix_server.c.
static int s_wake_rx = -1;
static int s_wake_tx = -1;

// ---- tally state (bit i-1 == input i) ----
static uint64_t s_program_bits = 0;
static uint64_t s_preview_bits = 0;

// ---- pending switch request (HTTP task -> client task) ----
static portMUX_TYPE s_switch_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_switch_pending = false;
static uint8_t s_switch_tally = 0;
static bool s_switch_auto = false;

bool atem_is_connected(void)
{
    return s_connected;
}

// ---------------------------------------------------------------------------
// Send helpers
// ---------------------------------------------------------------------------

static void send_pkt(size_t len)
{
    if (s_sock < 0) {
        return;
    }
    int sent = sendto(s_sock, s_pkt, len, 0,
                      (const struct sockaddr *)&s_peer, sizeof(s_peer));
    if (sent < 0) {
        ESP_LOGW(TAG, "sendto failed: errno %d", errno);
    }
}

// Send one command segment ("CPgI", "CPvI", "DAut", ...) with raw payload.
static void send_command(const char *cmd4, const uint8_t *payload,
                         size_t payload_len)
{
    if (!s_connected) {
        return;
    }
    size_t n = buildCommand(s_pkt, PKT_MAX, s_session_id, &s_local_packet_id,
                            cmd4, payload, payload_len);
    if (n) {
        send_pkt(n);
    }
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

// Fresh port number per attempt, like the original library: reconnecting from
// the same port keeps the ATEM tied to the dead session.
static void atem_connect(void)
{
    s_local_packet_id = 0;
    s_connected = false;
    s_init_payload_sent = false;
    s_has_initialized = false;
    s_waiting_for_incoming = false;
    s_session_id = PROVISIONAL_SESSION_ID;  // replaced by the switcher's
    s_init_payload_sent_at_id = MAX_INIT_PACKETS;
    s_last_contact_us = esp_timer_get_time();   // counts as an attempt
    memset(s_missed_init, 0xFF, sizeof(s_missed_init));

    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "UDP socket failed: errno %d", errno);
        return;
    }

    // Non-blocking: the loop drains whatever is queued and moves on.
    int flags = fcntl(s_sock, F_GETFL, 0);
    if (flags < 0 || fcntl(s_sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGW(TAG, "fcntl O_NONBLOCK failed: errno %d", errno);
    }

    // Bind an ephemeral-ish port so a reconnect looks like a new client.
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons((uint16_t)(50100 + esp_random() % (65300 - 50100)));
    if (bind(s_sock, (const struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "UDP bind failed: errno %d", errno);
        close(s_sock);
        s_sock = -1;
        return;
    }

    size_t n = buildHello(s_pkt, PKT_MAX, s_session_id);
    send_pkt(n);
}

// ---------------------------------------------------------------------------
// Wake-up pair (HTTP task -> client task)
// ---------------------------------------------------------------------------

// Create a tiny loopback UDP pair: the HTTP task pokes one byte into it and
// select() wakes immediately, so /cut does not have to wait for the 1 s
// watchdog timeout.
static void wake_init(void)
{
    int rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (rx < 0) {
        ESP_LOGW(TAG, "wakeup socket failed: errno %d", errno);
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001u);   // 127.0.0.1
    addr.sin_port = 0;                            // any free port
    socklen_t addr_len = sizeof(addr);
    if (bind(rx, (const struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        getsockname(rx, (struct sockaddr *)&addr, &addr_len) < 0) {
        ESP_LOGW(TAG, "wakeup socket bind failed: errno %d", errno);
        close(rx);
        return;
    }

    int tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (tx < 0 ||
        connect(tx, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "wakeup socket connect failed: errno %d", errno);
        if (tx >= 0) {
            close(tx);
        }
        close(rx);
        return;
    }

    // Neither end may block: the poke comes from the HTTP task, the drain from
    // the client task. Publish the descriptors only once both are connected,
    // so a /cut racing this setup never pokes a half-initialised socket.
    fcntl(rx, F_SETFL, fcntl(rx, F_GETFL, 0) | O_NONBLOCK);
    fcntl(tx, F_SETFL, fcntl(tx, F_GETFL, 0) | O_NONBLOCK);
    s_wake_rx = rx;
    s_wake_tx = tx;
}

static void wake_poke(void)
{
    if (s_wake_tx >= 0) {
        char byte = 0;
        send(s_wake_tx, &byte, 1, MSG_DONTWAIT);
    }
}

static void wake_drain(void)
{
    char scratch[8];
    while (s_wake_rx >= 0 &&
           recv(s_wake_rx, scratch, sizeof(scratch), MSG_DONTWAIT) > 0) {
    }
}

// ---------------------------------------------------------------------------
// Receive path
// ---------------------------------------------------------------------------

// Segment dispatcher: only "TlIn" matters to us.
static void on_segment(void *user, const char cmd5[5], const uint8_t *payload,
                       uint16_t len)
{
    (void)user;
    if (strcmp(cmd5, "TlIn") == 0 && len >= 2) {
        parseTally(payload, len, &s_program_bits, &s_preview_bits);
        // Same trigger point as the old lib callback: every tally packet
        // pushes the fresh state downstream to the WebSocket/ESP-NOW pump.
        espnow_tally(&s_program_bits, &s_preview_bits);
    }
}

// ---------------------------------------------------------------------------
// Pending switch (requested by the HTTP task)
// ---------------------------------------------------------------------------

static void do_pending_switch(void)
{
    bool pending;
    uint8_t tally;
    bool auto_transition;

    taskENTER_CRITICAL(&s_switch_lock);
    pending = s_switch_pending;
    tally = s_switch_tally;
    auto_transition = s_switch_auto;
    s_switch_pending = false;
    taskEXIT_CRITICAL(&s_switch_lock);

    if (!pending) {
        return;
    }
    if (tally == 0) {
        return;
    }
    if (!s_connected) {
        ESP_LOGW(TAG, "switch to %u dropped: not connected", tally);
        return;
    }

    // M/E 0 in byte 0, input number as big-endian 16-bit video source.
    uint8_t p[4] = {0, 0, 0, tally};
    if (auto_transition) {
        ESP_LOGI(TAG, "auto -> %u", tally);
        send_command("CPvI", p, sizeof(p));   // preview first...
        send_command("DAut", p, sizeof(p));   // ...then AUTO transition
    } else {
        ESP_LOGI(TAG, "cut -> %u", tally);
        send_command("CPgI", p, sizeof(p));   // straight to program
    }
}

void atem_request_switch(uint8_t tally_num, bool auto_transition)
{
    if (tally_num == 0) {
        return;
    }
    taskENTER_CRITICAL(&s_switch_lock);
    s_switch_tally = tally_num;
    s_switch_auto = auto_transition;
    s_switch_pending = true;
    taskEXIT_CRITICAL(&s_switch_lock);

    wake_poke();
}

// ---------------------------------------------------------------------------
// Client task
// ---------------------------------------------------------------------------

// Blocks until a datagram or a switch request arrives. The receive path is
// event-driven; the one-second timeout only bounds the watchdog feed and the
// reconnect check, and the wake pair makes /cut immediate.
static void wait_for_rx(void)
{
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(s_sock, &read_fds);
    int max_fd = s_sock;
    if (s_wake_rx >= 0) {
        FD_SET(s_wake_rx, &read_fds);
        if (s_wake_rx > max_fd) {
            max_fd = s_wake_rx;
        }
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    select(max_fd + 1, &read_fds, NULL, NULL, &tv);
}

// One pass: wait for a packet, drain what arrived, then run init recovery, the
// pending switch and the reconnect watchdog.
static void atem_run(void)
{
    if (s_sock >= 0) {
        wait_for_rx();
    }
    wake_drain();   // the poke is only a wake-up; the request itself is a flag

    // Drain all pending datagrams; each recv() consumes exactly one, so a bad
    // length cannot corrupt the next one.
    while (s_sock >= 0) {
        int n = recv(s_sock, s_pkt, PKT_MAX, 0);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "recv failed: errno %d", errno);
            }
            break;
        }
        if (n < 12) {
            continue;
        }

        s_session_id = headerSessionId(s_pkt);
        uint8_t flags = headerFlags(s_pkt);
        s_last_remote_packet_id = headerPacketId(s_pkt);
        uint16_t packet_len = headerLength(s_pkt);

        if (s_last_remote_packet_id < MAX_INIT_PACKETS) {
            s_missed_init[s_last_remote_packet_id >> 3] &=
                ~(1 << (s_last_remote_packet_id & 7));
        }

        if ((uint16_t)n != packet_len) {
            continue;      // header lies -> drop it
        }

        s_last_contact_us = esp_timer_get_time();
        s_waiting_for_incoming = false;

        if (flags & F_HELLO) {
            s_connected = true;
            size_t out = buildHelloAck(s_pkt, PKT_MAX, s_session_id);
            send_pkt(out);
        }

        // A 12-byte-only packet marks the end of the initial state dump.
        if (!s_init_payload_sent && n == 12 && s_last_remote_packet_id > 1) {
            s_init_payload_sent = true;
            s_init_payload_sent_at_id = s_last_remote_packet_id;
        }

        if (s_init_payload_sent && (flags & F_ACK_REQUEST) &&
            (s_has_initialized || !(flags & F_RESEND))) {
            // Acknowledge every command packet once we're up.
            size_t out = buildAck(s_pkt, PKT_MAX, s_session_id,
                                  s_last_remote_packet_id);
            send_pkt(out);
        } else if (s_init_payload_sent && (flags & F_REQUEST_NEXT) &&
                   s_has_initialized) {
            // The ATEM lost one of our packets. We don't keep sent packets
            // around, so answer with an empty one (same as the old lib).
            size_t out = buildEmptyResend(s_pkt, PKT_MAX, s_session_id,
                                          headerRequestNextId(s_pkt));
            send_pkt(out);
        }

        if (!(flags & F_HELLO) && packet_len > 12) {
            forEachSegment(s_pkt, (uint16_t)n, on_segment, nullptr);
        }
    }

    // Recover any init packets lost during the state dump, then go live.
    if (!s_has_initialized && s_init_payload_sent && !s_waiting_for_incoming) {
        bool asked = false;
        for (uint8_t i = 1; i < s_init_payload_sent_at_id &&
                            i <= MAX_INIT_PACKETS; i++) {
            if (s_missed_init[i >> 3] & (1 << (i & 7))) {
                size_t out = buildResendRequest(s_pkt, PKT_MAX, s_session_id,
                                                i - 1);
                send_pkt(out);
                s_waiting_for_incoming = true;
                asked = true;
                break;
            }
        }
        if (!asked) {
            s_has_initialized = true;
        }
    }

    do_pending_switch();

    // Connection logging + watchdog.
    if (s_connected != s_was_connected) {
        s_was_connected = s_connected;
        ESP_LOGI(TAG, "%s", s_connected ? "connected" : "disconnected");
    }
    if (esp_timer_get_time() - s_last_contact_us >= TIMEOUT_US) {
        ESP_LOGW(TAG, "timed out - reconnecting");
        atem_connect();
    }
}

// The only caller of atem_run(). When the socket failed to open there is
// nothing to wait on, so pace the watchdog retry instead of spinning.
static void atem_task(void *arg)
{
    (void)arg;
    watchdog_subscribe();
    for (;;) {
        watchdog_feed();
        atem_run();
        if (s_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

esp_err_t atem_setup(void)
{
    memset(&s_peer, 0, sizeof(s_peer));
    s_peer.sin_family = AF_INET;
    s_peer.sin_addr.s_addr = g_config.ip.addr;   // already network order
    s_peer.sin_port = htons(g_config.port);

    char ip[16];
    esp_ip4addr_ntoa(&g_config.ip, ip, sizeof(ip));
    ESP_LOGI(TAG, "target %s:%u", ip, g_config.port);

    wake_init();
    atem_connect();

    BaseType_t ok = xTaskCreate(atem_task, "atem", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "client task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
