#include <Arduino.h>
#include <WiFiServer.h>
#include <vector>
#include "vmixServer.h"
#include "espnow.h"

WiFiServer vmixServer(8099);

struct VmixClient {
    WiFiClient socket;
    bool subscribed;
};

std::vector<VmixClient*> subscribers;

uint64_t lastProgramBits = 0;
uint64_t lastPreviewBits = 0;

String getTallyString() {
    char buf[65];
    for (int i = 0; i < 64; i++) {
        bool pgm = (programBits & (1ULL << i)) != 0;
        bool pvw = (previewBits & (1ULL << i)) != 0;
        buf[i] = pgm ? (pvw ? '3' : '2') : (pvw ? '1' : '0');
    }
    buf[64] = '\0';
    return String(buf);
}

void sendTallyUpdate() {
    if (programBits == lastProgramBits && previewBits == lastPreviewBits) {
        return;
    }
    lastProgramBits = programBits;
    lastPreviewBits = previewBits;
    String ts = getTallyString();
    String msg = "TALLY OK " + ts + "\r\n";
    for (auto clp : subscribers) {
        VmixClient* cl = clp;
        if (cl->subscribed && cl->socket.connected()) {
            cl->socket.print(msg);
        }
    }
}

void processVmixCommand(String cmd, VmixClient* cl) {
    cmd.trim();
    if (cmd == "TALLY") {
        String ts = getTallyString();
        cl->socket.print("TALLY OK ");
        cl->socket.print(ts);
    } else if (cmd == "SUBSCRIBE TALLY") {
        cl->subscribed = true;
        cl->socket.print("SUBSCRIBE OK TALLY");
        String ts = getTallyString();
        cl->socket.print("TALLY OK ");
        cl->socket.print(ts);
    } else if (cmd == "UNSUBSCRIBE TALLY") {
        cl->subscribed = false;
        cl->socket.print("UNSUBSCRIBE OK TALLY");
    } else {
        cl->socket.print("ER");
    }
    cl->socket.print("\r\n");
}

void vmixServerSetup() {
    vmixServer.begin();
    Serial.println("vMix TCP Tally server started on port 8099");
}

void vmixServerLoop() {
    unsigned long now = millis();
    // Check for new clients
    WiFiClient newSock = vmixServer.available();
    if (newSock) {
        VmixClient* newCl = new VmixClient{newSock, false};
        subscribers.push_back(newCl);
        Serial.println("New vMix client connected");
    }
    // Handle existing clients
    for (auto it = subscribers.begin(); it != subscribers.end(); ) {
        VmixClient* cl = *it;
        if (!cl->socket.connected()) {
            Serial.println("vMix client disconnected");
            delete cl;
            it = subscribers.erase(it);
        } else {
            while (cl->socket.available() > 0) {
                String line = cl->socket.readStringUntil('\n');
                line.trim();
                if (line.length() > 0) {
                    Serial.printf("vMix cmd: %s\n", line.c_str());
                    processVmixCommand(line, cl);
                }
            }
            ++it;
        }
    }
}