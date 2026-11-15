// OBS WebSocket client (port of src/obs.cpp).
//
// Speaks obs-websocket v5: Hello/Identify handshake, then the tally is derived
// from scene names ("T3 Camera 1" == input 3, "T1,T2" == inputs 1 and 2) via
// the program/preview scene change events. Switching asks OBS for the scene
// list and cuts the first scene tagged T<n> to program, or (auto) to preview
// plus TriggerTransition -- that one needs OBS Studio Mode.
//
// Deliberate differences from the Arduino build:
//   * each bus update only touches its own half of the tally (espnow_tally()
//     treats NULL as "leave it alone"), so OSC/ATEM/vMix changes are not
//     clobbered by a scene event; only a transition needs both halves;
//   * fragmented text messages are reassembled from payload_offset (the
//     Arduino's IDF path treats every event as a whole message);
//   * the scene name is sent with cJSON so quotes/backslashes are escaped.
//
// Not ported: obs_broadcast_signal() (no caller in either build) and
// authentication -- like the Arduino build the server must accept the
// connection without a password.

#include "obs_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "espnow.h"

static const char *TAG = "obs";

// Scenes(4) + Transitions(16) + Vendors(512): everything this client reads.
#define OBS_EVENT_SUBSCRIPTIONS 532
// GetSceneList responses are the big ones; same ceiling as the Arduino build.
#define OBS_MSG_MAX (64 * 1024)
#define OBS_SEND_TIMEOUT_MS 250

static esp_websocket_client_handle_t s_client;
static uint64_t s_dsk_bits;

// Fragment reassembly for one text message.
static char *s_msg;
static size_t s_msg_len;
static size_t s_msg_cap;
static bool s_msg_too_big;

// Pending switch request (HTTP task -> websocket task).
static portMUX_TYPE s_switch_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_switch_pending;
static uint8_t s_switch_tally;
static bool s_switch_auto;

static uint64_t bitn(uint8_t n)
{
    return (uint64_t)1 << (n - 1);
}

// Scene-name tags: 'T' at a word boundary followed by digits names an input.
static uint64_t bits_from_tags(const char *s)
{
    uint64_t bits = 0;
    size_t len = strlen(s);
    bool boundary = true;

    for (size_t i = 0; i < len; i++) {
        if (s[i] == 'T' && boundary) {
            unsigned n = 0;
            i++;
            for (; i < len; i++) {
                if (isdigit((unsigned char)s[i])) {
                    n = n * 10 + (unsigned)(s[i] - '0');
                } else {
                    break;
                }
            }
            if (n >= 1 && n <= 64) {
                bits |= bitn((uint8_t)n);
            }
        }
        if (i < len) {
            boundary = !isalnum((unsigned char)s[i]);
        }
    }
    return bits;
}

// Bounded timeout: a stalled OBS must not freeze the calling task; the client
// buffers and reconnects on its own.
static void obs_send_text(const char *buf, int len)
{
    if (s_client == NULL) {
        return;
    }
    int sent = esp_websocket_client_send_text(s_client, buf, len,
                                              pdMS_TO_TICKS(OBS_SEND_TIMEOUT_MS));
    if (sent < 0) {
        ESP_LOGW(TAG, "send failed (%d bytes)", len);
    }
}

static void send_json(const cJSON *root)
{
    if (root == NULL) {
        return;
    }
    char *out = cJSON_PrintUnformatted(root);
    if (out == NULL) {
        ESP_LOGW(TAG, "json print failed (out of memory?)");
        return;
    }
    obs_send_text(out, (int)strlen(out));
    cJSON_free(out);
}

// ---- outgoing requests ----------------------------------------------------

// Scenes + Transitions + Vendors, which is what the handlers below read.
static void send_identify(void)
{
    char msg[96];
    int len = snprintf(msg, sizeof(msg),
                       "{\"op\":1,\"d\":{\"rpcVersion\":1,"
                       "\"eventSubscriptions\":%d}}",
                       OBS_EVENT_SUBSCRIPTIONS);
    if (len > 0 && len < (int)sizeof(msg)) {
        obs_send_text(msg, len);
    }
}

static void request_current_scenes(void)
{
    static const char *op1 =
        "{\"op\":6,\"d\":{\"requestType\":\"GetCurrentProgramScene\","
        "\"requestId\":\"a\",\"requestData\":{}}}";
    static const char *op2 =
        "{\"op\":6,\"d\":{\"requestType\":\"GetCurrentPreviewScene\","
        "\"requestId\":\"a\",\"requestData\":{}}}";
    obs_send_text(op1, (int)strlen(op1));
    obs_send_text(op2, (int)strlen(op2));
}

