// vMix TCP tally server on port 8099 (port of vmixServer.cpp).
//
// The Arduino build polls WiFiServer/WiFiClient from loop(); here the listener
// and its clients live in one task that blocks in select(), woken by a new
// connection, a client command, or a tally change (vmix_server_tally() pokes a
// loopback socket). vmix_server_tally() only updates the shared bitmaps plus a
// dirty flag, so every socket write stays in this task no matter which task
// (ATEM, vMix, OSC, HTTP) produced the tally, and no send can block a protocol
// task.
//
// Two deliberate divergences from the Arduino server:
//   * an input that is on program AND preview is reported as '3', which is
//     what the vMix protocol and this repo's README document; the Arduino
//     build only ever emits '1' for that case;
//   * the tally is always 64 characters, including before the first update
//     (the Arduino build sends "TALLY OK 0" until then).

#include "vmix_server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "watchdog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "vmix_srv";

#define VMIX_PORT 8099
#define VMIX_MAX_CLIENTS 8
#define VMIX_LINE_MAX 128
#define VMIX_TASK_STACK 4096
// A push that could not go out (stalled client) is retried this often; the
// client socket may never become readable, so this cannot be purely event
// driven.
#define VMIX_PENDING_RETRY_MS 20
// Longest the task waits when nothing is happening. It feeds the watchdog, so
// the wait cannot be open-ended.
#define VMIX_IDLE_WAIT_MS 1000

// "TALLY OK " + 64 state chars + CRLF + NUL.
#define VMIX_TALLY_LINE_LEN (9 + 64 + 2 + 1)

typedef struct {
    int fd;                    // -1 == free slot
    bool subscribed;
    bool pending;              // this client still needs a tally push
    // A tally line that is currently being written. A partially sent line is
    // finished before a newer state is queued, so the client never sees two
    // states spliced together.
    bool pushing;
    char push_line[VMIX_TALLY_LINE_LEN];
    size_t push_len;
    size_t push_sent;
    char line[VMIX_LINE_MAX];
    size_t line_len;
} vmix_client_t;

static int s_listen_fd = -1;
static vmix_client_t s_clients[VMIX_MAX_CLIENTS];

// Loopback UDP pair that wakes select() when the tally changes. The server
// task owns every client socket write, so vmix_server_tally() only pokes this.
static int s_wake_rx = -1;
static int s_wake_tx = -1;

// Shared with whatever task produces tally updates.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_program;
static uint64_t s_preview;
static bool s_dirty;

// "TALLY OK " + one char per input: '0' idle, '1' program, '2' preview,
// '3' both. Input 1 is the first character.
static void format_tally(char *out, uint64_t program, uint64_t preview)
{
    size_t pos = (size_t)snprintf(out, VMIX_TALLY_LINE_LEN, "TALLY OK ");
    for (uint8_t i = 0; i < 64; i++) {
        bool on_program = (program >> i) & 1;
        bool on_preview = (preview >> i) & 1;
        out[pos++] = on_program ? (on_preview ? '3' : '1')
                                : (on_preview ? '2' : '0');
    }
    out[pos++] = '\r';
    out[pos++] = '\n';
    out[pos] = '\0';
}

void vmix_server_tally(uint64_t program, uint64_t preview)
{
    taskENTER_CRITICAL(&s_lock);
    s_program = program;
    s_preview = preview;
    s_dirty = true;
    taskEXIT_CRITICAL(&s_lock);

    // Push now rather than at the next select() wake-up.
    if (s_wake_tx >= 0) {
        char byte = 0;
        send(s_wake_tx, &byte, 1, MSG_DONTWAIT);
    }
}

