#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <stdint.h> 
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "driver/gpio.h"

static const char *TAG = "tally";
#define DEBUG 1
#undef DEBUG

#define TALLY_COUNT 64  // number of tally lights (max 64)
#define TALLY_UPDATE_EACH 60000 // 1 minute;
#define CONFIG_ESPNOW_CHANNEL 1
#define DEFAULT_CAMID 3
#define DEFAULT_CAMGROUP 0

// Keep minumum current so powerbank will not shutoff
#define BACKGROUND_COLOR 0,0,0

// Explicit ESP-NOW power-save.
// The receiver is always listening by default (no implicit duty-cycling). When
// the Controller finishes a burst it sends SLEEP(ms): we may then power down
// the radio for `ms` because the Controller promises to stay silent for that
// long. sleep_request_ms is latched by the receive callback (which runs in the
// WiFi task and must not block) and consumed by app_main, which performs the
// actual sleep. See receiver_sleep().
volatile uint16_t sleep_request_ms = 0;


#define RMT_LED_STRIP_RESOLUTION_HZ 10000000 // 10MHz resolution, 1 tick = 0.1us (led strip needs a high resolution)
#ifdef CONFIG_IDF_TARGET_ESP32S3
 #define RMT_LED_STRIP_GPIO_NUM  14
 #define LED_COUNT  64
 #define DEFAULT_BRIGHTNESS 2  // 0-255
 // BOOT button on most ESP32-S3 dev/feather boards (active-low, pull-up)
 #define BUTTON_GPIO_NUM  0
#else //elifdef CONFIG_IDF_TARGET_ESP32C3
 #define RMT_LED_STRIP_GPIO_NUM  8
 #define LED_COUNT  25
 #define DEFAULT_BRIGHTNESS 10  // 0-255
 // BOOT button on ESP32-C3 DevKitM (active-low, pull-up)
 #define BUTTON_GPIO_NUM  9
#endif

// On-board button cycles camId through this range and persists it to NVS.
#define CAMID_MIN 1
#define CAMID_MAX 10
#define BUTTON_DEBOUNCE_MS 250   // min ms between accepted presses

uint8_t led_strip_pixels[LED_COUNT * 3];
rmt_channel_handle_t led_chan = NULL;
rmt_tx_channel_config_t tx_chan_config = {
    .gpio_num = RMT_LED_STRIP_GPIO_NUM,
    .clk_src = RMT_CLK_SRC_DEFAULT, // select source clock
    .resolution_hz = RMT_LED_STRIP_RESOLUTION_HZ,
    .mem_block_symbols = 64, // increase the block size can make the LED less flickering
    .trans_queue_depth = 4, // set the number of transactions that can be pending in the background
};
rmt_encoder_handle_t led_encoder = NULL;
rmt_transmit_config_t tx_config = {
    .loop_count = 0, // no transfer loop
};
led_strip_encoder_config_t encoder_config = {
    .resolution = RMT_LED_STRIP_RESOLUTION_HZ,
};

uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
nvs_handle_t nvs_tally_handle;

unsigned long lastMessageReceived = -TALLY_UPDATE_EACH;
uint8_t camId = DEFAULT_CAMID;
uint8_t camGroup = DEFAULT_CAMGROUP;
uint8_t bright_ratio = 255/DEFAULT_BRIGHTNESS;

typedef enum {
  SET_TALLY = 1,
  GET_TALLY = 2,
  SWITCH_CAMID = 3,
  HEARTBEAT = 4,
  SET_CAMID = 5,
  SET_COLOR = 6,
  SET_BRIGHTNESS = 7,
  SHOW_SIGNAL = 8,
  SET_CAMGROUP = 9,
  // 10 was SET_WAKE (implicit receiver duty-cycle, removed).
  SLEEP = 11,  // [cmd][dur_lo][dur_hi]: receiver may sleep `dur` ms

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
  SIGNAL_OK = 23,
} espnow_command;

unsigned long millis() {
  return esp_timer_get_time() / 1000;
}

void delay(long ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
}

// ---- On-board button (cycle camId) ----
// Uses the BOOT button already present on the dev boards, so no extra wiring
// is required. It is active-low with the internal pull-up enabled. We poll it
// from the main loop (which already sleeps ~50ms per iteration) and debounce
// on the falling (press) edge.
// Forward declarations: writeCamId() and displayNumber() are defined below.
void writeCamId(void);
void displayNumber(uint8_t r, uint8_t g, uint8_t b, uint8_t number);
static int button_last_state = 1;  // 1 = not pressed (pull-up)
static uint32_t button_last_press_ms = 0;

