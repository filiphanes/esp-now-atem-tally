// Minimal ATEM client.
//
// Reimplements just the parts of the SKAARHOJ ATEM Arduino library
// (Kasper Skårhøj, SKAARHOJ K/S, GPL v3 -- see lib.bak/ATEMbase + ATEMstd)
// that this controller needs. Speaks the same UDP wire protocol on port 9910:
//
//   * connection handshake (hello packet, session id), packet ACKs,
//     missed-init-packet recovery and a 5 s idle reconnect watchdog
//   * receiving "TlIn" (tally by index) -> program/preview bitmasks,
//     pushed downstream via espnow_tally() on every tally packet
//   * sending "CPvI" (set preview input), "CPgI" (set program input) and
//     "DAut" (AUTO transition) on M/E 0
//
// The pure wire-format builders/parsers live in lib/ATEMmini and are covered
// by host unit tests (test/atem_proto); this file is the stateful glue.
// All other switcher state the full library tracks is ignored.

#include <Arduino.h>
#include <WiFiUdp.h>

#include <ATEMmini.h>

#include "atem.h"
#include "espnow.h"
#include "main.h"

using namespace atemmini;

static const uint16_t PKT_MAX = 2048;           // rx buffer (>= one MTU datagram)
static const unsigned long TIMEOUT_MS = 5000;   // no traffic for this long => reconnect

// ---- connection state ----
static WiFiUDP udp;
static IPAddress swIp;
static uint16_t sessionId = PROVISIONAL_SESSION_ID;
static uint16_t localPacketId = 0;      // our outgoing command packet counter
static uint16_t lastRemotePacketId = 0;
static uint16_t initPayloadSentAtId = MAX_INIT_PACKETS;
static bool connectedFlag = false;      // hello handshake done
static bool initPayloadSent = false;    // saw a 12-byte-only packet => state dump complete
static bool hasInitialized = false;     // all init packets received/resent
static bool waitingForIncoming = false; // we asked for a resend, wait for it
static bool wasConnected = false;       // for connect/disconnect logging
static unsigned long lastContact = 0;
static uint8_t missedInit[(MAX_INIT_PACKETS + 7) / 8];  // bitmap of missing init ids
static uint8_t pkt[PKT_MAX];

// ---- tally state (bit i-1 == input i) ----
static uint64_t progBits = 0;
static uint64_t prevBits = 0;

uint64_t getProgramBits() { return progBits; }
uint64_t getPreviewBits() { return prevBits; }
bool atem_isConnected() { return connectedFlag; }

// ---------------------------------------------------------------------------
// Send helpers
// ---------------------------------------------------------------------------

static void sendPkt(size_t len)
{
    udp.beginPacket(swIp, SWITCHER_PORT);
    udp.write(pkt, len);
    udp.endPacket();
}

