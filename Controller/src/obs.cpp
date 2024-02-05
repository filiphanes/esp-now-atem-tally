#include <esp_log.h>
#include <esp32-hal-log.h>

#include "ArduinoJson.h"
#include "espNow.h"
#include "obs.h"

#define TALLY_UPDATE_EACH 3000

static const char *TAG = "websocket";
esp_websocket_client_handle_t client;
long lastMessageTime = 0;
uint64_t programBits = 0;
uint64_t previewBits = 0;
uint64_t DSKbits = 0;

static void log_error_if_nonzero(const char *message, int error_code) {
  if (error_code != 0) {
    ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
  }
}

inline uint64_t bitn(uint8_t n) {
  return (uint64_t)1 << (n-1);
}

uint64_t bitsFromTags(const char* s) {
  uint64_t bits = 0;
  int len = strlen(s);
  bool boundary = true;
  uint8_t n = 0;

  for (int i=0; i<len; i++) {
    if (s[i] == 'T' && boundary) {
      n = 0;
      i++;
      for (; i<len; i++) {
        if (isdigit(s[i])) {
          n = n*10 + s[i]-'0';
        } else {
          break;
        }
      }
      if (n > 0) bits |= bitn(n);
    } 
    boundary = !isalnum(s[i]);
  }
  return bits;
}

void obs_request_current_scenes() {
  const char* op1 = "{\"op\":6,\"d\":{\"requestType\":\"GetCurrentProgramScene\",\"requestId\":\"a\",\"requestData\":{}}}";
  esp_websocket_client_send_text(client, op1, strlen(op1), portMAX_DELAY);
  const char* op2 = "{\"op\":6,\"d\":{\"requestType\":\"GetCurrentPreviewScene\",\"requestId\":\"a\",\"requestData\":{}}}";  
  esp_websocket_client_send_text(client, op2, strlen(op2), portMAX_DELAY);
}

void obs_message_handler(StaticJsonDocument<512> doc) {
  switch ((int)doc["op"]) {
  case 5: {
    if (doc["d"]["eventType"] == "CurrentProgramSceneChanged") {
      // https://github.com/obsproject/obs-websocket/blob/5.3.3/docs/generated/protocol.md#getsceneitemlist
      programBits = bitsFromTags(doc["d"]["eventData"]["sceneName"]);
      uint64_t program = programBits | DSKbits;
      broadcastTally(&programBits, &previewBits);
    }
    else if (doc["d"]["eventType"]  == "CurrentPreviewSceneChanged") {
      previewBits = bitsFromTags(doc["d"]["eventData"]["sceneName"]);
      broadcastTally(&programBits, &previewBits);
    }
    else if (doc["d"]["eventType"] == "SceneTransitionStarted") {
      programBits |= previewBits;
      broadcastTally(&programBits, &previewBits);
    }
    else if (doc["d"]["eventType"] == "CustomEvent"
            && doc["d"]["eventData"]["type"] == "tally") {
      uint64_t bits = 0;
      bits |= bitn(doc["d"]["eventData"]["to"].as<int>());
      broadcastSignal(doc["d"]["eventData"]["signal"].as<uint8_t>(), &bits);
    }
    else if (doc["d"]["eventType"] == "VendorEvent" && doc["d"]["eventData"]["vendorName"] == "downstream-keyer") {
      DSKbits = bitsFromTags(doc["d"]["eventData"]["eventData"]["new_scene"]);
      uint64_t program = programBits | DSKbits;
      broadcastTally(&program, &previewBits);
		}
    break;
  }
  case 7:
    if (doc["d"]["requestType"] == "GetCurrentProgramScene") {
      // https://github.com/obsproject/obs-websocket/blob/5.3.3/docs/generated/protocol.md#getsceneitemlist
      programBits = bitsFromTags(doc["d"]["responseData"]["currentProgramSceneName"]);
      programBits |= DSKbits;
      broadcastTally(&programBits, &previewBits);
    } else if (doc["d"]["requestType"] == "GetCurrentPreviewScene") {
      previewBits = bitsFromTags(doc["d"]["responseData"]["currentPreviewSceneName"]);
      broadcastTally(&programBits, &previewBits);
    }
    break;
  case 2:
    obs_request_current_scenes();
    break;
  case 0: {
    // Scenes=4
    // Inputs=8
    // Transitions=16
    // SceneItems=128
    // InputShowStateChanged=262144 == preview anywhere in ui
    // InputActiveStateChanged=131072 == program
    // =393216
    const char* op1 = "{\"op\":1,\"d\":{\"rpcVersion\":1,\"eventSubscriptions\":532}}";
    esp_websocket_client_send_text(client, op1, strlen(op1), portMAX_DELAY);
    break;
    }
  }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
  switch (event_id) {
  case WEBSOCKET_EVENT_DATA: {
    // Serial.printf("Received opcode=%d\n", data->op_code);
    if (data->op_code == 1) {
      Serial.printf("<%.*s\n", data->data_len, (char *)data->data_ptr);
      StaticJsonDocument<512> doc;  // list of scenes is larger than 512 bytes
      auto error = deserializeJson(doc, data->data_ptr + data->payload_offset, data->payload_len);
      if (error) {
        Serial.print(F("deserializeJson() failed with code "));
        Serial.println(error.c_str());
        return;
      }
      obs_message_handler(doc);
    } else {
      // Serial.printf("Received=%.*s\n", data->data_len, (char *)data->data_ptr);
    }
    break;
  }
  case WEBSOCKET_EVENT_ERROR:
    Serial.println("WS ERROR");
    break;
  case WEBSOCKET_EVENT_CONNECTED:
    Serial.println("WS CONNECTED");
    break;
  case WEBSOCKET_EVENT_DISCONNECTED:
    Serial.println("WS DISCONNECTED");
    break;
  }
}

void obs_setup() {
  char uri[64];
  sprintf(uri, "ws://%s", inet_ntoa(config.obsIP), config.obsPort);
  Serial.printf("obs_setup %s\n", uri);
  const esp_websocket_client_config_t ws_cfg = {
    .uri = uri,
    .port = config.obsPort,
  };
  client = esp_websocket_client_init(&ws_cfg);
  esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);
  esp_websocket_client_start(client);
}

void obs_loop() {
  if (millis() - lastMessageTime > TALLY_UPDATE_EACH) {
    broadcastTally(&programBits, &previewBits);
    lastMessageTime = millis();
    if (!esp_websocket_client_is_connected(client)) {
      esp_websocket_client_destroy(client);
      obs_setup();
    }
  }
}
