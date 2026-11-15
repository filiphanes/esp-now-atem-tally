#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// LAN discovery for the web UI.
//
// Only ATEM is discoverable: Blackmagic switchers answer a hello broadcast on
// UDP 9910 and advertise the `_blackmagic._tcp` mDNS service. vMix has no
// Zeroconf/SSDP service (its TCP API is connect-only) and obs-websocket does
// not advertise either, so those stay manual.

#define DISCOVERY_MAX_DEVICES 8

typedef struct {
    char ip[16];
    uint16_t port;
    char name[48];   // mDNS instance name, or "ATEM" for a UDP-only reply
} discovery_device_t;

// Queries `_blackmagic._tcp` over mDNS and broadcasts the ATEM hello on the
// Ethernet interface, then collects the switchers that answer within
// `timeout_ms`. Fills `out` (up to `max`) and returns the number of unique
// devices found. Blocking; takes up to twice `timeout_ms` (one phase each).
size_t discovery_scan(discovery_device_t *out, size_t max, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
