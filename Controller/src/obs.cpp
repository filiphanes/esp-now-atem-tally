#include <esp_log.h>
#include <esp32-hal-log.h>

#include "ArduinoJson.h"
#include "obs.h"

static const char *TAG = "websocket";

static void log_error_if_nonzero(const char *message, int error_code)
{
  if (error_code != 0) {
    ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
  }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
  switch (event_id) {
  case WEBSOCKET_EVENT_CONNECTED:
    ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
    break;
  case WEBSOCKET_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
    break;
  case WEBSOCKET_EVENT_DATA: {
    ESP_LOGI(TAG, "WEBSOCKET_EVENT_DATA");
    ESP_LOGI(TAG, "Received opcode=%d", data->op_code);
    if (data->op_code == 0x08 && data->data_len == 2) {
      ESP_LOGW(TAG, "Received closed message with code=%d", 256 * data->data_ptr[0] + data->data_ptr[1]);
    } else {
      ESP_LOGW(TAG, "Received=%.*s", data->data_len, (char *)data->data_ptr);
    }
    // If received data contains json structure it succeed to parse
    ESP_LOGW(TAG, "Total payload length=%d, data_len=%d, current payload offset=%d\r\n", data->payload_len, data->data_len, data->payload_offset);
    StaticJsonDocument<256> doc;
    auto error = deserializeJson(doc, data->data_ptr + data->payload_offset, data->payload_len);
    if (error) {
      Serial.print(F("deserializeJson() failed with code "));
      Serial.println(error.c_str());
      return;
    }
    if (doc["op"] == 5) {
      if (doc["d"]["eventType"] == "CurrentProgramSceneChanged") {
        doc["d"]["eventData"]["scenaName"];
      } else if (doc["d"]["eventType"] == "CurrentPreviewSceneChanged") {
        doc["d"]["eventData"]["scenaName"];
      }
    }
    break;
  }
  case WEBSOCKET_EVENT_ERROR:
    ESP_LOGI(TAG, "WEBSOCKET_EVENT_ERROR");
    break;
  }
}

void setupOBS() {
  if (false) {
    char uri[100] = "ws://";
    strcat(uri, config.obsIP.toString().c_str());
    const esp_websocket_client_config_t ws_cfg = {
      .uri = uri,
      .port = config.obsPort,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);
    esp_websocket_client_start(client);
  }
}

void obsLoop() {
}
