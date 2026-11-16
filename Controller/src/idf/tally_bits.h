#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Shared helpers for everything that selects tally inputs 1..64 by bitmask
// (bit i-1 == input i). The range matches ESPNOW_MAX_TALLY in espnow.h and
// TALLY_COUNT in Receiver/main.c; it used to be re-implemented (as bitn()) in
// web.c, osc.c, vmix_client.c and obs_client.c.

#define TALLY_MAX 64

// Signal opcodes the receivers render (SIGNAL_CHANGE..SIGNAL_OK in
// Receiver/main.c). Values below 12 are digit glyphs on the matrix, not
// signals, so the controllers never send them as a signal.
#define TALLY_SIGNAL_MIN 12
#define TALLY_SIGNAL_MAX 23

static inline uint64_t tally_bitn(uint8_t n)
{
    return (uint64_t)1 << (n - 1);
}

// True for a tally number the receivers can address (camId 1..64).
static inline bool tally_id_valid(long n)
{
    return n >= 1 && n <= TALLY_MAX;
}

// True for a signal opcode the receivers render.
static inline bool tally_signal_valid(long n)
{
    return n >= TALLY_SIGNAL_MIN && n <= TALLY_SIGNAL_MAX;
}

#ifdef __cplusplus
}
#endif
