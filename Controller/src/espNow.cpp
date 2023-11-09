#include "espNow.h"

#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <WiFi.h>

#include "atemConnection.h"
#include "constants.h"

// Broadcast address, sends to all devices nearby
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
struct_message message;
esp_now_peer_info_t peerInfo;

inline uint64_t setBit(uint64_t bits, int i) {
  return bits | (1 << i);
}

void broadcastTally()
{
  message.command = SET_TALLY;
  message.program = getProgramBits();
  message.preview = getPreviewBits();
  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&message, sizeof(message));
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

void broadcastTest(int pgm, int pvw)
{
  message.command = SET_TALLY;
  message.program = 0;
  message.preview = 0;
  message.program = setBit(message.preview, pgm-1);
  message.preview = setBit(message.preview, pvw-1);
  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&message, sizeof(message));
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

void switchCamId(uint8_t id1, uint8_t id2)
{
  uint8_t msg[3] = {SWITCH_CAMID, id1, id2};
  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&msg, sizeof(msg));
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

// callback when data is sent
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
  // Serial.print("\r\nLast Packet Send Status:\t");
  // Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}

// callback when data is received
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len)
{
  struct_message *data = (struct_message *)incomingData;
  Serial.print("< ");
  Serial.println(data->command);

  if (data->command == GET_TALLY)
  {
    broadcastTally();
  }
}

void setupEspNow()
{
  Serial.println("Init ESP-NOW ...");
  WiFi.mode(WIFI_STA);
  // config long range mode
  int a = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
  Serial.println(a);

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK)
  {
    Serial.println("Error initializing ESP-NOW");
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
  if (esp_now_add_peer(&peerInfo) != ESP_OK)
  {
    Serial.println("Failed to add peer");
    return;
  }
}