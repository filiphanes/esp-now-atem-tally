#pragma once
#include <cstdint>
#include <cstddef>

// Minimal Blackmagic ATEM wire-format helpers.
//
// Pure, platform-independent layer extracted from the controller's ATEM
// client (src/atem.cpp) so it can be unit-tested on the host
// (test/atem_proto). Implements exactly the subset of the protocol the
// controller uses; wire format as reverse-documented by the SKAARHOJ Arduino
// library (GPL v3, see lib.bak/).
namespace atemmini {

static const uint16_t SWITCHER_PORT = 9910;

// UDP header flags (top 5 bits of byte 0).
enum HeaderFlags : uint8_t {
    F_ACK_REQUEST  = 0x1,   // sender wants an ack for this packet id
    F_HELLO        = 0x2,   // handshake packet
    F_RESEND       = 0x4,   // this is a retransmission
    F_REQUEST_NEXT = 0x8,   // sender asks us to (re)send something
    F_ACK          = 0x10,  // acknowledges the packet id in bytes 4-5
};

// The initial state dump from the switcher never spans more packet ids than
// this (observed up to ~32 on big switchers).
static const uint8_t MAX_INIT_PACKETS = 40;

static const uint16_t PROVISIONAL_SESSION_ID = 0x53AB;

// ---- header field accessors (12-byte UDP header) ----
uint8_t  headerFlags(const uint8_t *pkt);        // byte 0 top 5 bits
uint16_t headerLength(const uint8_t *pkt);       // byte 0 low 3 bits + byte 1
uint16_t headerSessionId(const uint8_t *pkt);    // bytes 2-3 (assigned by switcher)
uint16_t headerAckId(const uint8_t *pkt);        // bytes 4-5 (id being acked)
uint16_t headerRequestNextId(const uint8_t *pkt);// bytes 6-7 (F_REQUEST_NEXT payloads)
uint16_t headerPacketId(const uint8_t *pkt);     // bytes 10-11 (sender's local id)

// ---- packet builders ----
// All return the total packet size, or 0 if buf is too small. Buffers are
// fully written (zero-padded), no stale bytes leak through.

// 20-byte handshake opener (byte 9 = 0x3A, payload byte 0x01).
size_t buildHello(uint8_t *buf, size_t cap, uint16_t sessionId);
// Reply to the switcher's hello (byte 9 = 0x03).
size_t buildHelloAck(uint8_t *buf, size_t cap, uint16_t sessionId);
// Acknowledge one of the switcher's packets (id in bytes 4-5).
size_t buildAck(uint8_t *buf, size_t cap, uint16_t sessionId, uint16_t remoteId);
// Answer the switcher's "resend packet N" with an empty one claiming id N
// (we don't keep sent packets around; the original library does the same).
size_t buildEmptyResend(uint8_t *buf, size_t cap, uint16_t sessionId, uint16_t wantedId);
// Ask the switcher to resend everything after `afterId` during init recovery.
size_t buildResendRequest(uint8_t *buf, size_t cap, uint16_t sessionId, uint16_t afterId);

// One command segment ("CPgI", "CPvI", "DAut", ...) with raw payload.
// Increments *localIdInOut (command packets consume a local packet id,
// acks/hellos do not).
size_t buildCommand(uint8_t *buf, size_t cap, uint16_t sessionId,
                    uint16_t *localIdInOut,
                    const char *cmd4, const uint8_t *payload, size_t payloadLen);

// ---- parsing ----

// Walk the command segments packed behind the 12-byte header of a datagram
// of `n` bytes. Calls fn for every well-formed segment; malformed lengths
// stop the walk (matching the original library's "flush and give up").
// Segment layout: [len:2][?:2][cmd:4][payload...].
typedef void (*SegmentFn)(void *user, const char cmd5[5],
                          const uint8_t *payload, uint16_t len);
void forEachSegment(const uint8_t *pkt, uint16_t n, SegmentFn fn, void *user);

// "TlIn" (tally by index) payload -> bitmasks. Payload: [count:2] followed by
// one flag byte per source (bit 0 = program, bit 1 = preview). Bit i set =
// input i+1. Clamps count to 64 and to the bytes actually present.
void parseTally(const uint8_t *payload, uint16_t len,
                uint64_t *pgmOut, uint64_t *pvwOut);

} // namespace atemmini
