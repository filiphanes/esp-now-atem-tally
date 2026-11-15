// OSC tally control for the pure-IDF controller (port of src/osc.cpp).
//
// Addresses supported (same as the Arduino build):
//   /tally ii           first int = program, second int = preview
//   /tally,N            N becomes program (preview cleared)
//   /program,N          program only; multiple int args accepted
//   /preview,N          preview only; multiple int args accepted
//   /color,N            tally N becomes red
//   /brightness,N [i]   tally N brightness (default 255)
//   /id,N [i]           tally N camera id (default 1)
// N can also be a list/range: ",1,3,5-8" (see parse_osc_address).
//
// The packet parser follows OSC 1.0: the type tag string begins at the next
// 4-byte boundary after the address's NUL terminator and includes its leading
// comma, and int32 arguments are big-endian. A message whose arguments are all
// int32 may omit the type tag, which is what the README examples and many
// simple senders do. The Arduino implementation reads the type tag one byte
// late and skips the byte swap, so typed arguments there do not decode standard
// OSC; this port intentionally uses the correct layout.

#include "osc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_log.h"
#include "espnow.h"
#include "watchdog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "osc";

#define OSC_PORT 57110
#define OSC_MAX_PACKET 512

static int s_osc_sock = -1;

static uint64_t bitn(uint8_t n)
{
    return (uint64_t)1 << (n - 1);
}

static size_t align4(size_t n)
{
    return (n + 3) & ~(size_t)3;
}

// Ported from parseOscAddress(); also accepts '0' so tally ids 10..64 work.
static uint64_t parse_osc_address(const char *addr, int len)
{
    uint64_t bits = 0;
    int tally_num = 0;
    bool range_mode = false;
    int range_start = 0;

    for (int i = 0; i < len; i++) {
        char c = addr[i];
        if (c >= '0' && c <= '9') {
            tally_num = tally_num * 10 + (c - '0');
        } else if (c == ',') {
            if (tally_num > 0 && tally_num <= 64) {
                bits |= bitn((uint8_t)tally_num);
            }
            tally_num = 0;
            range_mode = false;
        } else if (c == '-') {
            if (tally_num > 0 && tally_num <= 64) {
                range_start = tally_num;
                range_mode = true;
                tally_num = 0;
            }
        } else if (c == '{') {
            range_start = tally_num;
            tally_num = 0;
            range_mode = true;
        } else if (c == '}') {
            if (range_start > 0 && tally_num > range_start) {
                for (int j = range_start; j <= tally_num && j <= 64; j++) {
                    bits |= bitn((uint8_t)j);
                }
            }
            tally_num = 0;
            range_start = 0;
            range_mode = false;
        } else if (c == '/') {
            if (tally_num > 0 && tally_num <= 64) {
                bits |= bitn((uint8_t)tally_num);
            }
            tally_num = 0;
        }
    }
    if (tally_num > 0 && tally_num <= 64) {
        if (range_mode && range_start > 0) {
            for (int j = range_start; j <= tally_num && j <= 64; j++) {
                bits |= bitn((uint8_t)j);
            }
        } else {
            bits |= bitn((uint8_t)tally_num);
        }
    }
    return bits;
}

static int32_t read_int32(const char *p)
{
    int32_t val;
    memcpy(&val, p, sizeof(val));
    return (int32_t)ntohl((uint32_t)val);
}

// Size in bytes of one argument of type `t` at `p`, including OSC padding.
// Returns -1 for a type whose size this parser does not know (or for a
// truncated packet), which stops argument parsing: without the size of the
// current argument there is no way to find the next one.
//
// 'h' is treated as a 32-bit int, matching the Arduino build; true OSC 1.1
// int64 arguments are not supported.
static int arg_size(char t, const char *p, int remaining)
{
    switch (t) {
    case 'i':
    case 'f':
    case 'c':
    case 'r':
    case 'm':
    case 'h':
        return remaining >= 4 ? 4 : -1;
    case 'd':
        return remaining >= 8 ? 8 : -1;
    case 's':
    case 'S': {
        if (remaining < 4) {
            return -1;
        }
        size_t str_len = strnlen(p, (size_t)remaining);
        if (str_len == (size_t)remaining) {
            return -1;   // no NUL terminator inside the packet
        }
        size_t total = align4(str_len + 1);
        return total <= (size_t)remaining ? (int)total : -1;
    }
    case 'b': {
        if (remaining < 4) {
            return -1;
        }
        int32_t blob_len = read_int32(p);
        if (blob_len < 0) {
            return -1;
        }
        size_t total = 4 + align4((size_t)blob_len);
        return total <= (size_t)remaining ? (int)total : -1;
    }
    case 'T':
    case 'F':
    case 'N':
    case 'I':
        return 0;   // no payload
    default:
        return -1;
    }
}

