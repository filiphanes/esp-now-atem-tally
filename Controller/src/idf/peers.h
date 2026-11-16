#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define PEERS_MAX 8

typedef struct {
    char ip[16];
    char host[64];
    uint16_t port;
} peer_info_t;

// Starts the background mDNS browse for other tally controllers.
esp_err_t peers_start(void);

// Copies the currently known peers (most recent scan wins).
size_t peers_snapshot(peer_info_t *out, size_t max);