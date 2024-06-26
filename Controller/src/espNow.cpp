#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <WiFi.h>

#include "atem.h"
#include "espnow.h"

#define MAX_TALLY_COUNT 64

// Broadcast address, sends to all devices nearby
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
esp_now_peer_info_t peerInfo;
espnow_tally_info_t tallies[MAX_TALLY_COUNT];
uint64_t programBits = 0;
uint64_t previewBits = 0;
long lastMessageAt = -10000;

espnow_tally_info_t * espnow_tallies() {
  return tallies;
}

void espnow_tally() {
  espnow_tally(&programBits, &previewBits);
}

void espnow_tally(uint64_t *program, uint64_t *preview) {
  uint8_t payload[1+sizeof(uint64_t)+sizeof(uint64_t)];
  payload[0] = SET_TALLY;
  memcpy(payload+1, program, sizeof(uint64_t));
  memcpy(payload+1+sizeof(uint64_t), preview, sizeof(uint64_t));
  esp_err_t result = esp_now_send(broadcastAddress, payload, sizeof(payload));
  programBits = *program;
  previewBits = *preview;
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

void switchCamId(uint8_t id1, uint8_t id2) {
  uint8_t payload[3] = {SWITCH_CAMID, id1, id2};
  esp_err_t result = esp_now_send(broadcastAddress, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

void espnow_brightness(uint8_t brightness, uint64_t *bits) {
  uint8_t payload[2+sizeof(uint64_t)];
  payload[0] = SET_BRIGHTNESS;
  payload[1] = brightness;
  memcpy(payload+2, bits, sizeof(*bits));
  if (!esp_now_send(broadcastAddress, payload, sizeof(payload))) {
    Serial.println("esp_now_send != OK");
  }
}

void espnow_camid(uint8_t camId, uint64_t *bits) {
  uint8_t payload[2+sizeof(uint64_t)];
  payload[0] = SET_CAMID;
  payload[1] = camId;
  memcpy(payload+2, bits, sizeof(*bits));
  esp_err_t result = esp_now_send(broadcastAddress, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

void espnow_color(uint32_t color, uint64_t *bits) {
  // "/rgba\0\0\0,ir\0{tallyid}{color}"
  uint8_t payload[4+sizeof(uint64_t)];
  payload[0] = SET_COLOR;
  payload[1] = (color >> 16) & 0xFF;
  payload[2] = (color >> 8) & 0xFF;
  payload[3] = color & 0xFF;
  memcpy(payload+4, bits, sizeof(*bits));
  esp_err_t result = esp_now_send(broadcastAddress, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

void espnow_signal(uint8_t signal, uint64_t *bits) {
  // "/signal\0,ii\0{tallyid}{signal}"
  uint8_t payload[1+sizeof(uint64_t)];
  payload[0] = signal;  // Signal number is command number
  memcpy(payload+1, bits, sizeof(*bits));
  esp_err_t result = esp_now_send(broadcastAddress, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

// callback when data is sent
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
  // Serial.print("\r\nLast Packet Send Status:\t");
  // Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}

// callback when data is received
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *data, int len)
{
  espnow_command command = (espnow_command) data[0];
  // Serial.printf("Command[%d]: ", len);
  switch (command)
  {
  case HEARTBEAT: {
    Serial.printf("HEARTBEAT %d\n", data[8]);
    for (int i=0; i<MAX_TALLY_COUNT; i++) {
      if (memcmp(tallies[i].mac_addr, mac_addr, 6) == 0) {
        // Found
        tallies[i].id = data[1];
        tallies[i].last_seen = millis();
        break;
      } else if (tallies[i].id == 0) {
        // Add to end of list
        memcpy(tallies[i].mac_addr, mac_addr, 6);
        tallies[i].id = data[1];
        tallies[i].last_seen = millis();
        break;
      }
    }
    break;
  }
  
  case GET_TALLY:
    Serial.println("GET_TALLY");
    espnow_tally();
    break;
  
  default:
    break;
  }
}

void espnow_setup()
{
  Serial.println("SetupEspNow");
  WiFi.mode(WIFI_STA);
  // config long range mode
  int a = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
  Serial.println(a);
  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init != OK");
    return;
  }

  // Once ESPNow is successfully Init, we will register for Send CB to
  // get the status of Transmitted packet and register peer data receive
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  // Register peer
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  // Add peer
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }
  
  // Zero tallies array
  for (int i=0; i<MAX_TALLY_COUNT; i++) {
    tallies[i].id = 0;
  }
}

void espnow_loop() {
  if (millis() - lastMessageAt > TALLY_UPDATE_EACH) {
    espnow_tally();
    lastMessageAt = millis();
  }
}