// Command replies are tiny, so a send that blocks means the peer is not
// reading at all: return false and let the caller drop the client rather than
// buffer a partial reply.
static bool send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, data + sent, len - sent, MSG_DONTWAIT);
        if (n <= 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static void close_client(vmix_client_t *cl)
{
    ESP_LOGI(TAG, "client %d disconnected", cl->fd);
    close(cl->fd);
    cl->fd = -1;
    cl->subscribed = false;
    cl->pending = false;
    cl->pushing = false;
    cl->push_len = 0;
    cl->push_sent = 0;
    cl->line_len = 0;
}

static void handle_command(vmix_client_t *cl, const char *cmd)
{
    if (strcmp(cmd, "TALLY") == 0) {
        // One-shot request: valid without SUBSCRIBE, and does not subscribe.
        cl->pending = true;
    } else if (strcmp(cmd, "SUBSCRIBE TALLY") == 0) {
        cl->subscribed = true;
        if (send_all(cl->fd, "SUBSCRIBE OK TALLY\r\n", 20)) {
            cl->pending = true;
        } else {
            close_client(cl);
        }
    } else if (strcmp(cmd, "UNSUBSCRIBE TALLY") == 0) {
        cl->subscribed = false;
        cl->pending = false;   // a queued push may still finish, but no new one
        if (!send_all(cl->fd, "UNSUBSCRIBE OK TALLY\r\n", 22)) {
            close_client(cl);
        }
    } else {
        if (!send_all(cl->fd, "ER\r\n", 4)) {
            close_client(cl);
        }
    }
}

// Nonblocking accept; one slot per client.
static void accept_clients(void)
{
    for (;;) {
        int fd = accept(s_listen_fd, NULL, NULL);
        if (fd < 0) {
            return;
        }
        int slot = -1;
        for (int i = 0; i < VMIX_MAX_CLIENTS; i++) {
            if (s_clients[i].fd < 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            ESP_LOGW(TAG, "no free client slot, rejecting");
            close(fd);
            continue;
        }
        s_clients[slot].fd = fd;
        s_clients[slot].subscribed = false;
        s_clients[slot].pending = false;
        s_clients[slot].pushing = false;
        s_clients[slot].push_len = 0;
        s_clients[slot].push_sent = 0;
        s_clients[slot].line_len = 0;
        ESP_LOGI(TAG, "client %d connected", fd);
    }
}

// Drain whatever is queued and reassemble lines ("...\r\n" is stripped).
static void read_client(vmix_client_t *cl)
{
    char chunk[128];
    for (;;) {
        int n = recv(cl->fd, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                char c = chunk[i];
                if (c == '\n') {
                    cl->line[cl->line_len] = '\0';
                    if (cl->line_len > 0) {
                        ESP_LOGD(TAG, "cmd: %s", cl->line);
                        handle_command(cl, cl->line);
                    }
                    cl->line_len = 0;
                    if (cl->fd < 0) {
                        return;  // handle_command closed it
                    }
                } else if (c != '\r' &&
                           cl->line_len < sizeof(cl->line) - 1) {
                    cl->line[cl->line_len++] = c;
                }
            }
            continue;
        }
        if (n == 0) {
            close_client(cl);
            return;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "recv on %d failed: errno %d", cl->fd, errno);
            close_client(cl);
        }
        return;
    }
}

// Writes as much of the in-flight line as the socket accepts. Returns true
// when the whole line is out; on EAGAIN the remainder stays queued (and
// push_sent keeps the offset) so the client gets a complete line on the next
// tick instead of a truncated one.
static bool push_line_out(vmix_client_t *cl)
{
    while (cl->push_sent < cl->push_len) {
        int n = send(cl->fd, cl->push_line + cl->push_sent,
                     cl->push_len - cl->push_sent, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            }
            ESP_LOGW(TAG, "send on %d failed: errno %d", cl->fd, errno);
            close_client(cl);
            return false;
        }
        if (n == 0) {
            close_client(cl);
            return false;
        }
        cl->push_sent += (size_t)n;
    }
    cl->pushing = false;
    cl->push_len = 0;
    cl->push_sent = 0;
    return true;
}

static void push_tally(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool changed = s_dirty;
    uint64_t program = s_program;
    uint64_t preview = s_preview;
    s_dirty = false;
    taskEXIT_CRITICAL(&s_lock);

    for (int i = 0; i < VMIX_MAX_CLIENTS; i++) {
        vmix_client_t *cl = &s_clients[i];
        if (cl->fd < 0) {
            continue;
        }
        // A one-shot TALLY request gets one push even without SUBSCRIBE.
        if (!cl->subscribed && !cl->pending && !cl->pushing) {
            continue;
        }

        // Two passes at most: finish a line that was only partially written,
        // then queue the state that changed while it was in flight.
        for (int pass = 0; pass < 2; pass++) {
            if (!cl->pushing) {
                if (!changed && !cl->pending) {
                    break;
                }
                format_tally(cl->push_line, program, preview);
                cl->push_len = strlen(cl->push_line);
                cl->push_sent = 0;
                cl->pushing = true;
                cl->pending = false;
            }
            if (!push_line_out(cl)) {
                break;  // partially sent (retried on the next tick) or closed
            }
            if (!changed) {
                break;  // the client is up to date
            }
        }
    }
}