void button_init() {
  gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << BUTTON_GPIO_NUM),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&io_conf);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "button gpio_config failed: %s", esp_err_to_name(err));
  }
}

// Returns true exactly once per (debounced) press. Call this from the main loop.
bool button_pressed() {
  int state = gpio_get_level(BUTTON_GPIO_NUM);
  bool pressed = false;
  if (state == 0 && button_last_state == 1) {  // falling edge
    if (millis() - button_last_press_ms > BUTTON_DEBOUNCE_MS) {
      button_last_press_ms = millis();
      pressed = true;
    }
  }
  button_last_state = state;
  return pressed;
}

// Cycle camId CAMID_MIN..CAMID_MAX, persist to NVS, and flash the new number.
void cycle_camid() {
  camId++;
  if (camId > CAMID_MAX || camId < CAMID_MIN) camId = CAMID_MIN;
  writeCamId();
  nvs_commit(nvs_tally_handle);  // make sure the new id survives reboot
  displayNumber(0, 0, 255, camId);
  // Keep the number on screen briefly even if no tally messages arrive:
  // suppress the "no signal" red-pixel blink for a moment.
  lastMessageReceived = millis();
  ESP_LOGI(TAG, "button: camId -> %d", camId);
}

void show() {
  ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config));
}

// y arrives from the UI slider (0..250). bright_ratio is a DIVISOR applied to
// every color channel below, so y=128 passes colors through unchanged and
// y<128 dims them. y>128 used to give bright_ratio = 128/y = 0, and
// "color / 0" is 0xFFFFFFFF on RISC-V (ESP32-C3) / UB on Xtensa, truncating
// to 255 per channel => white instead of red/green. Clamp y to [1,128] so the
// divisor is always >= 1. Capping at 128 loses nothing: tally channels are 0 or
// 255, so boosting above unity just clamps right back to 255.
void setBrightness(uint8_t y) {
  if (y == 0) y = 1;
  else if (y > 128) y = 128;
  bright_ratio = 128 / y;
}

inline void setPixelColor(int i, uint8_t r, uint8_t g, uint8_t b) {
#ifdef CONFIG_IDF_TARGET_ESP32S3
  led_strip_pixels[i * 3 + 0] = r;
  led_strip_pixels[i * 3 + 1] = g;
  led_strip_pixels[i * 3 + 2] = b;
#else //ifdef CONFIG_IDF_TARGET_ESP32C3
  led_strip_pixels[i * 3 + 0] = g;
  led_strip_pixels[i * 3 + 1] = r;
  led_strip_pixels[i * 3 + 2] = b;
#endif
}

void colorBlack() {
  memset(led_strip_pixels, 0, sizeof(led_strip_pixels));
}

void colorBlink(uint8_t r, uint8_t g, uint8_t b, int wait) {
  int count = 3;
  uint16_t i;
  r = r/bright_ratio;
  g = g/bright_ratio;
  b = b/bright_ratio;
  while (count--) {
    for(i=0; i<LED_COUNT; i++) setPixelColor(i, r, g, b);
    show();
    delay(wait);
    for(i=0; i<LED_COUNT; i++) setPixelColor(i, 0, 0, 0);
    show();
    delay(wait);
  }
}

// Fill strip pixels one after another with a color. Strip is NOT cleared
void colorWipe(uint8_t r, uint8_t g, uint8_t b, int wait) {
  r = r/bright_ratio;
  g = g/bright_ratio;
  b = b/bright_ratio;
  for(int i=0; i<LED_COUNT; i++) { // For each pixel in strip...
    setPixelColor(i, r, g, b);       // Set pixel's color (in RAM)
    show();                          // Update strip to match
    delay(wait);                     // Pause for a moment
  }
}

void readCamId() {
  esp_err_t err = nvs_get_u8(nvs_tally_handle, "camId", &camId);
  switch (err) {
      case ESP_OK:
          break;
      case ESP_ERR_NVS_NOT_FOUND:
          printf("camId not saved yet!\n");
          camId = DEFAULT_CAMID;
          err = nvs_set_u8(nvs_tally_handle, "camId", camId);
          printf((err != ESP_OK) ? "Failed to save default camId.\n" : "Default camId saved.\n");
          // Commit written value.
          // After setting any values, nvs_commit() must be called to ensure changes are written
          // to flash storage. Implementations may write to storage at other times,
          // but this is not guaranteed.
          printf("Committing updates in NVS ... ");
          err = nvs_commit(nvs_tally_handle);
          printf((err != ESP_OK) ? "Failed!\n" : "Done\n");
          break;
      default :
          printf("Error (%s) reading!\n", esp_err_to_name(err));
  }

  // Close
  // nvs_close(nvs_tally_handle);
  ESP_LOGI(TAG, "camId: %d", camId);
}

