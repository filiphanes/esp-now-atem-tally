#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <esp_now.h>
#include <ESPAsyncWebServer.h>
#include <stdint.h> 
#include <WiFi.h>
#include <WiFiAP.h>
#include <WiFiClient.h>

#define TALLY_COUNT 64
#define TALLY_UPDATE_EACH 60000 // 1 minute;
#define LED_PIN    8
#define LED_COUNT  25
#define BRIGHTNESS 64  // 0-255
#define CAMERA_ID_ADDRESS 0
#define CONFIG_BUTTON 13 // = D7
#define DEBUG 1

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
boolean configMode = false;
long lastMessageReceived = -TALLY_UPDATE_EACH;
uint8_t camId = 0;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const String ssid = "tally";
const String password = "tally";

AsyncWebServer server(80);
DNSServer dnsServer;

IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

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

void configLedAnimation() {
  // wipe to blue
  colorWipe(  0,   0, 255, 5);
  // wipe to black
  colorWipe(  0,   0,   0, 5);
}

void readCamId() {
  EEPROM.begin(512);
  EEPROM.get(CAMERA_ID_ADDRESS, camId);
  if (camId > TALLY_COUNT) camId = 1;
  EEPROM.commit();
  Serial.print("camId: ");
  Serial.println(camId);
}

void writeCamId() {
  EEPROM.begin(512);
  EEPROM.put(CAMERA_ID_ADDRESS, camId);
  EEPROM.commit();
}

void handleRoot(AsyncWebServerRequest *request) {
  String message = "";
  if (request->method() == HTTP_POST) {
    String cameraId = request->getParam("camera-id", true)->value();
    camId = cameraId.toInt();
    if (camId > TALLY_COUNT) camId = TALLY_COUNT;
    writeCamId();
    Serial.print("camId=");
    Serial.println(camId);
    message = "Camera ID set to: " + cameraId + ". Restart device!";
  }
  request->send(200, "text/html", "<html><head>"
    "<title>ESP NOW Tally</title>"
    "<style>"
      "body {background-color: black; color: #e6e5df; margin-top: 1rem;}"
    "</style>"
    "<script>"
      "function post(u){var x=new XMLHttpRequest();x.open('post',u);x.send()}"
    "</script>"
    "</head>"
    "<body>"
    "<strong>"+message+"</strong>"
    "<form method=\"post\" enctype=\"application/x-www-form-urlencoded\">"
    "<label>Camera Id:</label><br>"
    "<input type=\"number\" name=\"camera-id\" value=\"" + String(camId) + "\"><br>"
    "<input type=\"submit\" value=\"Save\"><br>"
    "<button onclick=\"post('red')\" style='color: red;'>Blink RED</button>"
    "<button onclick=\"post('green')\" style='color: green;'>Blink GREEN</button>"
    "<button onclick=\"post('blue')\" style='color: blue;'>Blink BLUE</button>"
    "<button onclick=\"post('restart')\">Restart</button>"
    "</form></body></html>");
}

void handleRed(AsyncWebServerRequest *request) {
  request->send(200, "text/plain", "OK");
  colorBlink(255, 0, 0, 100);
}

void handleGreen(AsyncWebServerRequest *request) {
  request->send(200, "text/plain", "OK");
  colorBlink(0, 255, 0, 100);
}

void handleBlue(AsyncWebServerRequest *request) {
  request->send(200, "text/plain", "OK");
  colorBlink(0, 0, 255, 100);
}

void handleRestart(AsyncWebServerRequest *request){
  request->send(200, "text/plain", "OK");
  colorWipe(0, 0, 0, 10);
  delay(1);
  ESP.restart();
}

void notFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "Not found");
}

void setupWebserver() {
  WiFi.mode(WIFI_AP);
  Serial.print("Setting soft-AP configuration ... ");
  Serial.println(WiFi.softAPConfig(gateway, gateway, subnet) ? "Ready" : "Failed!");

  Serial.print("Setting soft-AP ... ");
  Serial.println(WiFi.softAP(ssid) ? "Ready" : "Failed!");

  /* Setup the DNS server redirecting all the domains to the apIP */
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());

  Serial.println("Connect to WiFi network with SSID: " + String(ssid) + " (" + String(camId) + ")" + " to configure the device.");

  server.on("/", handleRoot);
  server.on("/red", handleRed);
  server.on("/green", handleGreen);
  server.on("/blue", handleBlue);
  server.on("/restart", handleRestart);
  server.onNotFound(handleRoot);
  server.begin();
}

