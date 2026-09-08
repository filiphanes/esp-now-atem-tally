#include "ATEMmini.h"

#include <cstring>

namespace atemmini {

uint8_t headerFlags(const uint8_t *pkt) {
    return pkt[0] >> 3;
}

uint16_t headerLength(const uint8_t *pkt) {
    return ((uint16_t)(pkt[0] & 0x07) << 8) | pkt[1];
}

uint16_t headerSessionId(const uint8_t *pkt) {
    return ((uint16_t)pkt[2] << 8) | pkt[3];
}

uint16_t headerAckId(const uint8_t *pkt) {
    return ((uint16_t)pkt[4] << 8) | pkt[5];
}

uint16_t headerRequestNextId(const uint8_t *pkt) {
    return ((uint16_t)pkt[6] << 8) | pkt[7];
}

uint16_t headerPacketId(const uint8_t *pkt) {
    return ((uint16_t)pkt[10] << 8) | pkt[11];
}

// Common header fill. Returns total length (== len) or 0 if cap is too small.
// Zero-fills first so no stale bytes survive in the outgoing packet.
// localIdInOut: only command packets consume a local packet id (pass NULL for
// acks/hellos/resend requests, which leave bytes 10-11 zero).
static size_t fillHeader(uint8_t *buf, size_t cap, uint8_t flags, uint16_t len,
                         uint16_t sessionId, uint16_t remoteId,
                         uint16_t *localIdInOut) {
    if (!buf || cap < len) return 0;
    memset(buf, 0, len);
    buf[0] = (flags << 3) | ((len >> 8) & 0x07);
    buf[1] = len & 0xFF;
    buf[2] = sessionId >> 8;
    buf[3] = sessionId & 0xFF;
    buf[4] = remoteId >> 8;
    buf[5] = remoteId & 0xFF;
    if (localIdInOut) {
        (*localIdInOut)++;
        buf[10] = *localIdInOut >> 8;
        buf[11] = *localIdInOut & 0xFF;
    }
    return len;
}

size_t buildHello(uint8_t *buf, size_t cap, uint16_t sessionId) {
    size_t n = fillHeader(buf, cap, F_HELLO, 12 + 8, sessionId, 0, nullptr);
    if (!n) return 0;
    buf[9] = 0x3A;   // fixed handshake magic
    buf[12] = 0x01;
    return n;
}

size_t buildHelloAck(uint8_t *buf, size_t cap, uint16_t sessionId) {
    size_t n = fillHeader(buf, cap, F_ACK, 12, sessionId, 0, nullptr);
    if (!n) return 0;
    buf[9] = 0x03;   // fixed handshake reply
    return n;
}

size_t buildAck(uint8_t *buf, size_t cap, uint16_t sessionId, uint16_t remoteId) {
    return fillHeader(buf, cap, F_ACK, 12, sessionId, remoteId, nullptr);
}

size_t buildEmptyResend(uint8_t *buf, size_t cap, uint16_t sessionId, uint16_t wantedId) {
    // Flags become AckRequest and the local packet id field carries the id
    // the switcher asked about -- exactly the trick the original library uses.
    size_t n = fillHeader(buf, cap, F_ACK_REQUEST, 12, sessionId, 0, nullptr);
    if (!n) return 0;
    buf[10] = wantedId >> 8;
    buf[11] = wantedId & 0xFF;
    return n;
}

size_t buildResendRequest(uint8_t *buf, size_t cap, uint16_t sessionId, uint16_t afterId) {
    size_t n = fillHeader(buf, cap, F_REQUEST_NEXT, 12, sessionId, 0, nullptr);
    if (!n) return 0;
    buf[6] = afterId >> 8;
    buf[7] = afterId & 0xFF;
    buf[8] = 0x01;
    return n;
}

size_t buildCommand(uint8_t *buf, size_t cap, uint16_t sessionId,
                    uint16_t *localIdInOut,
                    const char *cmd4, const uint8_t *payload, size_t payloadLen) {
    uint16_t segLen = (uint16_t)(4 + 4 + payloadLen);   // length field + cmd + payload
    size_t total = 12u + segLen;
    if (total > 0xFFFF) return 0;
    size_t n = fillHeader(buf, cap, F_ACK_REQUEST, (uint16_t)total, sessionId, 0, localIdInOut);
    if (!n) return 0;
    buf[12] = segLen >> 8;
    buf[13] = segLen & 0xFF;
    memcpy(&buf[16], cmd4, 4);
    if (payload && payloadLen) memcpy(&buf[20], payload, payloadLen);
    return n;
}

void forEachSegment(const uint8_t *pkt, uint16_t n, SegmentFn fn, void *user) {
    char cmd5[5] = {0, 0, 0, 0, 0};
    uint32_t off = 12;
    while (off + 8 <= n) {
        uint16_t segLen = ((uint16_t)pkt[off] << 8) | pkt[off + 1];
        if (segLen < 8 || off + segLen > n) break;      // malformed -> stop
        memcpy(cmd5, &pkt[off + 4], 4);
        fn(user, cmd5, &pkt[off + 8], (uint16_t)(segLen - 8));
        off += segLen;
    }
}

void parseTally(const uint8_t *payload, uint16_t len,
                uint64_t *pgmOut, uint64_t *pvwOut) {
    uint64_t pgm = 0, pvw = 0;
    if (len >= 2) {
        uint16_t sources = ((uint16_t)payload[0] << 8) | payload[1];
        if (sources > 64) sources = 64;
        if ((uint16_t)(len - 2) < sources) sources = (uint16_t)(len - 2);
        for (uint16_t i = 0; i < sources; i++) {
            if (payload[2 + i] & 0x01) pgm |= (uint64_t)1 << i;
            if (payload[2 + i] & 0x02) pvw |= (uint64_t)1 << i;
        }
    }
    *pgmOut = pgm;
    *pvwOut = pvw;
}

} // namespace atemmini