void readCamGroup() {
  esp_err_t err = nvs_get_u8(nvs_tally_handle, "camGroup", &camGroup);
  switch (err) {
      case ESP_OK:
          break;
      case ESP_ERR_NVS_NOT_FOUND:
          camGroup = DEFAULT_CAMGROUP;
          err = nvs_set_u8(nvs_tally_handle, "camGroup", camGroup);
          if (err == ESP_OK) {
            printf("Default camGroup saved.\n");
            err = nvs_commit(nvs_tally_handle);
            printf((err != ESP_OK) ? "Failed!\n" : "Done\n");
          } else {
            printf("Failed to save default camGroup.\n");
          }
          break;
      default :
          printf("Error (%s) reading!\n", esp_err_to_name(err));
  }

  ESP_LOGI(TAG, "camGroup: %d", camGroup);
}

void writeCamId(void) {
  if (nvs_set_u8(nvs_tally_handle, "camId", camId) != ESP_OK)
    ESP_LOGI(TAG, "writeCamId failed!");
}

void writeCamGroup() {
  if (nvs_set_u8(nvs_tally_handle, "camGroup", camGroup) != ESP_OK)
    ESP_LOGI(TAG, "writeCamGroup failed!");
}

// ---- Explicit receiver sleep (SLEEP command) ----
// Power down the radio for `ms`. We reuse the connectionless ESP-NOW sleep
// primitive (the proven way to let the WiFi modem sleep while ESP-NOW stays
// registered): shrink the RX wake window so the modem sleeps between brief
// wakeups, block for `ms` (esp_pm light-sleeps inside vTaskDelay), then
// restore always-on RX. The Controller guarantees no TX during `ms`.
#define SLEEP_WAKE_INTERVAL_MS 100   // connectionless wake cadence while sleeping
#define SLEEP_WAKE_WINDOW_MS   2     // minimal RX window while sleeping

void receiver_sleep(uint16_t ms) {
  esp_err_t e1 = esp_wifi_connectionless_module_set_wake_interval(SLEEP_WAKE_INTERVAL_MS);
  esp_err_t e2 = esp_now_set_wake_window(SLEEP_WAKE_WINDOW_MS);
  ESP_LOGI(TAG, "sleep %ums (interval=%u window=%u: %s/%s)", ms,
           SLEEP_WAKE_INTERVAL_MS, SLEEP_WAKE_WINDOW_MS,
           esp_err_to_name(e1), esp_err_to_name(e2));
  unsigned long end = millis() + ms;
  while ((long)(millis() - end) < 0) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  // Restore always-on RX (window >= interval => listen all the time).
  esp_now_set_wake_window(0xFFFF);
}

