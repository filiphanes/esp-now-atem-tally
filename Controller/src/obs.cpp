#include "ArduinoJson.h"
#include "espnow.h"
#include "obs.h"

#define MULTILINE(...) #__VA_ARGS__

static const char *TAG = "websocket";
uint64_t DSKbits = 0;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
// Arduino core 3.x (pioarduino, IDF 5.x) no longer ships the IDF
// esp_websocket_client component with the framework's prebuilt libraries,
// so the OBS connection uses the WebSockets library client -- the same
// Links2004/WebSockets dependency that already serves the tally UI socket.
WebSocketsClient wsClient;
static bool obs_ws_connected = false;
static bool obs_ws_begun = false;
// QWebSocket (OBS 30) fragments large text messages into continuation
// frames; reassemble them here before JSON parsing. Whole messages arrive
// as a single WStype_TEXT event.
static String wsTextMsg;
static bool wsTextTooBig = false;
static const size_t WS_TEXT_MAX = 64 * 1024;
#else
esp_websocket_client_handle_t client;
#endif

struct ObsPendingSwitch {
  bool active;
  bool autoTransition;
  uint8_t tallyNum;
};
ObsPendingSwitch pendingSwitch = {false, false, 0};

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

// Send one text frame to the OBS server. On core 2 the IDF client takes an
// explicit bounded timeout so a stalled OBS cannot freeze the main loop
// forever; on core 3 the WebSockets library client has no timeout parameter,
// but it queues internally and only ever blocks on the TCP socket itself.
static void obs_send_text(const char *buf, size_t len, TickType_t timeout = portMAX_DELAY) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)timeout;
  wsClient.sendTXT(buf, len);
#else
  esp_websocket_client_send_text(client, buf, len, timeout);
#endif
}

void obs_broadcast_signal(uint64_t bits, uint8_t signal) {
  for (int i = 0; i < 64; i++) {
    if (bits & bitn(i)) {
      char op[192];
      int n = snprintf(op, sizeof(op),
        "{\"op\":6,\"d\":{\"requestType\":\"BroadcastCustomEvent\",\"requestId\":\"b\","
        "\"requestData\":{\"eventData\":{\"type\":\"tally\",\"from\":0,\"to\":%d,\"signal\":%u}}}}",
        i, signal);
      if (n > 0 && n < (int)sizeof(op)) {
        // Bounded timeout: a stalled OBS must not freeze the main loop
        // forever; the websocket client buffers and reconnects on its own.
        obs_send_text(op, n, pdMS_TO_TICKS(250));
      } else {
        Serial.printf("OBS: signal json truncated (%d)\n", n);
      }
    }
  }
}

void obs_request_current_scenes() {
  const char* op1 = "{\"op\":6,\"d\":{\"requestType\":\"GetCurrentProgramScene\",\"requestId\":\"a\",\"requestData\":{}}}";
  obs_send_text(op1, strlen(op1));
  const char* op2 = "{\"op\":6,\"d\":{\"requestType\":\"GetCurrentPreviewScene\",\"requestId\":\"a\",\"requestData\":{}}}";
  obs_send_text(op2, strlen(op2));
}

static bool jsonContains(const char* hay, size_t len, const char* needle) {
  size_t nl = strlen(needle);
  if (nl == 0 || len < nl) return false;
  for (size_t i = 0; i + nl <= len; i++) {
    if (memcmp(hay + i, needle, nl) == 0) return true;
  }
  return false;
}

