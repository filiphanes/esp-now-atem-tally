#include <Arduino.h>
#include <WiFiServer.h>
#include <vector>
#include "espnow.h"

WiFiServer vmixServer(8099);

struct VmixClient {
    WiFiClient socket;
    bool subscribed;
};

std::vector<VmixClient*> subscribers;
String lastTallyResponse = "TALLY OK 0\r\n";

String vmix_tally_string(uint64_t *program, uint64_t *preview) {
    char buf[65];
    for (int i = 0; i < 64; i++) {
        bool pgm = (*program & (1ULL << i)) != 0;
        bool pvw = (*preview & (1ULL << i)) != 0;
        buf[i] = pgm ? '1' : (pvw ? '2' : '0');
    }
    buf[64] = '\0';
    return String(buf);
}

void vmix_tally(uint64_t *program, uint64_t *preview) {
    String ts = vmix_tally_string(program, preview);
    lastTallyResponse = "TALLY OK " + ts + "\r\n";
    for (auto clp : subscribers) {
        VmixClient* cl = clp;
        if (cl->subscribed && cl->socket.connected()) {
            cl->socket.print(lastTallyResponse);
        }
    }
}

void processVmixCommand(String cmd, VmixClient* cl) {
    cmd.trim();
    if (cmd == "TALLY") {
        cl->socket.print(lastTallyResponse);
    } else if (cmd == "SUBSCRIBE TALLY") {
        cl->subscribed = true;
        cl->socket.print("SUBSCRIBE OK TALLY\r\n");
        cl->socket.print(lastTallyResponse);
    } else if (cmd == "UNSUBSCRIBE TALLY") {
        cl->subscribed = false;
        cl->socket.print("UNSUBSCRIBE OK TALLY\r\n");
    } else {
        cl->socket.print("ER\r\n");
    }
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