static void send_switch(const char *scene_name, bool auto_transition)
{
    const char *request_type = auto_transition ? "SetCurrentPreviewScene"
                                               : "SetCurrentProgramScene";
    cJSON *root = cJSON_CreateObject();
    cJSON *d = NULL;
    cJSON *rd = NULL;
    if (root != NULL) {
        cJSON_AddNumberToObject(root, "op", 6);
        d = cJSON_AddObjectToObject(root, "d");
    }
    if (d != NULL) {
        cJSON_AddStringToObject(d, "requestType", request_type);
        cJSON_AddStringToObject(d, "requestId", "p");
        rd = cJSON_AddObjectToObject(d, "requestData");
    }
    if (rd != NULL) {
        cJSON_AddStringToObject(rd, "sceneName", scene_name);
    }
    send_json(root);
    cJSON_Delete(root);

    if (auto_transition) {
        static const char *trig =
            "{\"op\":6,\"d\":{\"requestType\":\"TriggerTransition\","
            "\"requestId\":\"t\",\"requestData\":{}}}";
        obs_send_text(trig, (int)strlen(trig));
    }
}

// ---- incoming messages ----------------------------------------------------

static const cJSON *json_obj(const cJSON *parent, const char *key)
{
    return parent == NULL ? NULL : cJSON_GetObjectItem(parent, key);
}

static const char *json_str(const cJSON *parent, const char *key)
{
    const cJSON *item = json_obj(parent, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static long json_int(const cJSON *parent, const char *key, long fallback)
{
    const cJSON *item = json_obj(parent, key);
    return cJSON_IsNumber(item) ? (long)item->valuedouble : fallback;
}

// Replaces the program bus, leaving preview untouched.
static void set_program(uint64_t program)
{
    espnow_tally(&program, NULL);
}

// Replaces the preview bus, leaving program untouched.
static void set_preview(uint64_t preview)
{
    espnow_tally(NULL, &preview);
}

static void handle_scene_list(const cJSON *d)
{
    bool pending;
    uint8_t tally;
    bool auto_transition;
    taskENTER_CRITICAL(&s_switch_lock);
    pending = s_switch_pending;
    tally = s_switch_tally;
    auto_transition = s_switch_auto;
    s_switch_pending = false;
    taskEXIT_CRITICAL(&s_switch_lock);
    if (!pending) {
        return;
    }

    const cJSON *scenes = json_obj(json_obj(d, "responseData"), "scenes");
    int count = cJSON_IsArray(scenes) ? cJSON_GetArraySize(scenes) : 0;
    uint64_t target = bitn(tally);
    const char *found = NULL;
    for (int i = 0; i < count; i++) {
        const cJSON *scene = cJSON_GetArrayItem(scenes, i);
        const char *name = json_str(scene, "sceneName");
        if (name != NULL && (bits_from_tags(name) & target)) {
            found = name;
            break;
        }
    }
    if (found == NULL) {
        ESP_LOGW(TAG, "no scene tagged T%u found", tally);
        return;
    }

    ESP_LOGI(TAG, "%s -> '%s'", auto_transition ? "auto" : "cut", found);
    send_switch(found, auto_transition);
}

static void handle_event(const cJSON *d)
{
    const char *type = json_str(d, "eventType");
    const cJSON *data = json_obj(d, "eventData");
    if (type == NULL) {
        return;
    }

    if (strcmp(type, "CurrentProgramSceneChanged") == 0) {
        const char *name = json_str(data, "sceneName");
        set_program(bits_from_tags(name != NULL ? name : "") | s_dsk_bits);
    } else if (strcmp(type, "CurrentPreviewSceneChanged") == 0) {
        const char *name = json_str(data, "sceneName");
        set_preview(bits_from_tags(name != NULL ? name : ""));
    } else if (strcmp(type, "SceneTransitionStarted") == 0) {
        // A transition is running: whatever is on preview joins program.
        uint64_t program = 0;
        uint64_t preview = 0;
        espnow_get_tally(&program, &preview);
        program |= preview;
        espnow_tally(&program, &preview);
    } else if (strcmp(type, "CustomEvent") == 0) {
        if (strcmp(json_str(data, "type") != NULL ? json_str(data, "type") : "",
                   "tally") == 0) {
            long to = json_int(data, "to", 0);
            if (to >= 1 && to <= 64) {
                uint64_t bits = bitn((uint8_t)to);
                espnow_signal((uint8_t)json_int(data, "signal", 0), &bits);
            }
        }
    } else if (strcmp(type, "VendorEvent") == 0) {
        const char *vendor = json_str(data, "vendorName");
        if (vendor != NULL && strcmp(vendor, "downstream-keyer") == 0) {
            // A downstream keyer overlays another scene on program.
            const char *name = json_str(json_obj(data, "eventData"), "new_scene");
            s_dsk_bits = bits_from_tags(name != NULL ? name : "");
            uint64_t program = 0;
            espnow_get_tally(&program, NULL);
            program |= s_dsk_bits;
            espnow_tally(&program, NULL);   // preview stays as it is
        }
    }
}

static void handle_response(const cJSON *d)
{
    const char *request_type = json_str(d, "requestType");
    const cJSON *rd = json_obj(d, "responseData");
    if (request_type == NULL) {
        return;
    }

    if (strcmp(request_type, "GetCurrentProgramScene") == 0) {
        const char *name = json_str(rd, "currentProgramSceneName");
        set_program(bits_from_tags(name != NULL ? name : "") | s_dsk_bits);
    } else if (strcmp(request_type, "GetCurrentPreviewScene") == 0) {
        const char *name = json_str(rd, "currentPreviewSceneName");
        set_preview(bits_from_tags(name != NULL ? name : ""));
    } else if (strcmp(request_type, "GetSceneList") == 0) {
        handle_scene_list(d);
    }
}

static void dispatch_message(const char *buf, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(buf, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "json parse failed (%u bytes)", (unsigned)len);
        return;
    }

    const cJSON *d = json_obj(root, "d");
    switch (json_int(root, "op", -1)) {
    case 0:  // Hello -> Identify
        send_identify();
        break;
    case 2:  // Identified -> ask where we are
        request_current_scenes();
        break;
    case 5:  // Event
        handle_event(d);
        break;
    case 7:  // RequestResponse
        handle_response(d);
        break;
    default:
        break;
    }
    cJSON_Delete(root);
}

// ---- websocket plumbing ---------------------------------------------------

// OBS (QWebSocket) fragments large text messages; reassemble them here so the
// JSON parser always sees a whole document.
static void accumulate_text(const esp_websocket_event_data_t *data)
{
    if (data->payload_offset == 0 && data->op_code == 1) {
        s_msg_len = 0;
        s_msg_too_big = false;
    }
    if (s_msg_too_big) {
        return;
    }
    size_t need = s_msg_len + (size_t)data->data_len + 1;
    if (need > OBS_MSG_MAX) {
        s_msg_too_big = true;
        ESP_LOGW(TAG, "message over %u bytes, dropped", OBS_MSG_MAX);
        return;
    }
    if (need > s_msg_cap) {
        size_t cap = s_msg_cap != 0 ? s_msg_cap : 2048;
        while (cap < need) {
            cap *= 2;
        }
        if (cap > OBS_MSG_MAX) {
            cap = OBS_MSG_MAX;
        }
        char *grown = realloc(s_msg, cap);
        if (grown == NULL) {
            s_msg_too_big = true;
            ESP_LOGW(TAG, "out of memory reassembling message");
            return;
        }
        s_msg = grown;
        s_msg_cap = cap;
    }
    memcpy(s_msg + s_msg_len, data->data_ptr, (size_t)data->data_len);
    s_msg_len += (size_t)data->data_len;
    s_msg[s_msg_len] = '\0';

    if (data->fin) {
        if (!s_msg_too_big) {
            dispatch_message(s_msg, s_msg_len);
        }
        // Scene lists are the only large documents; give the memory back.
        free(s_msg);
        s_msg = NULL;
        s_msg_cap = 0;
        s_msg_len = 0;
    }
}

static void ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    const esp_websocket_event_data_t *data = event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 1 || data->op_code == 0) {
            accumulate_text(data);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "websocket error (handshake status %d)",
                 data->error_handle.esp_ws_handshake_status_code);
        break;
    default:
        break;
    }
}