// GetSceneList responses are large; filter to sceneName so it fits in a small doc.
void obs_handle_scene_list(const char* json, size_t len) {
  if (!pendingSwitch.active) return;

  StaticJsonDocument<128> filter;
  filter["d"]["responseData"]["scenes"][0]["sceneName"] = true;

  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, json, len, DeserializationOption::Filter(filter));
  if (error) {
    Serial.printf("Scene list parse failed: %s\n", error.c_str());
    return;
  }

  uint8_t target = pendingSwitch.tallyNum;
  bool autoT = pendingSwitch.autoTransition;
  pendingSwitch.active = false;

  uint64_t targetBit = bitn(target);
  const char* foundName = nullptr;
  JsonArray scenes = doc["d"]["responseData"]["scenes"];
  for (JsonObject scene : scenes) {
    const char* name = scene["sceneName"];
    if (name && (bitsFromTags(name) & targetBit)) {
      foundName = name;
      break;
    }
  }
  if (!foundName) {
    Serial.printf("No scene tagged T%u found\n", target);
    return;
  }

  Serial.printf("OBS %s -> '%s'\n", autoT ? "auto" : "cut", foundName);

  // Build the request with ArduinoJson so scene names containing quotes,
  // backslashes or other JSON-significant characters are escaped safely.
  const char* requestType = autoT ? "SetCurrentPreviewScene" : "SetCurrentProgramScene";
  StaticJsonDocument<512> req;
  req["op"] = 6;
  JsonObject d = req.createNestedObject("d");
  d["requestType"] = requestType;
  d["requestId"] = "p";
  d.createNestedObject("requestData")["sceneName"] = foundName;

  char out[512];
  size_t outLen = serializeJson(req, out, sizeof(out));
  if (outLen == 0 || outLen >= sizeof(out)) {
    Serial.println("OBS switch: scene name too long, request truncated");
    return;
  }
  obs_send_text(out, outLen);

  if (autoT) {
    const char* trig = "{\"op\":6,\"d\":{\"requestType\":\"TriggerTransition\",\"requestId\":\"t\",\"requestData\":{}}}";
    obs_send_text(trig, strlen(trig));
  }
}

void obs_switch_scene(uint8_t tallyNum, bool autoTransition) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!obs_ws_connected) {
    Serial.println("OBS switch: not connected");
    return;
  }
#else
  if (!client || !esp_websocket_client_is_connected(client)) {
    Serial.println("OBS switch: not connected");
    return;
  }
#endif
  if (tallyNum == 0) return;
  pendingSwitch.tallyNum = tallyNum;
  pendingSwitch.autoTransition = autoTransition;
  pendingSwitch.active = true;
  const char* req = "{\"op\":6,\"d\":{\"requestType\":\"GetSceneList\",\"requestId\":\"s\",\"requestData\":{}}}";
  obs_send_text(req, strlen(req));
}

void obs_message_handler(StaticJsonDocument<512> doc) {
  switch ((int)doc["op"]) {
  case 5: {
    if (doc["d"]["eventType"] == "CurrentProgramSceneChanged") {
      // https://github.com/obsproject/obs-websocket/blob/5.3.3/docs/generated/protocol.md#getsceneitemlist
      programBits = bitsFromTags(doc["d"]["eventData"]["sceneName"]);
      programBits = programBits | DSKbits;
      espnow_tally(&programBits, &previewBits);
    }
    else if (doc["d"]["eventType"]  == "CurrentPreviewSceneChanged") {
      previewBits = bitsFromTags(doc["d"]["eventData"]["sceneName"]);
      espnow_tally(&programBits, &previewBits);
    }
    else if (doc["d"]["eventType"] == "SceneTransitionStarted") {
      programBits |= previewBits;
      espnow_tally(&programBits, &previewBits);
    }
    else if (doc["d"]["eventType"] == "CustomEvent"
            && doc["d"]["eventData"]["type"] == "tally") {
      uint64_t bits = bitn(doc["d"]["eventData"]["to"].as<int>());
      espnow_signal(doc["d"]["eventData"]["signal"].as<uint8_t>(), &bits);
    }
    else if (doc["d"]["eventType"] == "VendorEvent" && doc["d"]["eventData"]["vendorName"] == "downstream-keyer") {
      DSKbits = bitsFromTags(doc["d"]["eventData"]["eventData"]["new_scene"]);
      programBits = programBits | DSKbits;
      espnow_tally(&programBits, &previewBits);
		}
    break;
  }
  case 7:
    if (doc["d"]["requestType"] == "GetCurrentProgramScene") {
      // https://github.com/obsproject/obs-websocket/blob/5.3.3/docs/generated/protocol.md#getsceneitemlist
      programBits = bitsFromTags(doc["d"]["responseData"]["currentProgramSceneName"]);
      programBits |= DSKbits;
      espnow_tally(&programBits, &previewBits);
    } else if (doc["d"]["requestType"] == "GetCurrentPreviewScene") {
      previewBits = bitsFromTags(doc["d"]["responseData"]["currentPreviewSceneName"]);
      espnow_tally(&programBits, &previewBits);
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
    obs_send_text(op1, strlen(op1));
    break;
    }
  }
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3

// One complete text message from the OBS server.
static void obs_handle_ws_text(const char* payload, size_t len) {
  if (jsonContains(payload, len, "GetSceneList")) {
    obs_handle_scene_list(payload, len);
    return;
  }
  StaticJsonDocument<512> doc;  // list of scenes is larger than 512 bytes
  auto error = deserializeJson(doc, payload, len);
  if (error) {
    Serial.print(F("deserializeJson() failed with code "));
    Serial.println(error.c_str());
    return;
  }
  obs_message_handler(doc);
}

static void obs_ws_event(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
  case WStype_CONNECTED:
    Serial.println("WS CONNECTED");
    obs_ws_connected = true;
    wsTextMsg = "";
    wsTextTooBig = false;
    break;
  case WStype_DISCONNECTED:
    Serial.println("WS DISCONNECTED");
    obs_ws_connected = false;
    break;
  case WStype_TEXT:
    obs_handle_ws_text((const char*)payload, length);
    break;
  case WStype_FRAGMENT_TEXT_START:
    wsTextMsg = "";
    wsTextTooBig = false;
    // fall through: a fragment start is also accumulated like a middle part
  case WStype_FRAGMENT:
    if (wsTextTooBig) break;
    if (wsTextMsg.length() + length > WS_TEXT_MAX
        || !wsTextMsg.concat((const char*)payload, length)) {
      wsTextTooBig = true;
      wsTextMsg = "";
      Serial.println("OBS: websocket message too large, dropped");
    }
    break;
  case WStype_FRAGMENT_FIN:
    if (!wsTextTooBig) {
      obs_handle_ws_text(wsTextMsg.c_str(), wsTextMsg.length());
    }
    wsTextMsg = "";
    break;
  case WStype_ERROR:
    Serial.println("WS ERROR");
    break;
  default:
    break;
  }
}

