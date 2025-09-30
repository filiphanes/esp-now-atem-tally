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

static const char *TAG = "tally";
#define DEBUG 1
#undef DEBUG

#define TALLY_COUNT 64  // number of tally lights (max 64)
#define TALLY_UPDATE_EACH 60000 // 1 minute;
#define CONFIG_ESPNOW_CHANNEL 1
#define DEFAULT_CAMID 3
#define DEFAULT_CAMGROUP 0

#define RMT_LED_STRIP_RESOLUTION_HZ 10000000 // 10MHz resolution, 1 tick = 0.1us (led strip needs a high resolution)
#ifdef CONFIG_IDF_TARGET_ESP32S3
 #define RMT_LED_STRIP_GPIO_NUM  14
 #define LED_COUNT  64
 #define DEFAULT_BRIGHTNESS 2  // 0-255
#else //elifdef CONFIG_IDF_TARGET_ESP32C3
 #define RMT_LED_STRIP_GPIO_NUM  8
 #define LED_COUNT  25
 #define DEFAULT_BRIGHTNESS 10  // 0-255
#endif

static uint8_t led_strip_pixels[LED_COUNT * 3];
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
  vTaskDelay(pdMS_TO_TICKS(2000));
}

void show() {
  ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config));
}

void setBrightness(uint8_t y) {
  bright_ratio = 128/y;
}

void setPixelColor(int i, uint8_t r, uint8_t g, uint8_t b) {
#ifdef CONFIG_IDF_TARGET_ESP32S3
  led_strip_pixels[i * 3 + 0] = r/bright_ratio;
  led_strip_pixels[i * 3 + 1] = g/bright_ratio;
#else //ifdef CONFIG_IDF_TARGET_ESP32C3
  led_strip_pixels[i * 3 + 0] = g/bright_ratio;
  led_strip_pixels[i * 3 + 1] = r/bright_ratio;
#endif
  led_strip_pixels[i * 3 + 2] = b/bright_ratio;
}

void colorBlack() {
  memset(led_strip_pixels, 0, sizeof(led_strip_pixels));
}

