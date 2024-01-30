#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <stdint.h> 
#include <WiFi.h>
#include <WiFiAP.h>
#include <WiFiClient.h>

#define TALLY_COUNT 64
#define TALLY_UPDATE_EACH 60000 // 1 minute;
#define LED_PIN    8
#define LED_COUNT  25
#define BRIGHTNESS 10  // 0-255
#define CAMERA_ID_ADDRESS 0
#define DEBUG 1

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
unsigned long lastMessageReceived = -TALLY_UPDATE_EACH;
uint8_t camId = 0;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

enum enum_command : uint8_t {
  SET_TALLY = 1,
  GET_TALLY = 2,
  SWITCH_CAMID = 3,
  HEARTBEAT = 4,
  SET_CAMID = 5,
  SET_COLOR = 6,
  SET_BRIGHTNESS = 7,

  SIGNAL_CHANGE = 12,
  SIGNAL_LEFT = 13,
  SIGNAL_DOWN = 14,
  SIGNAL_UP = 15,
  SIGNAL_RIGHT = 16,
  SIGNAL_FOCUS = 17,
  SIGNAL_DEFOCUS = 18,
  SIGNAL_ZOOMIN = 19,
  SIGNAL_ZOOMOUT = 20,
  SIGNAL_ISOUP = 21,
  SIGNAL_ISODOWN = 22,
};

void colorBlink(uint8_t r, uint8_t g, uint8_t b, int wait) {
  int count = 3;
  uint16_t i;
  while (count--) {
    for(i=0; i<LED_COUNT; i++) strip.setPixelColor(i, r, g, b);
    strip.show();
    delay(wait);
    for(i=0; i<LED_COUNT; i++) strip.setPixelColor(i, 0, 0, 0);
    strip.show();
    delay(wait);
  }
}

// Fill strip pixels one after another with a color. Strip is NOT cleared
void colorWipe(uint8_t r, uint8_t g, uint8_t b, int wait) {
  for(int i=0; i<LED_COUNT; i++) { // For each pixel in strip...
    strip.setPixelColor(i, r, g, b);       // Set pixel's color (in RAM)
    strip.show();                          // Update strip to match
    delay(wait);                           // Pause for a moment
  }
}

void readCamId() {
  EEPROM.begin(512);
  EEPROM.get(CAMERA_ID_ADDRESS, camId);
  if (camId > TALLY_COUNT) camId = 2;
  EEPROM.commit();
  Serial.print("camId: ");
  Serial.println(camId);
}

void writeCamId() {
  EEPROM.begin(512);
  EEPROM.put(CAMERA_ID_ADDRESS, camId);
  EEPROM.commit();
}

