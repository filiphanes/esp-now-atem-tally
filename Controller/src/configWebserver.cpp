#include "configWebserver.h"

WebServer web(80);
WebSocketsServer wsServer(81);

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_CONNECTED) {
    Serial.printf("WS client %d connected\n", num);
    ws_tally();
  } else if (type == WStype_DISCONNECTED) {
    Serial.printf("WS client %d disconnected\n", num);
  }
}

void ws_tally() {
  if (!programBits && !previewBits) return;
  String msg = "TALLY ";
  uint64_t b1 = programBits;
  uint64_t b2 = previewBits;
  while (b1 || b2) {
    char c = '0';
    if (b1 & 1) c += 1;
    if (b2 & 1) c += 2;
    msg += c;
    b1 >>= 1;
    b2 >>= 1;
  }
  wsServer.broadcastTXT(msg);
}

void broadcastSet(String key, String value, String csv) {
  String msg = key + " " + value + " " + csv;
  wsServer.broadcastTXT(msg);
}

void handleRoot() {
  extern const uint8_t data_index_html_start[] asm("_binary_data_index_html_start");
  web.send(200, "text/html", (const char*)data_index_html_start);
}

inline uint64_t bitn(uint8_t n) { return (uint64_t)1 << (n-1); }

uint64_t bitsFromCSV(String s) {
  uint64_t bits = 0;
  int num = 0;
  for (size_t i = 0; i < s.length(); ++i) {
    if (isdigit(s[i])) num = 10*num + s[i] - '0';
    else if (num > 0) { bits |= bitn(num); num = 0; }
  }
  if (num > 0) bits |= bitn(num);
  return bits;
}

uint32_t parseHexColor(String s) {
  uint32_t c = 0;
  for (int i=0; i < s.length(); i++) {
    unsigned char ch = s[i];
    c = c << 4;
    if (ch >= '0' && ch <= '9') c += ch - '0';
    else if (ch >= 'A' && ch <= 'F') c += ch - 'A' + 10;
    else if (ch >= 'a' && ch <= 'f') c += ch - 'a' + 10;
  }
  return c;
}

void handleSet() {
  String i = web.arg("i");
  uint64_t bits = bitsFromCSV(i);
  if (bits == 0) {
    web.send(400, "text/plain", "Missing i");
    return;
  }

  if (web.hasArg("camid")) {
    String v = web.arg("camid");
    espnow_camid(v.toInt(), &bits);
    broadcastSet("CAMID", v, i);
  } else if (web.hasArg("brightness")) {
    String v = web.arg("brightness");
    espnow_brightness(v.toInt(), &bits);
    broadcastSet("BRIGHTNESS", v, i);
  } else if (web.hasArg("color")) {
    String v = web.arg("color");
    uint32_t c = parseHexColor(v);
    espnow_color(c, &bits);
    broadcastSet("COLOR", v, i);
  } else if (web.hasArg("signal")) {
    String v = web.arg("signal");
    espnow_signal(v.toInt(), &bits);
    broadcastSet("SIGNAL", v, i);
  }

  web.send(200, "text/plain", "OK");
}

void handleConfig() {
  if (web.method() == HTTP_PUT) {
    String body = web.arg(0);
    body.trim();
    Serial.println("PUT body: [" + body + "]");
    if (body.length() > 0) {
      writeConfig(body);
    }
    web.send(200, "text/plain", "OK");
    delay(10);
    ESP.restart();
  } else if (web.method() == HTTP_GET) {
    web.send(200, "text/plain", readConfig());
  }
}

void handleSeen() {
  String s = "{\"tallies\":[";
  espnow_tally_info_t *t = espnow_tallies();
  unsigned long now = millis();
  for (int i=1; i<64; i++) {
    if (t[i].id == 0) break;
    s += "{\"id\":" + String(t[i].id) + ", \"seen\":" + String((now - t[i].last_seen)/1000) + "},";
  }
  if (s.endsWith(",")) s.remove(s.length()-1);
  s += "]}";
  web.send(200, "application/json", s);
}

void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("ETH Started");
      ETH.setHostname("tally");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("ETH Connected");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.print("ETH MAC: ");
      Serial.print(ETH.macAddress());
      Serial.print(", IPv4: ");
      Serial.print(ETH.localIP());
      if (ETH.fullDuplex()) Serial.print(", FULL_DUPLEX");
      Serial.print(", ");
      Serial.print(ETH.linkSpeed());
      Serial.println("Mbps");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("ETH Disconnected");
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("ETH Stopped");
      break;
    default: break;
  }
}

void setupWebserver() {
  Serial.print("Starting HTTP server on ");
  Serial.print(ARDUINO_BOARD);
  #ifdef WEBSERVER_WT32_ETH01_VERSION
  Serial.print(" with ");
  Serial.println(SHIELD_TYPE);
  Serial.println(WEBSERVER_WT32_ETH01_VERSION);
  WT32_ETH01_onEvent();
  ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER);
  WT32_ETH01_waitForConnect();
  #else
  WiFi.onEvent(WiFiEvent);
  ETH.begin();
  #endif

  web.on("/set", handleSet);
  web.on("/seen", handleSeen);
  web.on("/config", handleConfig);

  extern const uint8_t data_tally_html_start[] asm("_binary_data_tally_html_start");
  web.on("/tally", [](){
    web.send(200, "text/html", (const char*)data_tally_html_start);
  });

  extern const uint8_t data_director_html_start[] asm("_binary_data_director_html_start");
  web.on("/director", [](){
    web.send(200, "text/html", (const char*)data_director_html_start);
  });

  extern const uint8_t data_index_html_start[] asm("_binary_data_index_html_start");
  web.on("/", [](){
    web.send(200, "text/html", (const char*)data_index_html_start);
  });
  web.on("/tally.json", []() {
      web.send(200, "application/json", "{\"program\":"+String(programBits)+",\"preview\":"+String(previewBits)+"}");
  });


  web.begin();
  wsServer.begin();
  wsServer.onEvent(webSocketEvent);
  Serial.printf("WebSocket server started on port 81\n");
}

void webserverLoop() {
  web.handleClient();
  wsServer.loop();
}