#if LED_COUNT==25
// 5x5 matrices for digits 0 to 9 and icon signals
const uint8_t digitsMatrix[] = {
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
  // 9
  0,1,1,1,0,
  1,0,0,0,1,
  0,1,1,1,1,
  0,0,0,0,1,
  0,1,1,1,0,
  // 10
  1,0,0,1,0,
  1,0,1,0,1,
  1,0,1,0,1,
  1,0,1,0,1,
  1,0,0,1,0,
  // 11
  0,1,0,0,1,
  1,1,0,1,1,
  0,1,0,0,1,
  0,1,0,0,1,
  0,1,0,0,1,
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
#elif LED_COUNT==64
// 5x5 matrices for digits 0 to 9 and icon signals
const uint8_t digitsMatrix[] = {
  0,0,1,1,1,1,0,0,
  0,1,0,0,0,0,1,0,
  0,1,0,0,0,0,1,0,
  0,1,0,0,0,0,1,0,
  0,1,0,0,0,0,1,0,
  0,1,0,0,0,0,1,0,
  0,1,0,0,0,0,1,0,
  0,0,1,1,1,1,0,0,

  0,0,0,0,0,1,0,0,
  0,0,0,0,1,1,0,0,
  0,0,0,1,0,1,0,0,
  0,0,1,0,0,1,0,0,
  0,0,0,0,0,1,0,0,
  0,0,0,0,0,1,0,0,
  0,0,0,0,0,1,0,0,
  0,0,0,0,0,1,0,0,

  0,0,0,1,1,1,0,0,
  0,0,1,0,0,0,1,0,
  0,1,0,0,0,0,0,1,
  0,0,0,0,0,0,1,0,
  0,0,0,0,1,1,0,0,
  0,0,1,1,0,0,0,0,
  0,1,0,0,0,0,0,0,
  0,1,1,1,1,1,1,1,

  0,1,1,1,1,1,0,0,
  1,0,0,0,0,0,1,0,
  0,0,0,0,0,0,1,0,
  0,0,1,1,1,1,0,0,
  0,0,0,0,0,0,1,0,
  0,0,0,0,0,0,1,0,
  1,0,0,0,0,0,1,0,
  0,1,1,1,1,1,0,0,

  0,0,0,0,0,1,1,0,
  0,0,0,0,1,0,1,0,
  0,0,0,1,0,0,1,0,
  0,0,1,0,0,0,1,0,
  0,1,0,0,0,0,1,0,
  0,1,1,1,1,1,1,0,
  0,0,0,0,0,0,1,0,
  0,0,0,0,0,0,1,0,

  1,1,1,1,1,1,1,0,
  1,0,0,0,0,0,0,0,
  1,0,0,0,0,0,0,0,
  1,1,1,1,1,1,0,0,
  0,0,0,0,0,0,1,0,
  0,0,0,0,0,0,0,1,
  1,0,0,0,0,0,1,0,
  0,1,1,1,1,1,0,0,

  0,0,1,1,1,1,1,0,
  0,1,0,0,0,0,0,0,
  1,0,0,0,0,0,0,0,
  1,0,1,1,1,1,0,0,
  1,1,0,0,0,0,1,0,
  1,0,0,0,0,0,0,1,
  0,1,0,0,0,0,1,0,
  0,0,1,1,1,1,0,0,

  1,1,1,1,1,1,1,1,
  0,0,0,0,0,0,1,0,
  0,0,0,0,0,1,0,0,
  0,0,0,0,1,0,0,0,
  0,0,0,0,1,0,0,0,
  0,0,0,1,0,0,0,0,
  0,0,0,1,0,0,0,0,
  0,0,0,1,0,0,0,0,

  0,0,1,1,1,1,0,0,
  0,1,0,0,0,0,1,0,
  0,1,0,0,0,0,1,0,
  0,0,1,1,1,1,0,0,
  0,1,0,0,0,0,1,0,
  0,1,0,0,0,0,1,0,
  0,1,0,0,0,0,1,0,
  0,0,1,1,1,1,0,0,
  // 9
  0,0,1,1,1,1,0,0,
  0,1,0,0,0,0,1,0,
  0,1,0,0,0,0,1,0,
  0,0,1,1,1,1,1,0,
  0,0,0,0,0,0,1,0,
  0,0,0,0,0,0,1,0,
  0,1,0,0,0,0,1,0,
  0,0,1,1,1,1,0,0,
  // 10
  0,0,1,0,0,1,1,0,
  0,1,1,0,1,0,0,1,
  1,0,1,0,1,0,0,1,
  0,0,1,0,1,0,0,1,
  0,0,1,0,1,0,0,1,
  0,0,1,0,1,0,0,1,
  0,0,1,0,1,0,0,1,
  0,0,1,0,0,1,1,0,
  // 11
  0,0,1,0,0,0,1,0,
  0,1,1,0,0,1,1,0,
  1,0,1,0,1,0,1,0,
  0,0,1,0,0,0,1,0,
  0,0,1,0,0,0,1,0,
  0,0,1,0,0,0,1,0,
  0,0,1,0,0,0,1,0,
  0,0,1,0,0,0,1,0,
  // CHANGE
  1,0,0,0,0,0,0,1,
  0,1,0,0,0,0,1,0,
  0,0,1,0,0,1,0,0,
  0,0,0,1,1,0,0,0,
  0,0,0,1,1,0,0,0,
  0,0,1,0,0,1,0,0,
  0,1,0,0,0,0,1,0,
  1,0,0,0,0,0,0,1,
  // Left
  0,0,0,1,0,0,0,0,
  0,0,1,0,0,0,0,0,
  0,1,0,0,0,0,0,0,
  1,1,1,1,1,1,1,0,
  0,1,0,0,0,0,0,0,
  0,0,1,0,0,0,0,0,
  0,0,0,1,0,0,0,0,
  0,0,0,0,0,0,0,0,
  // Down
  0,0,0,0,0,0,0,0,
  0,0,0,0,1,0,0,0,
  0,0,0,0,1,0,0,0,
  0,0,0,0,1,0,0,0,
  0,1,0,0,1,0,0,1,
  0,0,1,0,1,0,1,0,
  0,0,0,1,1,1,0,0,
  0,0,0,0,1,0,0,0,
  // Up
  0,0,0,0,1,0,0,0,
  0,0,0,1,1,1,0,0,
  0,0,1,0,1,0,1,0,
  0,1,0,0,1,0,0,1,
  0,0,0,0,1,0,0,0,
  0,0,0,0,1,0,0,0,
  0,0,0,0,1,0,0,0,
  0,0,0,0,0,0,0,0,
  // Right
  0,0,0,0,1,0,0,0,
  0,0,0,0,0,1,0,0,
  0,0,0,0,0,0,1,0,
  0,1,1,1,1,1,1,1,
  0,0,0,0,0,0,1,0,
  0,0,0,0,0,1,0,0,
  0,0,0,0,1,0,0,0,
  0,0,0,0,0,0,0,0,
  // Focus
  1,1,1,1,1,1,1,1,
  1,0,0,0,0,0,0,0,
  1,0,0,0,0,0,0,0,
  1,1,1,1,1,1,0,0,
  1,0,0,0,0,0,0,0,
  1,0,0,0,0,0,0,0,
  1,0,0,0,0,0,0,0,
  1,0,0,0,0,0,0,0,
  // DeFocus (Blur)
  1,1,1,1,1,1,0,0,
  1,0,0,0,0,0,1,0,
  1,0,0,0,0,0,1,0,
  1,1,1,1,1,1,0,0,
  1,0,0,0,0,0,1,0,
  1,0,0,0,0,0,1,0,
  1,0,0,0,0,0,1,0,
  1,1,1,1,1,1,0,0,
  // ZoomIn
  1,1,1,1,1,1,1,1,
  0,0,0,0,0,0,1,0,
  0,0,0,0,0,1,0,0,
  0,0,0,0,1,0,0,0,
  0,0,0,1,0,0,0,0,
  0,0,1,0,0,0,0,0,
  0,1,0,0,0,0,0,0,
  1,1,1,1,1,1,1,1,
  // ZoomOut
  1,1,1,1,1,1,1,1,
  0,1,0,0,0,0,0,0,
  0,0,1,0,0,0,0,0,
  0,0,0,1,0,0,0,0,
  0,0,0,0,1,0,0,0,
  0,0,0,0,0,1,0,0,
  0,0,0,0,0,0,1,0,
  1,1,1,1,1,1,1,1,
  // ISO+
  1,0,0,0,0,0,0,0,
  1,0,0,0,0,0,0,0,
  1,0,0,0,1,0,0,0,
  1,0,0,0,1,0,0,0,
  1,0,1,1,1,1,1,0,
  1,0,0,0,1,0,0,0,
  1,0,0,0,1,0,0,0,
  1,0,0,0,0,0,0,0,
  // ISO-
  1,0,0,0,0,0,0,0,
  1,0,0,0,0,0,0,0,
  1,0,0,0,0,0,0,0,
  1,0,1,1,1,1,1,0,
  1,0,0,0,0,0,0,0,
  1,0,0,0,0,0,0,0,
  1,0,0,0,0,0,0,0,
  1,0,0,0,0,0,0,0,
  // OK
  0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,1,
  0,0,0,0,0,0,1,0,
  0,0,0,0,0,1,0,0,
  0,0,0,0,1,0,0,0,
  1,0,0,1,0,0,0,0,
  0,1,1,0,0,0,0,0,
  0,0,1,0,0,0,0,0,

  0
};
#endif

void displayDigit(uint8_t r, uint8_t g, uint8_t b, uint8_t digit) {
  uint8_t x = 0;
  r = r/bright_ratio;
  g = g/bright_ratio;
  b = b/bright_ratio;
  for (int i = 0; i < LED_COUNT; i++) {
    x = digitsMatrix[digit*LED_COUNT + i];
    setPixelColor(LED_COUNT-1-i, x*r, x*g, x*b);
  }
  show();
}

// Display a digit in the 5x5 matrix using given color
void displayNumber(uint8_t r, uint8_t g, uint8_t b, uint8_t number) {
  displayDigit(r, g, b, number % 10);
  // Signify tens by number of first white pixels
  if (r+g+b > 0) {
    for(int i=0; i<LED_COUNT; i++) {
      if (number < 13) break;
      setPixelColor(i, 255/bright_ratio, 255/bright_ratio, 255/bright_ratio);
      number -= 10;
    }
  }
  show();
}

void displaySignal(uint8_t signal) {
  displayDigit(0, 0, 255, signal);
}

void fillColorDirect(uint8_t r, uint8_t g, uint8_t b) {
  for (int i=0; i<LED_COUNT; i++) {
    setPixelColor(i, r, g, b);
  }
  show();
}

void fillColor(uint8_t r, uint8_t g, uint8_t b) {
  r = r/bright_ratio;
  g = g/bright_ratio;
  b = b/bright_ratio;
  fillColorDirect(r, g, b);
}

inline bool getBit(uint64_t bits, int i) {
  return bits & ((uint64_t)1 << i);
}

// ---- RX work latching ----
// The ESP-NOW receive callback runs in the WiFi task: it must not block or
// drive peripherals (RMT, NVS, delay). It only latches the newest command
// here; app_main picks it up and renders. Last-write-wins is correct for
// tally traffic: an intermediate frame we skip is superseded by the next
// burst packet ~20 ms later anyway.
typedef struct {
  uint8_t cmd;
  uint8_t a, b, c;   // color bytes / brightness / signal number / cam id
  uint64_t bits;     // target bitmask
  uint64_t pvwBits;  // SET_TALLY preview bitmask
} rx_work_t;

static portMUX_TYPE rx_work_mux = portMUX_INITIALIZER_UNLOCKED;
static rx_work_t rx_work;
static volatile bool rx_work_pending = false;

static void latch_rx_work(uint8_t cmd, uint8_t a, uint8_t b, uint8_t c,
                          uint64_t bits, uint64_t pvwBits) {
  portENTER_CRITICAL(&rx_work_mux);
  rx_work.cmd = cmd;
  rx_work.a = a; rx_work.b = b; rx_work.c = c;
  rx_work.bits = bits;
  rx_work.pvwBits = pvwBits;
  rx_work_pending = true;
  portEXIT_CRITICAL(&rx_work_mux);
}

// Move the latched work out under a critical section (64-bit fields are not
// atomic and the callback can preempt app_main mid-copy).
static bool take_rx_work(rx_work_t *out) {
  if (!rx_work_pending) return false;
  portENTER_CRITICAL(&rx_work_mux);
  *out = rx_work;
  rx_work_pending = false;
  portEXIT_CRITICAL(&rx_work_mux);
  return true;
}

// Execute one latched command in app_main context. May block/render freely.
static void handle_rx_work(const rx_work_t *w) {
  switch (w->cmd) {

  case SET_TALLY:
    // Yellow for program+preview, red for program, green for preview, grey idle
    if (!getBit(w->bits, camId - 1) && !getBit(w->pvwBits, camId - 1)) {
      fillColorDirect(BACKGROUND_COLOR);
    } else {
      fillColor(255 * getBit(w->bits, camId - 1),
                255 * getBit(w->pvwBits, camId - 1),
                0);
    }
    break;

  case SET_COLOR:
    ESP_LOGI(TAG, "SET_COLOR #%02x%02x%02x", w->a, w->b, w->c);
    if (getBit(w->bits, camId - 1)) fillColor(w->a, w->b, w->c);
    break;

  case SHOW_SIGNAL:
    if (getBit(w->bits, camId - 1)) displaySignal(w->a);
    break;

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
  case SIGNAL_ISODOWN:
  case SIGNAL_OK:
    ESP_LOGI(TAG, "SIGNAL %u", w->cmd);
    if (getBit(w->bits, camId - 1)) displaySignal(w->cmd);
    break;

  case SET_BRIGHTNESS:
    if (getBit(w->bits, camId - 1)) setBrightness(w->a);
    break;

  case SET_CAMID:
    if (getBit(w->bits, camId - 1)) {
      camId = w->a;
      writeCamId();
      nvs_commit(nvs_tally_handle);  // make sure it survives power loss
      displayNumber(0, 0, 255, camId);
      ESP_LOGI(TAG, "SET_CAMID %d", camId);
      delay(1000);  // keep the new number visible briefly
    }
    break;

  case SET_CAMGROUP:
    if (getBit(w->bits, camId - 1)) {
      camGroup = w->a;
      writeCamGroup();
      nvs_commit(nvs_tally_handle);
      displayNumber(0, 255, 0, camGroup);
      ESP_LOGI(TAG, "SET_CAMGROUP %d", camGroup);
      delay(1000);
    }
    break;

  case SWITCH_CAMID:
    ESP_LOGI(TAG, "SWITCH_CAMID %u<>%u", w->a, w->b);
    if (camId == w->a) {
      camId = w->b;
      writeCamId();
    } else if (camId == w->b) {
      camId = w->a;
      writeCamId();
    }
    break;
  }
}

// Callback function that will be executed when data is received.
// Runs in the WiFi task: latch only, never block or render.
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  if (len < 1) return;
  espnow_command command = (espnow_command)data[0];
#ifdef DEBUG
  ESP_LOGI(TAG, "<[%d] ", command);
#endif
  switch (command) {

  case SET_TALLY: {
    if (len < 1 + 16) break;  // need both bitmasks
    uint64_t pgm, pvw;
    memcpy(&pgm, data + 1, sizeof(pgm));            // memcpy: data is unaligned
    memcpy(&pvw, data + 1 + sizeof(uint64_t), sizeof(pvw));
    latch_rx_work(SET_TALLY, 0, 0, 0, pgm, pvw);
    lastMessageReceived = millis();
#ifdef DEBUG
    printf("Program ");
    for (int i = 0; i < TALLY_COUNT; i++) printf("%d", getBit(pgm, i) ? 1 : 0);
    printf("\nPreview ");
    for (int i = 0; i < TALLY_COUNT; i++) printf("%d", getBit(pvw, i) ? 1 : 0);
    printf("\n");
#endif
    break;
  }

  case HEARTBEAT:
    break;

  case SET_COLOR: {
    if (len < 4 + 8) break;
    uint64_t bits;
    memcpy(&bits, data + 4, sizeof(bits));
    latch_rx_work(SET_COLOR, data[1], data[2], data[3], bits, 0);
    lastMessageReceived = millis();
    break;
  }

  case SHOW_SIGNAL: {
    if (len < 2 + 8) break;
    uint64_t bits;
    memcpy(&bits, data + 2, sizeof(bits));
    latch_rx_work(SHOW_SIGNAL, data[1], 0, 0, bits, 0);
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
  case SIGNAL_ISODOWN:
  case SIGNAL_OK: {
    if (len < 1 + 8) break;
    uint64_t bits;
    memcpy(&bits, data + 1, sizeof(bits));
    latch_rx_work(command, 0, 0, 0, bits, 0);
    lastMessageReceived = millis();
    break;
  }

  case SET_BRIGHTNESS: {
    if (len < 2 + 8) break;
    uint64_t bits;
    memcpy(&bits, data + 2, sizeof(bits));
    latch_rx_work(SET_BRIGHTNESS, data[1], 0, 0, bits, 0);
    lastMessageReceived = millis();
    break;
  }

  case SLEEP: {
    // [cmd][dur_lo][dur_hi]: the Controller says we may sleep `dur` ms.
    // Only latched here; app_main performs the actual sleep.
    if (len >= 3) {
      sleep_request_ms = data[1] | (data[2] << 8);
    }
    lastMessageReceived = millis();
    break;
  }

  case SET_CAMID: {
    if (len < 2 + 8) break;
    uint64_t bits;
    memcpy(&bits, data + 2, sizeof(bits));
    latch_rx_work(SET_CAMID, data[1], 0, 0, bits, 0);
    lastMessageReceived = millis();
    break;
  }

  case SET_CAMGROUP: {
    if (len < 2 + 8) break;
    uint64_t bits;
    memcpy(&bits, data + 2, sizeof(bits));
    latch_rx_work(SET_CAMGROUP, data[1], 0, 0, bits, 0);
    lastMessageReceived = millis();
    break;
  }

  case SWITCH_CAMID: {
    if (len < 3) break;
    latch_rx_work(SWITCH_CAMID, data[1], data[2], 0, 0, 0);
    lastMessageReceived = millis();
    break;
  }

  case GET_TALLY:
    ESP_LOGI(TAG, "GET_TALLY");
    break;
  }
}

void sendHeartbeat() {
  uint8_t payload[2] = {HEARTBEAT, camId};
  esp_err_t err = esp_now_send(broadcast_mac, (uint8_t *)&payload, sizeof(payload));
  #ifdef DEBUG
  ESP_LOGI(TAG, ">HEARTBEAT\n");
  #endif
  if (err != ESP_OK) ESP_LOGI(TAG, "esp_now_send returned 0x%x: %s\n", err, esp_err_to_name(err));
}

void testDigits() {
  for (int i=0; i<=24; i++) {
    displayDigit(0, 0, 255, i);
    delay(1000);
  }
}

/* WiFi should start before using ESPNOW */
static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_start());
    ESP_ERROR_CHECK( esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    // Use Long-Range only, matching the Controller (LR-only). This drops
    // 11g/11n which kept the RF front-end in a higher-power RX state, and LR
    // actually extends range. Both ends must use the same protocol mask.
    ESP_ERROR_CHECK( esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR) );
}