static void handle_packet(char *buf, int len)
{
    int addr_len = (int)strnlen(buf, (size_t)len);
    if (addr_len < 2) {
        return;
    }

    // OSC strings are NUL-terminated, so the next field starts at the next
    // 4-byte boundary *after* the terminator (addr_len + 1), not after the
    // last character. Using align4(addr_len) breaks "/program" and
    // "/preview" because their 8-char names are already 4-byte aligned.
    size_t type_start = align4((size_t)addr_len + 1);
    const char *types = "";
    int type_count = 0;
    const char *args = "";
    int args_len = 0;
    bool bare_args = false;

    if (type_start < (size_t)len && buf[type_start] == ',') {
        size_t type_total = strnlen(buf + type_start, (size_t)len - type_start);
        if (type_start + type_total < (size_t)len) {
            types = buf + type_start + 1;
            type_count = (int)type_total - 1;
            size_t args_start = type_start + align4(type_total + 1);
            if (args_start <= (size_t)len) {
                args = buf + args_start;
                args_len = len - (int)args_start;
            }
        }
    } else if (type_start < (size_t)len) {
        // Some senders (and the README examples) omit the type tag when every
        // argument is an int32; treat the remaining words as int32.
        bare_args = true;
        args = buf + type_start;
        args_len = len - (int)type_start;
        type_count = args_len / 4;
    }

    const char *addr = buf;
    if (strncmp(addr, "/tally", 6) == 0) {
        uint64_t program = 0;
        uint64_t preview = 0;
        int arg_off = 0;
        int int_args = 0;

        for (int i = 0; i < type_count; i++) {
            char t = bare_args ? 'i' : types[i];
            if (t == 'i' || t == 'h') {
                if (arg_off + 4 > args_len) {
                    break;
                }
                int32_t val = read_int32(args + arg_off);
                if (val > 0 && val <= 64) {
                    if (int_args == 0) {
                        program |= bitn((uint8_t)val);
                    } else {
                        preview |= bitn((uint8_t)val);
                    }
                }
                int_args++;
                arg_off += 4;
                continue;
            }
            int size = arg_size(t, args + arg_off, args_len - arg_off);
            if (size < 0) {
                break;
            }
            arg_off += size;
        }

        if (int_args == 0) {
            // No typed arguments: "/tally,3" carries the ids in the address,
            // with the same list/range syntax as /color, /brightness and /id.
            // Sets program and clears preview, like "/tally ii(3)(0)".
            program = parse_osc_address(addr + 6, addr_len - 6);
        }
        if (program || preview) {
            espnow_tally(&program, &preview);
        }
    } else if (strncmp(addr, "/program", 8) == 0) {
        uint64_t program = 0;
        int arg_off = 0;

        for (int i = 0; i < type_count; i++) {
            char t = bare_args ? 'i' : types[i];
            if (t == 'i' || t == 'h') {
                if (arg_off + 4 > args_len) {
                    break;
                }
                int32_t val = read_int32(args + arg_off);
                if (val > 0 && val <= 64) {
                    program |= bitn((uint8_t)val);
                }
                arg_off += 4;
                continue;
            }
            int size = arg_size(t, args + arg_off, args_len - arg_off);
            if (size < 0) {
                break;
            }
            arg_off += size;
        }
        if (program) {
            espnow_tally(&program, NULL);
        }
    } else if (strncmp(addr, "/preview", 8) == 0) {
        uint64_t preview = 0;
        int arg_off = 0;

        for (int i = 0; i < type_count; i++) {
            char t = bare_args ? 'i' : types[i];
            if (t == 'i' || t == 'h') {
                if (arg_off + 4 > args_len) {
                    break;
                }
                int32_t val = read_int32(args + arg_off);
                if (val > 0 && val <= 64) {
                    preview |= bitn((uint8_t)val);
                }
                arg_off += 4;
                continue;
            }
            int size = arg_size(t, args + arg_off, args_len - arg_off);
            if (size < 0) {
                break;
            }
            arg_off += size;
        }
        if (preview) {
            espnow_tally(NULL, &preview);
        }
    } else if (strncmp(addr, "/color", 6) == 0) {
        uint64_t bits = parse_osc_address(addr + 6, addr_len - 6);
        if (bits == 0) {
            return;
        }
        espnow_color(0xff0000, &bits);
    } else if (strncmp(addr, "/brightness", 10) == 0) {
        uint64_t bits = parse_osc_address(addr + 10, addr_len - 10);
        if (bits == 0) {
            return;
        }
        uint8_t brightness = 255;
        if (type_count > 0 && (bare_args || types[0] == 'i') &&
            args_len >= 4) {
            brightness = (uint8_t)(read_int32(args) & 0xff);
        }
        espnow_brightness(brightness, &bits);
    } else if (strncmp(addr, "/id", 3) == 0) {
        uint64_t bits = parse_osc_address(addr + 3, addr_len - 3);
        if (bits == 0) {
            return;
        }
        uint8_t cam_id = 1;
        if (type_count > 0 && (bare_args || types[0] == 'i') &&
            args_len >= 4) {
            cam_id = (uint8_t)(read_int32(args) & 0xff);
        }
        espnow_camid(cam_id, &bits);
    }
}

static void osc_task(void *arg)
{
    (void)arg;
    watchdog_subscribe();

    char buf[OSC_MAX_PACKET + 1];
    for (;;) {
        watchdog_feed();
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(s_osc_sock, buf, OSC_MAX_PACKET, 0,
                           (struct sockaddr *)&from, &from_len);
        if (len <= 0) {
            continue;
        }
        buf[len] = '\0';
        handle_packet(buf, len);
    }
}

esp_err_t osc_start(void)
{
    s_osc_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_osc_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return ESP_FAIL;
    }

    // The receive timeout is what lets osc_task() feed the watchdog: without
    // it a silent network would block recvfrom() forever.
    struct timeval rx_timeout = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_osc_sock, SOL_SOCKET, SO_RCVTIMEO, &rx_timeout,
               sizeof(rx_timeout));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(OSC_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_osc_sock, (struct sockaddr *)&bind_addr,
             sizeof(bind_addr)) != 0) {
        ESP_LOGE(TAG, "bind(%d) failed: errno %d", OSC_PORT, errno);
        close(s_osc_sock);
        s_osc_sock = -1;
        return ESP_FAIL;
    }

    BaseType_t ok = xTaskCreate(osc_task, "osc", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        close(s_osc_sock);
        s_osc_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "OSC listening on UDP %d", OSC_PORT);
    return ESP_OK;
}