// 5x5 matrices for digits 0 to 9
const unsigned digits5x5[] = {
  0,1,1,1,0,
  1,0,0,0,1,
  1,0,0,0,1,
  1,0,0,0,1,
  0,1,1,1,0,

  0,0,0,1,0,
  0,0,1,1,0,
  0,1,0,1,0,
  0,0,0,1,0,
  0,0,0,1,0,

  1,1,1,1,0,
  0,0,0,0,1,
  0,1,1,1,0,
  1,0,0,0,0,
  1,1,1,1,1,

  1,1,1,1,0,
  0,0,0,0,1,
  0,1,1,1,0,
  0,0,0,0,1,
  1,1,1,1,0,

  1,0,0,0,1,
  1,0,0,0,1,
  0,1,1,1,1,
  0,0,0,0,1,
  0,0,0,0,1,

  1,1,1,1,1,
  1,0,0,0,0,
  1,1,1,1,0,
  0,0,0,0,1,
  1,1,1,1,0,

  0,1,1,1,1,
  1,0,0,0,0,
  1,1,1,1,0,
  1,0,0,0,1,
  0,1,1,1,0,

  1,1,1,1,1,
  0,0,0,1,0,
  0,0,1,0,0,
  0,0,1,0,0,
  0,0,1,0,0,

  0,1,1,1,0,
  1,0,0,0,1,
  0,1,1,1,0,
  1,0,0,0,1,
  0,1,1,1,0,

  0,1,1,1,0,
  1,0,0,0,1,
  0,1,1,1,1,
  0,0,0,0,1,
  0,1,1,1,0,
  // CHANGE
  1,0,0,0,1,
  0,1,0,1,0,
  0,0,1,0,0,
  0,1,0,1,0,
  1,0,0,0,1,
  // Left
  0,0,1,0,0,
  0,1,0,0,0,
  1,1,1,1,1,
  0,1,0,0,0,
  0,0,1,0,0,
  // Down
  0,0,1,0,0,
  0,0,1,0,0,
  1,0,1,0,1,
  0,1,1,1,0,
  0,0,1,0,0,
  // Up
  0,0,1,0,0,
  0,1,1,1,0,
  1,0,1,0,1,
  0,0,1,0,0,
  0,0,1,0,0,
  // Right
  0,0,1,0,0,
  0,0,0,1,0,
  1,1,1,1,1,
  0,0,0,1,0,
  0,0,1,0,0,
  // Focus
  1,1,1,1,1,
  1,0,0,0,0,
  1,1,1,1,0,
  1,0,0,0,0,
  1,0,0,0,0,
  // DeFocus (Blur)
  1,1,1,1,0,
  1,0,0,0,1,
  1,1,1,1,0,
  1,0,0,0,1,
  1,1,1,1,0,
  // ZoomIn
  1,1,1,1,1,
  0,0,0,1,0,
  0,0,1,0,0,
  0,1,0,0,0,
  1,1,1,1,1,
  // ZoomOut
  1,1,1,1,1,
  0,1,0,0,0,
  0,0,1,0,0,
  0,0,0,1,0,
  1,1,1,1,1,
  // ISO+
  1,0,0,0,0,
  1,0,0,1,0,
  1,0,1,1,1,
  1,0,0,1,0,
  1,0,0,0,0,
  // ISO-
  1,0,0,0,0,
  1,0,0,0,0,
  1,0,1,1,1,
  1,0,0,0,0,
  1,0,0,0,0,
  // OK
  0,0,0,0,1,
  0,0,0,1,0,
  1,0,1,0,0,
  0,1,0,0,0,
  0,1,0,0,0,

  0
};

void displayDigit(uint8_t r, uint8_t g, uint8_t b, uint8_t digit) {
  uint8_t x = 0;
  for (int i = 0; i < 25; i++) {
    x = digits5x5[digit*25 + i];
    strip.setPixelColor(24-i, x*r, x*g, x*b);
  }
  strip.show();
}

// Display a digit in the 5x5 matrix using given color
void displayNumber(uint8_t r, uint8_t g, uint8_t b, uint8_t number) {
  displayDigit(r, g, b, number % 10);
  // Signify tens by number of first white pixels
  if (r+g+b > 0) {
    for(int i=0; i<LED_COUNT; i++) {
      if (number < 10) break;
      strip.setPixelColor(i, 255, 255, 255);
      number -= 10;
    }
  }
  strip.show();
}

void displaySignal(uint8_t signal) {
  displayDigit(0, 0, 255, signal - 2);
}

void fillColor(uint8_t r, uint8_t g, uint8_t b) {
  for (int i=0; i<25; i++) {
    strip.setPixelColor(i, r, g, b);
  }
  strip.show();
}

inline bool getBit(uint64_t bits, int i) {
  return bits & (1 << i);
}

