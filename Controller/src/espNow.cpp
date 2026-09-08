#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <WiFi.h>

#include "atem.h"
#include "espnow.h"
#include "configWebserver.h"
#include "vmixServer.h"

#define MAX_TALLY_COUNT 64

// Broadcast address, sends to all devices nearby
uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
esp_now_peer_info_t peerInfo;
espnow_tally_info_t tallies[MAX_TALLY_COUNT];
uint64_t programBits = 0;
uint64_t previewBits = 0;
long lastMessageAt = -10000;

// ---- Generic ESP-NOW burst pump ----
// Keeps re-sending a small payload every `config.tally_burst_gap` ms for
// `config.tally_burst_ms` so a dropped packet does not lose a tally update.
// Receivers are always listening while awake, so the burst is pure redundancy.
//
// Explicit receiver sleep: once a burst drains we send SLEEP(sleep_ms) and then
// stay silent for sleep_ms, letting receivers power down their radio. A new
// tally change that arrives during that window is latched (tally_dirty) and
// flushed as soon as the receivers wake. sleep_ms == 0 keeps receivers awake.
// Idle-set keepalive cadence. Receivers blank their broadcast tally and show
// a moving red "no signal" pixel after 5 s of silence (Receiver/main.c), so the
// controller MUST keep re-bursting the current tally even when the switcher is
// quiet. The natural cadence is sleep_ms + tally_burst_ms (the burst/sleep
// cycle the controller already drives); that keeps power-save intact while
// staying well under the 5 s receiver timeout. We clamp it so sleep_ms == 0
// (always-on receivers) does not spam packets every loop tick, and so a very
// large sleep_ms still fires shortly after the receivers wake.
#define RX_NO_SIGNAL_MS   5000UL   // receiver blanks after this much silence
#define KEEPALIVE_MIN_MS   1000UL   // floor: no more than ~1 burst/sec when idle
#define KEEPALIVE_MAX_MS   4000UL   // ceiling: leaves >=1s margin under RX timeout
static inline unsigned long keepalive_window_ms() {
  unsigned long cycle = (unsigned long)config.sleep_ms + config.tally_burst_ms;
  if (cycle > KEEPALIVE_MAX_MS) cycle = KEEPALIVE_MAX_MS;
  if (cycle < KEEPALIVE_MIN_MS) cycle = KEEPALIVE_MIN_MS;
  return cycle;
}
static unsigned long burst_until = 0;   // 0 == idle
static unsigned long burst_next  = 0;
static uint8_t  burst_payload[16];
static uint8_t  burst_len = 0;
static unsigned long sleep_until = 0;   // receivers sleep until this millis(); no new sends before it
static bool tally_dirty = false;       // a tally change is pending a burst
static bool burst_active_prev = false; // edge detection: burst just drained

static void startBurst(const uint8_t *payload, uint8_t len, uint32_t duration_ms) {
  if (len > sizeof(burst_payload)) len = sizeof(burst_payload);
  memcpy(burst_payload, payload, len);
  burst_len   = len;
  burst_until = millis() + duration_ms;
  // Fire the first packet immediately (latency for already-awake receivers).
  esp_err_t r = esp_now_send(broadcast_mac, burst_payload, burst_len);
  if (r != ESP_OK) Serial.println("esp_now_send != OK");
  burst_next = millis() + config.tally_burst_gap;   // schedule the rest
}

// One non-blocking tick. Call from the main loop. Sends 0..1 packets.
static bool espnow_burst_tick() {
  if (burst_len == 0) return false;
  if ((long)(millis() - burst_until) >= 0) { burst_len = 0; return false; } // done
  if ((long)(millis() - burst_next)  <  0) return true;                    // not yet
  burst_next += config.tally_burst_gap;
  if ((long)(millis() - burst_next) > 0) burst_next = millis();            // catch up
  esp_err_t r = esp_now_send(broadcast_mac, burst_payload, burst_len);
  if (r != ESP_OK) Serial.println("esp_now_send != OK");
  return true;
}

espnow_tally_info_t * espnow_tallies() {
  return tallies;
}

static void espnow_flush_tally();  // forward decl: espnow_tally flushes on change

void espnow_tally() {
  espnow_tally(&programBits, &previewBits);
}

// Called on every tally change (ATEM callback / OBS event / vMix tally line)
// and on the periodic refresh. Fires downstream side effects ONCE per call,
// then arms a SET_TALLY burst so a dropped packet does not stick. A NULL
// pointer means "leave that bitfield unchanged" (used by the /program and
// /preview OSC endpoints).
void espnow_tally(uint64_t *program, uint64_t *preview) {
  if (program) programBits = *program;
  if (preview) previewBits = *preview;
  vmix_tally(&programBits, &previewBits);
  ws_tally();
  tally_dirty = true;
  espnow_flush_tally();
}

// Start a SET_TALLY burst now, unless the receivers are sleeping (then the
// change stays latched in tally_dirty and is flushed when they wake) or a
// burst is already running (startBurst then refreshes its payload/time).
static void espnow_flush_tally() {
  if (!tally_dirty) return;
  if ((long)(millis() - sleep_until) < 0) return;   // receivers sleeping: defer
  tally_dirty = false;
  lastMessageAt = millis();
  uint8_t payload[1 + sizeof(uint64_t) + sizeof(uint64_t)];
  payload[0] = SET_TALLY;
  memcpy(payload + 1, &programBits, sizeof(uint64_t));
  memcpy(payload + 1 + sizeof(uint64_t), &previewBits, sizeof(uint64_t));
  startBurst(payload, sizeof(payload), config.tally_burst_ms);
}