void colorBlink(uint8_t r, uint8_t g, uint8_t b, int wait) {
  int count = 3;
  uint16_t i;
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
  for(int i=0; i<LED_COUNT; i++) { // For each pixel in strip...
    setPixelColor(i, r, g, b);       // Set pixel's color (in RAM)
    show();                          // Update strip to match
    delay(wait);                           // Pause for a moment
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
  esp_err_t err = nvs_get_u8(nvs_tally_handle, "camGroup", &camId);
  switch (err) {
      case ESP_OK:
          break;
      case ESP_ERR_NVS_NOT_FOUND:
          camId = DEFAULT_CAMGROUP;
          err = nvs_set_u8(nvs_tally_handle, "camGroup", camId);
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

void writeCamId() {
  if (nvs_set_u8(nvs_tally_handle, "camId", camId) != ESP_OK)
    ESP_LOGI(TAG, "writeCamId failed!");
}

void writeCamGroup() {
  if (nvs_set_u8(nvs_tally_handle, "camGroup", camGroup) != ESP_OK)
    ESP_LOGI(TAG, "writeCamGroup failed!");
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
      setPixelColor(i, 255, 255, 255);
      number -= 10;
    }
  }
  show();
}

void displaySignal(uint8_t signal) {
  displayDigit(0, 0, 255, signal);
}

void fillColor(uint8_t r, uint8_t g, uint8_t b) {
  for (int i=0; i<LED_COUNT; i++) {
    setPixelColor(i, r, g, b);
  }
  show();
}

inline bool getBit(uint64_t bits, int i) {
  return bits & ((uint64_t)1 << i);
}

// Callback function that will be executed when data is received
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  espnow_command command = (espnow_command)data[0];
  ESP_LOGI(TAG, "<[%d] ", command);
  switch (command) {

  case SET_TALLY: {
    uint64_t *program_p = (uint64_t *)(data+1);
    uint64_t *preview_p = (uint64_t *)(data+1+sizeof(uint64_t));
    // uint8_t  *group_p   = (uint8_t *) (data+1+sizeof(uint64_t)+sizeof(uint64_t));
    // if (group_p <= data+len && *group_p != camGroup) return;
    fillColor(
      255*getBit(*program_p, camId-1),
      255*getBit(*preview_p, camId-1),
      0
    );
    lastMessageReceived = millis();
#ifdef DEBUG
    ESP_LOGI(TAG, "SET_TALLY");
    // for (int i=1; i<len; i++) ESP_LOGI(TAG, "%02x ", data[i]);
    // ESP_LOGI(TAG, );
    printf("Program ");
    for (int i=0; i<TALLY_COUNT; i++) printf("%d", getBit(*program_p, i)?1:0);
    printf("\n");
    printf("Preview ");
    for (int i=0; i<TALLY_COUNT; i++) printf("%d", getBit(*preview_p, i)?1:0);
    printf("\n");
#endif
    break;
  }
    
  case HEARTBEAT:
    break;

  case SET_COLOR: {
    uint64_t *bits_p = (uint64_t *)(data+4);
    ESP_LOGI(TAG, "SET_COLOR #%02x%02x%02x\n", data[2], data[3], data[4]);
    if (getBit(*bits_p, camId-1)) {
      fillColor(data[1], data[2], data[3]);
    }
    lastMessageReceived = millis();
    break;
  }
  
  case SHOW_SIGNAL: {
    uint64_t *bits_p = (uint64_t *)(data+2);
    uint8_t signal = data[1];
    if (getBit(*bits_p, camId-1)) {
      displaySignal(signal);
    }
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
    uint64_t *bits_p = (uint64_t *)(data+1);
    ESP_LOGI(TAG, "SIGNAL %u %llu\n", command, *bits_p);
    if (getBit(*bits_p, camId-1)) displaySignal(command);
    lastMessageReceived = millis();
    break;
  }

  case SET_BRIGHTNESS: {
    uint64_t *bits_p = (uint64_t *)(data+2);
    uint64_t brightness = data[1];
    if (getBit(*bits_p, camId-1)) {
      setBrightness(brightness);
    }
    lastMessageReceived = millis();
    break;
  }

  case SET_CAMID: {
    uint64_t *bits_p = (uint64_t *)(data+2);
    if (getBit(*bits_p, camId-1)) {
      camId = data[1];
      writeCamId();
      displayNumber(0, 0, 255, camId);
      ESP_LOGI(TAG, "SET_CAMID %d\n", camId);
      delay(1000);  // so new number is visible
    }
    /*
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (memcmp(mac, (uint8_t*) (data + sizeof(espnow_command)), 6) == 0) {
      camId = data[7];
      ESP_LOGI(TAG, "SET_CAMID %d\n", camId);
    }
    */
    lastMessageReceived = millis();
    break;
  }

  case SET_CAMGROUP: {
    uint64_t *bits_p = (uint64_t *)(data+2);
    if (getBit(*bits_p, camId-1)) {
      camGroup = data[1];
      writeCamGroup();
      displayNumber(0, 255, 0, camGroup);
      ESP_LOGI(TAG, "SET_CAMGRUOP %d\n", camGroup);
      delay(1000);  // so new number is visible
    }
    /*
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (memcmp(mac, (uint8_t*) (data + sizeof(espnow_command)), 6) == 0) {
      camId = data[7];
      ESP_LOGI(TAG, "SET_CAMID %d\n", camId);
    }
    */
    lastMessageReceived = millis();
    break;
  }

  case SWITCH_CAMID: {
    uint8_t* id1 = (uint8_t*) (data + sizeof(espnow_command));
    uint8_t* id2 = (uint8_t*) (data + sizeof(espnow_command) + sizeof(uint8_t));
    ESP_LOGI(TAG, "SWITCH_CAMID %u<>%u", *id1, *id2);
    if (camId == *id1) {
      camId = *id2;
      writeCamId();
    } else if (camId == *id2) {
      camId = *id1;
      writeCamId();
    }
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
    ESP_ERROR_CHECK( esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N|WIFI_PROTOCOL_LR) );
}

void app_main() {
  esp_err_t err;
  ESP_LOGI(TAG, "rmt_new_tx_channel");
  ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));
  ESP_LOGI(TAG, "rmt_new_led_strip_encoder");
  ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));
  ESP_LOGI(TAG, "rmt_enable");
  ESP_ERROR_CHECK(rmt_enable(led_chan));
  // testDigits();

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
  
  // Add Broadcast Peer
  esp_now_peer_info_t peer;
  peer.channel = 0;
  // peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  // memcpy(&peer.lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
  memcpy(peer.peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
  err = esp_now_add_peer(&peer);
  if (unlikely(err != ESP_OK)) {
    ESP_LOGI(TAG, "esp_now_add_peer returned 0x%x: %s\n", err, esp_err_to_name(err));
  }
  // ESP_ERROR_CHECK( esp_now_register_send_cb(espnow_send_cb) );
  ESP_ERROR_CHECK( esp_now_register_recv_cb(espnow_recv_cb) );

  // Loop
  while (1) {
    sendHeartbeat();
    delay(2000);
    if (millis() - lastMessageReceived > 5000) {
      fillColor(0, 0, 0);
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