// The wakeup pair exists purely to interrupt select(); the payloads are
// meaningless and an overflowing queue of them is harmless.
static void wake_init(void)
{
    s_wake_rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_wake_rx < 0) {
        ESP_LOGW(TAG, "wakeup socket failed: errno %d", errno);
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001u);   // 127.0.0.1
    addr.sin_port = 0;                            // any free port
    socklen_t addr_len = sizeof(addr);
    if (bind(s_wake_rx, (const struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        getsockname(s_wake_rx, (struct sockaddr *)&addr, &addr_len) < 0) {
        ESP_LOGW(TAG, "wakeup socket bind failed: errno %d", errno);
        close(s_wake_rx);
        s_wake_rx = -1;
        return;
    }

    s_wake_tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_wake_tx >= 0 &&
        connect(s_wake_tx, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s_wake_tx);
        s_wake_tx = -1;
    }

    // Neither end may block: the poke runs in whatever task produced the
    // tally, and the drain runs once per select() wake-up.
    if (s_wake_tx >= 0) {
        fcntl(s_wake_rx, F_SETFL, fcntl(s_wake_rx, F_GETFL, 0) | O_NONBLOCK);
        fcntl(s_wake_tx, F_SETFL, fcntl(s_wake_tx, F_GETFL, 0) | O_NONBLOCK);
    } else {
        close(s_wake_rx);
        s_wake_rx = -1;
    }
}

static void wake_drain(void)
{
    char scratch[16];
    while (s_wake_rx >= 0 &&
           recv(s_wake_rx, scratch, sizeof(scratch), MSG_DONTWAIT) > 0) {
    }
}

// One pass: block until something happens, then service the listener, the
// clients and any pending tally push.
static void vmix_server_task(void *arg)
{
    (void)arg;
    watchdog_subscribe();
    for (;;) {
        watchdog_feed();
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(s_listen_fd, &read_fds);
        int max_fd = s_listen_fd;
        if (s_wake_rx >= 0) {
            FD_SET(s_wake_rx, &read_fds);
            if (s_wake_rx > max_fd) {
                max_fd = s_wake_rx;
            }
        }

        bool any_pending = false;
        for (int i = 0; i < VMIX_MAX_CLIENTS; i++) {
            if (s_clients[i].fd >= 0) {
                FD_SET(s_clients[i].fd, &read_fds);
                if (s_clients[i].fd > max_fd) {
                    max_fd = s_clients[i].fd;
                }
                any_pending = any_pending || s_clients[i].pending ||
                              s_clients[i].pushing;
            }
        }

        // Block until a connection, a command or a tally change. The wait is
        // bounded either way: a stalled client may never make its socket
        // readable (so a pending push is retried on a timer), and this task
        // feeds the watchdog.
        struct timeval tv = {0, 0};
        if (any_pending) {
            tv.tv_usec = VMIX_PENDING_RETRY_MS * 1000;
        } else {
            tv.tv_sec = VMIX_IDLE_WAIT_MS / 1000;
            tv.tv_usec = (VMIX_IDLE_WAIT_MS % 1000) * 1000;
        }
        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ready < 0) {
            // A bad descriptor would otherwise spin this task at 100% CPU.
            ESP_LOGW(TAG, "select failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        wake_drain();
        if (FD_ISSET(s_listen_fd, &read_fds)) {
            accept_clients();
        }
        for (int i = 0; i < VMIX_MAX_CLIENTS; i++) {
            if (s_clients[i].fd >= 0 &&
                FD_ISSET(s_clients[i].fd, &read_fds)) {
                read_client(&s_clients[i]);
            }
        }
        push_tally();
    }
}

esp_err_t vmix_server_start(void)
{
    for (int i = 0; i < VMIX_MAX_CLIENTS; i++) {
        s_clients[i].fd = -1;
    }

    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_fd < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        return ESP_FAIL;
    }

    int reuse = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Nonblocking: accept_clients() just takes whatever is queued.
    int flags = fcntl(s_listen_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(s_listen_fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(VMIX_PORT);
    if (bind(s_listen_fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind on port %d failed: errno %d", VMIX_PORT, errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        return ESP_FAIL;
    }
    if (listen(s_listen_fd, VMIX_MAX_CLIENTS) < 0) {
        ESP_LOGE(TAG, "listen failed: errno %d", errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        return ESP_FAIL;
    }

    wake_init();

    BaseType_t ok = xTaskCreate(vmix_server_task, "vmix_srv", VMIX_TASK_STACK,
                                NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        close(s_listen_fd);
        s_listen_fd = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "TCP tally server on port %d", VMIX_PORT);
    return ESP_OK;
}