void switchCamId(uint8_t id1, uint8_t id2) {
  uint8_t payload[3] = {SWITCH_CAMID, id1, id2};
  esp_err_t result = esp_now_send(broadcast_mac, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

void espnow_brightness(uint8_t brightness, uint64_t *bits) {
  uint8_t payload[2+sizeof(uint64_t)];
  payload[0] = SET_BRIGHTNESS;
  payload[1] = brightness;
  memcpy(payload+2, bits, sizeof(*bits));
  if (!esp_now_send(broadcast_mac, payload, sizeof(payload))) {
    Serial.println("esp_now_send != OK");
  }
}

void espnow_camid(uint8_t camId, uint64_t *bits) {
  uint8_t payload[2+sizeof(uint64_t)];
  payload[0] = SET_CAMID;
  payload[1] = camId;
  memcpy(payload+2, bits, sizeof(*bits));
  esp_err_t result = esp_now_send(broadcast_mac, payload, sizeof(payload));
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
  esp_err_t result = esp_now_send(broadcast_mac, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

void espnow_signal(uint8_t signal, uint64_t *bits) {
  // "/signal\0,ii\0{tallyid}{signal}"
  uint8_t payload[1+sizeof(uint64_t)];
  payload[0] = signal;  // Signal number is command number
  memcpy(payload+1, bits, sizeof(*bits));
  esp_err_t result = esp_now_send(broadcast_mac, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

// callback when data is sent (WiFi task context: only touch cheap flags).
// Broadcast has no MAC-level ack, so failures are rare (queue exhaustion,
// radio contention) -- but a rising streak means receivers are missing our
// packets, and we force a resync burst instead of waiting for the keepalive.
#define SEND_FAIL_RESYNC_STREAK 3
static volatile uint8_t sendFailStreak = 0;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
// IDF 5.5 hands the send callback a wifi_tx_info_t instead of the peer MAC;
// only the status is used below.
void OnDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
{
  (void)tx_info;
#else
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
  (void)mac_addr;
#endif
  if (status == ESP_NOW_SEND_SUCCESS) {
    sendFailStreak = 0;
  } else if (sendFailStreak < 255) {
    sendFailStreak = (uint8_t)(sendFailStreak + 1);   // volatile ++ is deprecated
  }
}

// callback when data is received
#if ESP_IDF_VERSION_MAJOR >= 5
// IDF 5.x hands the callback an esp_now_recv_info_t that carries the sender
// MAC, so unwrap it to keep the body below unchanged.
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
  const uint8_t *mac_addr = info->src_addr;
#else
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *data, int len)
{
#endif
  espnow_command command = (espnow_command) data[0];
  // Serial.printf("Command[%d]: ", len);
  switch (command)
  {
  case HEARTBEAT: {
    if (len < 2) break;   // payload is [cmd][camId]
    Serial.printf("HEARTBEAT %d\n", data[1]);
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
  memcpy(peerInfo.peer_addr, broadcast_mac, 6);
  peerInfo.channel = 0;
  peerInfo.ifidx = WIFI_IF_STA;   // be explicit: default would be STA anyway
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

// Tell every receiver it may power down its radio for `ms`. Sent once, right
// after a burst drains, when the receivers are awake and listening. The caller
// must then stay silent for `ms` so the sleep is real (see sleep_until).
void espnow_sleep_all(uint16_t ms) {
  uint8_t payload[3];
  payload[0] = SLEEP;
  payload[1] = ms & 0xFF;
  payload[2] = (ms >> 8) & 0xFF;
  esp_err_t r = esp_now_send(broadcast_mac, payload, sizeof(payload));
  if (r != ESP_OK) Serial.println("esp_now_send != OK");
}

// Forget receivers we have not heard from in a while so the find-or-add scan
// in OnDataRecv and any UI listing tallies[] don't accumulate ghosts.
#define TALLY_STALE_MS 60000UL

static void prune_stale_tallies() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_TALLY_COUNT; i++) {
    if (tallies[i].id != 0 && now - tallies[i].last_seen > TALLY_STALE_MS) {
      tallies[i].id = 0;
      memset(tallies[i].mac_addr, 0, sizeof(tallies[i].mac_addr));
    }
  }
}

void espnow_loop() {
  prune_stale_tallies();
  // Drive any in-flight burst first (one packet per ~tally_burst_gap tick).
  bool active = espnow_burst_tick();
  // Edge: a burst just drained -> tell receivers they may sleep, and hold off
  // our own sends until they wake.
  if (burst_active_prev && !active && config.sleep_ms > 0) {
    espnow_sleep_all(config.sleep_ms);
    sleep_until = millis() + config.sleep_ms;
  }
  burst_active_prev = active;
  if (active) return;
  // Receivers may be sleeping: do not start anything new until they wake.
  if ((long)(millis() - sleep_until) < 0) return;
  // Awake window: flush a tally change that arrived during sleep.
  espnow_flush_tally();
  // Delivery failures piling up -> force a resync burst right away instead of
  // waiting for the periodic keepalive window.
  if (sendFailStreak >= SEND_FAIL_RESYNC_STREAK) {
    sendFailStreak = 0;
    Serial.println("espnow: send failures, forcing resync burst");
    tally_dirty = true;
    espnow_flush_tally();
  }
  if (burst_len != 0) return;                  // flush armed a burst
  // Keepalive: re-burst the current tally on a cadence safely below the
  // receiver's 5 s no-signal timeout so receivers stay lit and resync even
  // when the switcher never changes. Without this the strip blanks within
  // a few seconds of idle.
  if (millis() - lastMessageAt > keepalive_window_ms()) {
    tally_dirty = true;
    espnow_flush_tally();
  }
}