#else

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
  switch (event_id) {
  case WEBSOCKET_EVENT_DATA: {
    // Serial.printf("Received opcode=%d\n", data->op_code);
    if (data->op_code == 1) {
      Serial.printf("<%.*s\n", data->data_len, (char *)data->data_ptr);
      const char* payload = data->data_ptr + data->payload_offset;
      size_t payloadLen = data->payload_len;
      if (jsonContains(payload, payloadLen, "GetSceneList")) {
        obs_handle_scene_list(payload, payloadLen);
        return;
      }
      StaticJsonDocument<512> doc;  // list of scenes is larger than 512 bytes
      auto error = deserializeJson(doc, payload, payloadLen);
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

#endif

void obs_setup() {
  Serial.printf("obs_setup %s:%d\n", config.ip.toString().c_str(), config.port);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  obs_ws_begun = true;
  wsClient.begin(config.ip, config.port, "/");
  // The library retries the connection on this interval, replacing the old
  // client-recreate backoff below (HTTP config changes restart the device,
  // so ip/port never change at runtime).
  wsClient.setReconnectInterval(5000);
  wsClient.onEvent(obs_ws_event);
#else
  char uri[64];
  sprintf(uri, "ws://%s:%d", config.ip.toString().c_str(), config.port);
  const esp_websocket_client_config_t ws_cfg = {
    .uri = uri,
  };
  client = esp_websocket_client_init(&ws_cfg);
  esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);
  esp_websocket_client_start(client);
#endif
}

void obs_loop() {

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // WebSocketsClient owns reconnection once begun (setReconnectInterval), so
  // there is no create/destroy cycle; if the boot config had no IP yet
  // (fresh flash, empty NVS), begin() is deferred until a config exists.
  if (!obs_ws_begun && config.ip != 0) {
    obs_setup();
  }
  wsClient.loop();
#else
  // esp_websocket_client has auto-reconnect enabled by default, so we do NOT
  // destroy the client every loop when it's momentarily disconnected — that
  // caused a tight create/destroy spin which prevented the WS handshake from
  // ever completing. We only (re)create the client when it is absent, paced by
  // a backoff so a down server doesn't reset the connection attempt each
  // iteration. HTTP config changes restart the device, so config.ip/port never
  // change at runtime and need no teardown here.
  if (!client && config.ip != 0) {
    static unsigned long nextRetry = 0;
    if ((long)(millis() - nextRetry) >= 0) {
      obs_setup();
      nextRetry = millis() + 5000;
    }
  }
#endif
}
