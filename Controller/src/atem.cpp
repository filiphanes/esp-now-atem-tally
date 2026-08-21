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
// All other switcher state the full library tracks is ignored.

#include <Arduino.h>
#include <WiFiUdp.h>

#include "atem.h"
#include "espnow.h"
#include "main.h"

// ---- protocol constants (subset of ATEMbase.h) ----
static const uint16_t ATEM_PORT = 9910;
enum : uint8_t {
    HDR_ACK_REQUEST = 0x1,   // sender wants an ack for this packet id
    HDR_HELLO       = 0x2,   // handshake packet
    HDR_RESEND      = 0x4,   // this is a retransmission
    HDR_REQUEST_NEXT= 0x8,   // sender asks us to resend something
    HDR_ACK         = 0x10,  // acknowledges the packet id in bytes 4-5
};
static const uint8_t MAX_INIT_PACKETS = 40;     // init dump stays below this many packet ids
static const uint16_t PKT_MAX = 2048;           // rx buffer (>= one MTU datagram)
static const unsigned long TIMEOUT_MS = 5000;   // no traffic for this long => reconnect

// ---- connection state ----
static WiFiUDP udp;
static IPAddress swIp;
static uint16_t sessionId = 0x53AB;
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

// ---- tally state (bit i-1 == input i, like the old getProgramTally(i)) ----
static uint64_t progBits = 0;
static uint64_t prevBits = 0;

uint64_t getProgramBits() { return progBits; }
uint64_t getPreviewBits() { return prevBits; }
bool atem_isConnected() { return connectedFlag; }

// ---------------------------------------------------------------------------
// Packet building / sending
// ---------------------------------------------------------------------------

// Fill the 12-byte UDP header. countLocal: only true for AckRequest command
// packets (acks/hellos/resend requests never consume a local packet id).
static void makeHeader(uint8_t flags, uint16_t len, uint16_t remoteId, bool countLocal)
{
    pkt[0] = (flags << 3) | ((len >> 8) & 0x07);
    pkt[1] = len & 0xFF;
    pkt[2] = sessionId >> 8;        // session id given back by the ATEM
    pkt[3] = sessionId & 0xFF;
    pkt[4] = remoteId >> 8;         // remote packet id being acked
    pkt[5] = remoteId & 0xFF;
    if (countLocal) {
        localPacketId++;
        pkt[10] = localPacketId >> 8;
        pkt[11] = localPacketId & 0xFF;
    } else {
        pkt[10] = 0;
        pkt[11] = 0;
    }
}

static void sendPkt(uint16_t len)
{
    udp.beginPacket(swIp, ATEM_PORT);
    udp.write(pkt, len);
    udp.endPacket();
}

// Send one command segment ("CPgI", "CPvI", "DAut", ...) with raw payload.
static void sendCommand(const char *cmd4, const uint8_t *payload, uint8_t payloadLen)
{
    if (!connectedFlag) return;
    uint16_t segLen = 4 + 4 + payloadLen;   // length field + cmd string + payload
    uint16_t total = 12 + segLen;
    memset(pkt, 0, total);
    makeHeader(HDR_ACK_REQUEST, total, 0, true);
    pkt[12] = segLen >> 8;
    pkt[13] = segLen & 0xFF;
    memcpy(&pkt[16], cmd4, 4);
    if (payload && payloadLen) memcpy(&pkt[20], payload, payloadLen);
    sendPkt(total);
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
    sessionId = 0x53AB;                 // provisional, replaced by the switcher's
    initPayloadSentAtId = MAX_INIT_PACKETS;
    lastContact = millis();             // counts as an attempt
    memset(missedInit, 0xFF, sizeof(missedInit));

    udp.stop();
    int port = random(50100, 65300);
    if (!udp.begin(port)) {
        Serial.println("ATEM: UDP bind failed");
        return;
    }

    // 20-byte hello packet announcing us to the switcher.
    memset(pkt, 0, sizeof(pkt));
    makeHeader(HDR_HELLO, 12 + 8, 0, false);
    pkt[9] = 0x3a;
    pkt[12] = 0x01;
    sendPkt(20);
}

