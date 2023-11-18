#include "espNow.h"

#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <WiFi.h>

#include "constants.h"
#include "main.h"

#define MAX_TALLY_COUNT 64

// Broadcast address, sends to all devices nearby
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
esp_now_peer_info_t peerInfo;
esp_now_tally_info_t tallies[MAX_TALLY_COUNT];

esp_now_tally_info_t * get_tallies() {
  return tallies;
}

void broadcastTally()
{
  uint64_t program = getProgramBits();
  uint64_t preview = getPreviewBits();
  broadcastTally(&program, &preview);
}

void broadcastTally(uint64_t *program, uint64_t *preview) {
  uint8_t payload[1+sizeof(uint64_t)+sizeof(uint64_t)];
  payload[0] = SET_TALLY;
  memcpy(payload+1, program, sizeof(uint64_t));
  memcpy(payload+1+sizeof(uint64_t), preview, sizeof(uint64_t));
  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

void broadcastTest(int pgm, int pvw) {
  uint64_t program = 0;
  uint64_t preview = 0;
  program = program | (1 << pgm-1);
  preview = preview | (1 << pvw-1);
  broadcastTally(&program, &preview);
}

void switchCamId(uint8_t id1, uint8_t id2) {
  uint8_t payload[3] = {SWITCH_CAMID, id1, id2};
  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

void broadcastBrightness(uint8_t id, uint8_t brightness) {
  uint8_t payload[3] {SET_BRIGHTNESS, id, brightness};
  if (!esp_now_send(broadcastAddress, (uint8_t *)&payload, sizeof(payload))) {
    Serial.println("esp_now_send != OK");
  }
}

void broadcastColor(uint8_t r, uint8_t g, uint8_t b, uint64_t *bits) {
  uint8_t payload[4+sizeof(uint64_t)];
  payload[0] = SET_COLOR;
  payload[1] = r;
  payload[2] = g;
  payload[3] = b;
  memcpy(payload+4, bits, sizeof(bits));
  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&payload, sizeof(payload));
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
  enum_command command = (enum_command) data[0];
  Serial.printf("Command[%d]: ", len);
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
    broadcastTally();
    break;
  
  default:
    break;
  }
}

void setupEspNow()
{
  Serial.println("Init ESP-NOW");
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