// Send one command segment ("CPgI", "CPvI", "DAut", ...) with raw payload.
static void sendCommand(const char *cmd4, const uint8_t *payload, size_t payloadLen)
{
    if (!connectedFlag) return;
    size_t n = buildCommand(pkt, PKT_MAX, sessionId, &localPacketId,
                            cmd4, payload, payloadLen);
    if (n) sendPkt(n);
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

// Fresh port number per attempt, like the original library: reconnecting from
// the same port keeps the ATEM tied to the dead session.
static void atem_connect()
{
    localPacketId = 0;
    connectedFlag = false;
    initPayloadSent = false;
    hasInitialized = false;
    waitingForIncoming = false;
    sessionId = PROVISIONAL_SESSION_ID; // replaced by the switcher's on handshake
    initPayloadSentAtId = MAX_INIT_PACKETS;
    lastContact = millis();             // counts as an attempt
    memset(missedInit, 0xFF, sizeof(missedInit));

    udp.stop();
    int port = random(50100, 65300);
    if (!udp.begin(port)) {
        Serial.println("ATEM: UDP bind failed");
        return;
    }

    size_t n = buildHello(pkt, PKT_MAX, sessionId);
    sendPkt(n);
}

// ---------------------------------------------------------------------------
// Receive path
// ---------------------------------------------------------------------------

// Segment dispatcher: only "TlIn" matters to us.
static void onSegment(void *user, const char cmd5[5],
                      const uint8_t *payload, uint16_t len)
{
    (void)user;
    if (strcmp(cmd5, "TlIn") == 0 && len >= 2) {
        parseTally(payload, len, &progBits, &prevBits);
        espnow_tally(&progBits, &prevBits);  // same trigger point as the old lib callback
    }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void atem_loop()
{
    // Drain all pending datagrams; each parsePacket()/read() pair consumes
    // exactly one, so a bad length cannot corrupt the next one.
    while (true) {
        int size = udp.parsePacket();
        if (size <= 0) break;
        int n = udp.read(pkt, size < PKT_MAX ? size : PKT_MAX);
        if (n < 12) continue;

        sessionId = headerSessionId(pkt);
        uint8_t flags = headerFlags(pkt);
        lastRemotePacketId = headerPacketId(pkt);
        uint16_t packetLen = headerLength(pkt);

        if (lastRemotePacketId < MAX_INIT_PACKETS)
            missedInit[lastRemotePacketId >> 3] &= ~(1 << (lastRemotePacketId & 7));

        if ((uint16_t)size != packetLen) continue;      // header lies -> drop it

        lastContact = millis();
        waitingForIncoming = false;

        if (flags & F_HELLO) {
            connectedFlag = true;
            size_t out = buildHelloAck(pkt, PKT_MAX, sessionId);
            sendPkt(out);
        }

        // A 12-byte-only packet marks the end of the initial state dump.
        if (!initPayloadSent && n == 12 && lastRemotePacketId > 1) {
            initPayloadSent = true;
            initPayloadSentAtId = lastRemotePacketId;
        }

        if (initPayloadSent && (flags & F_ACK_REQUEST) &&
            (hasInitialized || !(flags & F_RESEND))) {
            // Acknowledge every command packet once we're up.
            size_t out = buildAck(pkt, PKT_MAX, sessionId, lastRemotePacketId);
            sendPkt(out);
        } else if (initPayloadSent && (flags & F_REQUEST_NEXT) && hasInitialized) {
            // The ATEM lost one of our packets. We don't keep sent packets
            // around, so answer with an empty one (same as the old lib).
            size_t out = buildEmptyResend(pkt, PKT_MAX, sessionId,
                                          headerRequestNextId(pkt));
            sendPkt(out);
        }

        if (!(flags & F_HELLO) && packetLen > 12) forEachSegment(pkt, n, onSegment, nullptr);
    }

    // Recover any init packets lost during the state dump, then go live.
    if (!hasInitialized && initPayloadSent && !waitingForIncoming) {
        bool asked = false;
        for (uint8_t i = 1; i < initPayloadSentAtId && i <= MAX_INIT_PACKETS; i++) {
            if (missedInit[i >> 3] & (1 << (i & 7))) {
                size_t out = buildResendRequest(pkt, PKT_MAX, sessionId, i - 1);
                sendPkt(out);
                waitingForIncoming = true;
                asked = true;
                break;
            }
        }
        if (!asked) hasInitialized = true;
    }

    // Connection logging + watchdog.
    if (connectedFlag != wasConnected) {
        wasConnected = connectedFlag;
        Serial.println(connectedFlag ? "ATEM: connected" : "ATEM: disconnected");
    }
    if ((unsigned long)(lastContact + TIMEOUT_MS) <= millis()) {
        Serial.println("ATEM: timed out - reconnecting");
        atem_connect();
    }
}

void atem_setup()
{
    Serial.print("atem_setup IP:");
    Serial.println(config.ip.toString());
    swIp = config.ip;
    atem_connect();
}

void atem_switch_scene(uint8_t tallyNum, bool autoTransition)
{
    if (tallyNum == 0) return;
    if (!atem_isConnected()) {
        Serial.println("ATEM switch: not connected");
        return;
    }
    // M/E 0 in byte 0, input number as big-endian 16-bit video source.
    uint8_t p[4] = {0, 0, 0, tallyNum};
    if (autoTransition) {
        Serial.printf("ATEM auto -> %u\n", tallyNum);
        sendCommand("CPvI", p, sizeof(p));  // preview first...
        sendCommand("DAut", p, sizeof(p));  // ...then AUTO transition
    } else {
        Serial.printf("ATEM cut -> %u\n", tallyNum);
        sendCommand("CPgI", p, sizeof(p));  // straight to program
    }
}