enum enum_command : uint8_t {
  SET_TALLY = 1,
  GET_TALLY = 2,
  SWITCH_CAMID = 3,
};

// Must match the sender structure
typedef struct struct_message {
  enum_command command;
  uint64_t program = 0;
  uint64_t preview = 0;
} struct_message;
struct_message messageData;

// 5x5 matrices for digits 0 to 9
const unsigned digits5x5[10*25] = {
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
  1,1,1,1,0,
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
  0,1,1,1,0
};

// Display a digit in the 5x5 matrix using given color
void displayNumber(uint8_t r, uint8_t g, uint8_t b, int number) {
  int digit = number % 10;
  uint8_t x = 0;
  int i = 0;
  for (i = 0; i < 25; i++) {
    x = digits5x5[digit*25 + i];
    strip.setPixelColor(i, x*r, x*g, x*b);
  }
  // Signify tens by number of first white pixels
  if (r+g+b > 0) {
    for(i=0; i<LED_COUNT; i++) {
      if (number < 10) break;
      strip.setPixelColor(i, 255, 255, 255);
      number -= 10;
    }
  }
  strip.show();
}

inline uint64_t setBit(uint64_t bits, int i) {
  return bits | (1 << i);
}

inline bool getBit(uint64_t bits, int i) {
  return bits & (1 << i);
}

// Callback function that will be executed when data is received
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
  struct_message *data = (struct_message *)incomingData;
  lastMessageReceived = millis();
  Serial.printf("Length: %d\n", len);
  Serial.print("Command: ");
  switch (data->command) {
  case SET_TALLY:
    Serial.println("SET_TALLY");
    // for (int i=1; i<len; i++) Serial.printf("%02x ", incomingData[i]);
    // Serial.println();
    Serial.print("Program ");
    for (int i=0; i<TALLY_COUNT; i++) Serial.print(getBit(data->program, i - 1)?1:0);
    Serial.println();
    Serial.print("Preview ");
    for (int i=0; i<TALLY_COUNT; i++) Serial.print(getBit(data->preview, i - 1)?1:0);
    Serial.println();
    displayNumber(getBit(data->program, camId - 1) ? 255 : 0,
                  getBit(data->preview, camId - 1) ? 255 : 0,
                  0,
                  camId);
    break;
  case GET_TALLY:
    // TODO: Relay tally info, when requester not in range
    Serial.println("GET_TALLY");
    break;
  case SWITCH_CAMID:
    uint8_t* id1 = (uint8_t*) incomingData + sizeof(enum_command);
    uint8_t* id2 = (uint8_t*) incomingData + sizeof(enum_command) + sizeof(uint8_t);
    Serial.printf("SWITCH_CAMID %d<>%d", id1, id2);
    if (camId == *id1) {
      camId = *id2;
      writeCamId();
    } else if (camId == *id2) {
      camId = *id1;
      writeCamId();
    }
    break;
  }
}

void requestState() {
  messageData.command = GET_TALLY;
  esp_err_t err = esp_now_send(broadcastAddress, (uint8_t *)&messageData, sizeof(enum_command));
  #ifdef DEBUG
  Serial.printf("Sent %d\n", messageData.command);
  #endif
  if (err) Serial.printf("esp_now_send returned 0x%x\n", err);
}

void setupEspNow() {
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

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Started");

  strip.begin();
  strip.show();  // Turn OFF all pixels ASAP
  strip.setBrightness(BRIGHTNESS);
  readCamId();

  /*
  pinMode(CONFIG_BUTTON, INPUT_PULLDOWN);
  if (digitalRead(CONFIG_BUTTON) == HIGH) {
    Serial.println("Config mode activated");
    configMode = true;
    // configLedAnimation();
    setupWebserver();
    return;
  }
  */

  setupEspNow();
}

void loop() {
  if (configMode) {
    dnsServer.processNextRequest();
    Serial.print(".");
    delay(10);
    return;
  }

  if ((millis() - lastMessageReceived) > TALLY_UPDATE_EACH) {
    requestState();
    lastMessageReceived = millis();
  }
  delay(500);
}