// ---- public API -----------------------------------------------------------

void obs_request_switch(uint8_t tally_num, bool auto_transition)
{
    if (tally_num == 0 || tally_num > 64) {
        return;
    }
    if (s_client == NULL || !esp_websocket_client_is_connected(s_client)) {
        ESP_LOGW(TAG, "switch to %u dropped: not connected", tally_num);
        return;
    }

    taskENTER_CRITICAL(&s_switch_lock);
    s_switch_tally = tally_num;
    s_switch_auto = auto_transition;
    s_switch_pending = true;
    taskEXIT_CRITICAL(&s_switch_lock);

    static const char *req =
        "{\"op\":6,\"d\":{\"requestType\":\"GetSceneList\","
        "\"requestId\":\"s\",\"requestData\":{}}}";
    obs_send_text(req, (int)strlen(req));
}

esp_err_t obs_start(void)
{
    char ip[16];
    esp_ip4addr_ntoa(&g_config.ip, ip, sizeof(ip));
    char uri[64];
    snprintf(uri, sizeof(uri), "ws://%s:%u", ip, g_config.port);
    ESP_LOGI(TAG, "target %s", uri);

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .task_stack = 6144,
        .buffer_size = 4096,
        .disable_auto_reconnect = false,
        .reconnect_timeout_ms = 5000,
    };
    s_client = esp_websocket_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "client init failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                                 ws_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register events failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client start failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