// Callback function that will be executed when data is received
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *data, int len) {
  enum_command command = (enum_command)data[0];
  Serial.printf("<[%d] ");
  switch (command) {

  case SET_TALLY: {
    uint64_t *program_p = (uint64_t *)(data+1);
    uint64_t *preview_p = (uint64_t *)(data+1+sizeof(uint64_t));
    if      (getBit(*program_p, camId-1)) fillColor(255, 0, 0);
    else if (getBit(*preview_p, camId-1)) fillColor(0, 255, 0);
    else fillColor(0, 0, 0);
    lastMessageReceived = millis();
#ifdef DEBUG
    Serial.println("SET_TALLY");
    // for (int i=1; i<len; i++) Serial.printf("%02x ", data[i]);
    // Serial.println();
    Serial.print("Program ");
    for (int i=0; i<TALLY_COUNT; i++) Serial.print(getBit(*program_p, i)?1:0);
    Serial.println();
    Serial.print("Preview ");
    for (int i=0; i<TALLY_COUNT; i++) Serial.print(getBit(*preview_p, i)?1:0);
    Serial.println();
#endif
    break;
  }
    
  case HEARTBEAT:
    break;

  case SET_COLOR: {
    uint64_t *bits_p = (uint64_t *)(data+4);
    Serial.printf("SET_COLOR #%02x%02x%02x\n", data[2], data[3], data[4]);
    if (getBit(*bits_p, camId-1)) {
      fillColor(data[1], data[2], data[3]);
    }
    lastMessageReceived = millis();
    break;
  }
  
  case SIGNAL_CHANGE:
  case SIGNAL_FOCUS:
  case SIGNAL_DEFOCUS:
  case SIGNAL_ZOOMIN:
  case SIGNAL_ZOOMOUT:
  case SIGNAL_LEFT:
  case SIGNAL_DOWN:
  case SIGNAL_UP:
  case SIGNAL_RIGHT:
  case SIGNAL_ISOUP:
  case SIGNAL_ISODOWN: {
    uint64_t *bits_p = (uint64_t *)(data+1);
    Serial.printf("SIGNAL %u %lu\n", command, *bits_p);
    if (getBit(*bits_p, camId-1)) displaySignal(command);
    break;
  }

  case SET_BRIGHTNESS: {
    uint64_t *bits_p = (uint64_t *)(data+2);
    uint64_t brightness = data[1];
    if (getBit(*bits_p, camId-1)) {
      strip.setBrightness(brightness);
    }
    lastMessageReceived = millis();
    break;
  }

  case SET_CAMID: {
    uint64_t *bits_p = (uint64_t *)(data+2);
    if (getBit(*bits_p, camId-1)) {
      camId = data[1];
      writeCamId();
      displayNumber(0, 0, 255, camId);
      Serial.printf("SET_CAMID %d\n", camId);
      delay(1000);  // so new number is visible
    }
    /*
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (memcmp(mac, (uint8_t*) (data + sizeof(enum_command)), 6) == 0) {
      camId = data[7];
      Serial.printf("SET_CAMID %d\n", camId);
    }
    */
    lastMessageReceived = millis();
    break;
  }

  case SWITCH_CAMID: {
    uint8_t* id1 = (uint8_t*) (data + sizeof(enum_command));
    uint8_t* id2 = (uint8_t*) (data + sizeof(enum_command) + sizeof(uint8_t));
    Serial.printf("SWITCH_CAMID %d<>%d", id1, id2);
    if (camId == *id1) {
      camId = *id2;
      writeCamId();
    } else if (camId == *id2) {
      camId = *id1;
      writeCamId();
    }
    lastMessageReceived = millis();
    break;
  }

  case GET_TALLY:
    Serial.println("GET_TALLY");
    break;
  }
}

void sendHeartbeat() {
  uint8_t payload[2] = {HEARTBEAT, camId};
  esp_err_t err = esp_now_send(broadcastAddress, (uint8_t *)&payload, sizeof(payload));
  #ifdef DEBUG
  Serial.printf(">HEARTBEAT\n");
  #endif
  if (err) Serial.printf("esp_now_send returned 0x%x\n", err);
}

void setup() {
  Serial.begin(115200);
  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  readCamId();
  // strip.show();  // Turn OFF all pixels ASAP
  displayNumber(0, 0, 255, camId);
  delay(300);

  Serial.println("Starting ESP NOW");
  if (!WiFi.mode(WIFI_STA)) {
    Serial.println("WiFi.mode not successfull");
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init not OK");
    ESP.restart();
  }
  
  // Add Broadcast Peer
  esp_now_peer_info_t peer;
  peer.channel = 0;
  // peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  // memcpy(&peer.lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
  memcpy(peer.peer_addr, broadcastAddress, ESP_NOW_ETH_ALEN);
  if(esp_err_t err = esp_now_add_peer(&peer)) {
    Serial.printf("esp_now_add_peer returned 0x%x\n", err);
  }

  esp_now_register_recv_cb(OnDataRecv);
}

void loop() {
  sendHeartbeat();
  delay(10000);
  if (millis() - lastMessageReceived > 5000) {
    fillColor(0, 0, 0);
    strip.setPixelColor(millis()%LED_COUNT, 128, 0, 0);
    strip.show();
  }
}