// ---------------------------------------------------------------------------
// Receive path
// ---------------------------------------------------------------------------

static void handleTally(const uint8_t *p, uint16_t len)
{
    if (len < 2) return;
    uint16_t sources = word(p[0], p[1]);
    if (sources > TALLY_COUNT) sources = TALLY_COUNT;
    if (len < 2u + sources) sources = len - 2;

    uint64_t pgm = 0, pvw = 0;
    for (uint16_t i = 0; i < sources; i++) {
        if (p[2 + i] & 0x01) pgm |= (uint64_t)1 << i;   // bit 0: program
        if (p[2 + i] & 0x02) pvw |= (uint64_t)1 << i;   // bit 1: preview
    }
    progBits = pgm;
    prevBits = pvw;
    espnow_tally(&progBits, &prevBits);     // same trigger point as the old lib callback
}

// Walk the command segments packed behind the 12-byte header and dispatch the
// ones we care about. Segment layout: [len:2][?:2][cmd:4][payload...].
static void parseSegments(uint16_t n)
{
    uint32_t off = 12;
    while (off + 8 <= n) {
        uint16_t cmdLen = word(pkt[off], pkt[off + 1]);
        if (cmdLen < 8 || off + cmdLen > n) break;      // malformed -> stop
        const char *cmd = (const char *)&pkt[off + 4];
        if (cmdLen > 8 && strncmp(cmd, "TlIn", 4) == 0)
            handleTally(&pkt[off + 8], cmdLen - 8);
        off += cmdLen;                                  // everything else: skip
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

        sessionId = word(pkt[2], pkt[3]);
        uint8_t flags = pkt[0] >> 3;
        lastRemotePacketId = word(pkt[10], pkt[11]);
        uint16_t packetLen = word(pkt[0] & 0x07, pkt[1]);

        if (lastRemotePacketId < MAX_INIT_PACKETS)
            missedInit[lastRemotePacketId >> 3] &= ~(1 << (lastRemotePacketId & 7));

        if ((uint16_t)size != packetLen) continue;      // header lies -> drop it

        lastContact = millis();
        waitingForIncoming = false;

        if (flags & HDR_HELLO) {
            connectedFlag = true;
            memset(pkt, 0, 12);
            makeHeader(HDR_ACK, 12, 0, false);
            pkt[9] = 0x03;
            sendPkt(12);
        }

        // A 12-byte-only packet marks the end of the initial state dump.
        if (!initPayloadSent && n == 12 && lastRemotePacketId > 1) {
            initPayloadSent = true;
            initPayloadSentAtId = lastRemotePacketId;
        }

        if (initPayloadSent && (flags & HDR_ACK_REQUEST) &&
            (hasInitialized || !(flags & HDR_RESEND))) {
            // Acknowledge every command packet once we're up.
            memset(pkt, 0, 12);
            makeHeader(HDR_ACK, 12, lastRemotePacketId, false);
            sendPkt(12);
        } else if (initPayloadSent && (flags & HDR_REQUEST_NEXT) && hasInitialized) {
            // The ATEM lost one of our packets. We don't keep sent packets
            // around, so answer with an empty one (same as the old lib).
            uint16_t wanted = word(pkt[6], pkt[7]);
            memset(pkt, 0, 12);
            makeHeader(HDR_ACK_REQUEST, 12, 0, false);
            pkt[10] = wanted >> 8;
            pkt[11] = wanted & 0xFF;
            sendPkt(12);
        }

        if (!(flags & HDR_HELLO) && packetLen > 12) parseSegments(n);
    }

    // Recover any init packets lost during the state dump, then go live.
    if (!hasInitialized && initPayloadSent && !waitingForIncoming) {
        bool asked = false;
        for (uint8_t i = 1; i < initPayloadSentAtId && i <= MAX_INIT_PACKETS; i++) {
            if (missedInit[i >> 3] & (1 << (i & 7))) {
                memset(pkt, 0, 12);
                makeHeader(HDR_REQUEST_NEXT, 12, 0, false);
                pkt[6] = (i - 1) >> 8;      // "resend everything after id i-1"
                pkt[7] = (i - 1) & 0xFF;
                pkt[8] = 0x01;
                sendPkt(12);
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