void app_main() {
  esp_err_t err;

  // Power management: cap CPU at 80 MHz and allow light sleep when idle.
  // Requires CONFIG_PM_ENABLE=y in sdkconfig. The WiFi driver still holds a
  // PM lock while the radio is awake (we keep PS_NONE), so packets are not
  // missed; the CPU still underclocks and halts between activity. Enable WiFi
  // power-save (WIFI_PS_MIN_MODEM) later to also let the radio sleep.
  esp_pm_config_t pm_config = {
      .max_freq_mhz = 80,
      .min_freq_mhz = 80,
      .light_sleep_enable = true,
  };
  esp_err_t pm_err = esp_pm_configure(&pm_config);
  if (pm_err != ESP_OK) {
      ESP_LOGW(TAG, "esp_pm_configure failed: %s (enable CONFIG_PM_ENABLE)",
               esp_err_to_name(pm_err));
  }

  ESP_LOGI(TAG, "rmt_new_tx_channel");
  ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));
  ESP_LOGI(TAG, "rmt_new_led_strip_encoder");
  ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));
  ESP_LOGI(TAG, "rmt_enable");
  ESP_ERROR_CHECK(rmt_enable(led_chan));
  // testDigits();

  ESP_LOGI(TAG, "button_init (GPIO %d)", BUTTON_GPIO_NUM);
  button_init();

  // Initialize NVS
  err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK( nvs_flash_erase() );
      err = nvs_flash_init();
      ESP_ERROR_CHECK(err);
  }
  err = nvs_open("tally", NVS_READWRITE, &nvs_tally_handle);
  ESP_ERROR_CHECK(err);
  readCamId();
  // strip.show();  // Turn OFF all pixels ASAP
  displayNumber(0, 0, 255, camId);
  delay(300);

  ESP_LOGI(TAG, "wifi_init");
  wifi_init();

  /* Initialize ESPNOW and register sending and receiving callback function. */
  ESP_LOGI(TAG, "esp_now_init");
  ESP_ERROR_CHECK( esp_now_init() );
  
  // Add Broadcast Peer. Zero-init first: stack garbage in ifidx/channel
  // makes esp_now_add_peer fail or bind the wrong interface.
  esp_now_peer_info_t peer = {0};
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  // memcpy(&peer.lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
  memcpy(peer.peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
  err = esp_now_add_peer(&peer);
  if (unlikely(err != ESP_OK)) {
    ESP_LOGI(TAG, "esp_now_add_peer returned 0x%x: %s\n", err, esp_err_to_name(err));
  }
  // ESP_ERROR_CHECK( esp_now_register_send_cb(espnow_send_cb) );
  ESP_ERROR_CHECK( esp_now_register_recv_cb(espnow_recv_cb) );

  // Receiver is always-on by default and only powers down its radio when told
  // to via SLEEP. Nothing to configure at boot.

  // Loop
  while (1) {
    // On-board button: cycle camId 1..10 and persist to NVS.
    if (button_pressed()) {
      cycle_camid();
    }
    // Execute latched ESP-NOW work here in app_main context: rendering and
    // NVS writes must not happen in the WiFi-task receive callback.
    rx_work_t w;
    if (take_rx_work(&w)) {
      handle_rx_work(&w);
    }
    // Explicit sleep: the Controller sent SLEEP(ms). app_main is the only place
    // we may block, so consume the request here (the recv cb just latches it).
    if (sleep_request_ms > 0) {
      uint16_t dur = sleep_request_ms;
      sleep_request_ms = 0;
      receiver_sleep(dur);
      lastMessageReceived = millis();   // just woke from a controller-ordered sleep
      continue;
    }
    // Heartbeat disabled to cut TX activity / heat. The controller re-sends
    // SET_TALLY on every change, so receiver presence detection is optional.
    // Re-enable with sendHeartbeat(); here if discovery is needed.
    delay(50);
    if (millis() - lastMessageReceived > 5000) {
      fillColorDirect(BACKGROUND_COLOR);
      // Paint one pixel red to signify we haven't received message
      setPixelColor(millis()%LED_COUNT, 128, 0, 0);
      show();
    }
    // TODO: read Serial.read();
  }
  nvs_close(nvs_tally_handle);
}

/*
Arduino:
RAM:   [==        ]  16.8% (used 55000 bytes from 327680 bytes)
Flash: [=====     ]  48.8% (used 702901 bytes from 1441792 bytes)
Building .pio/build/esp32s3-matrix/firmware.bin

ESP IDF:
RAM:   [=         ]   9.9% (used 32560 bytes from 327680 bytes)
Flash: [=======   ]  70.8% (used 742185 bytes from 1048576 bytes)

*/
