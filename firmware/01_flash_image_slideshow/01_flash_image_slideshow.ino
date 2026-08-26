#include "image_data.h"
#include "driver/jpeg_decode.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <Wire.h>

// ===============================================================
// Hardware Pin Configuration
// ===============================================================
#define LCD_RST_PIN 23   // Reset GPIO for Waveshare/DFRobot DSI LCD
#define BACKLIGHT_PIN 26 // Fallback GPIO backlight pin
#define I2C_SDA_PIN 7    // SDA for I2C backlight controller
#define I2C_SCL_PIN 8    // SCL for I2C backlight controller
#define BACKLIGHT_I2C_ADDR 0x45
#define BACKLIGHT_REG_BRIGHTNESS 0x86
#define LCD_BRIGHTNESS_PCT 60 // LCD Backlight Brightness Percentage (0-100)

// Button Pin Configuration
#define BUTTON_PIN 32
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_DOUBLE_MS 350
#define BUTTON_LONG_PRESS_MS 2000
#define BUTTON_POLL_MS 5

// ===============================================================
// Hardware Handles
// ===============================================================
esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
esp_lcd_panel_handle_t dpi_panel = NULL;
esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
uint16_t *display_fbs[2] = {NULL, NULL};
uint16_t *draw_fb = NULL;
uint8_t current_fb_idx = 0;

jpeg_decoder_handle_t jpeg_decoder = NULL;
jpeg_decode_cfg_t jpeg_decode_cfg;
uint16_t *jpeg_output_buf = NULL;
uint8_t *jpeg_input_buf = NULL;
uint32_t allocated_in_size = 0;

// ===============================================================
// Playback and Button Action Queue
// ===============================================================
enum ButtonAction : uint8_t {
  BTN_ACTION_SINGLE = 1, // Advance to next image
  BTN_ACTION_DOUBLE = 2  // Toggle Autoplay mode
};
QueueHandle_t button_action_queue = NULL;

int current_image_idx = 0;
bool autoplay_enabled = false;
unsigned long last_autoplay_ms = 0;

// -------------------------------------------------------------
// HARDWARE INITIALIZATION
// -------------------------------------------------------------
void reset_display() {
  pinMode(LCD_RST_PIN, OUTPUT);
  digitalWrite(LCD_RST_PIN, LOW);
  delay(50);
  digitalWrite(LCD_RST_PIN, HIGH);
  delay(120);
}

void init_mipi_ldo() {
  esp_ldo_channel_config_t ldo_cfg;
  memset(&ldo_cfg, 0, sizeof(ldo_cfg));
  ldo_cfg.chan_id = 3;
  ldo_cfg.voltage_mv = 2500;
  esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy);
}

void init_backlight() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  uint8_t brightness_val = (LCD_BRIGHTNESS_PCT * 255) / 100;
  bool i2c_success = false;

  // 1. Standard Waveshare backlight MCU (0x45)
  Wire.beginTransmission(BACKLIGHT_I2C_ADDR);
  Wire.write(BACKLIGHT_REG_BRIGHTNESS);
  Wire.write(brightness_val);
  if (Wire.endTransmission() == 0) i2c_success = true;

  Wire.beginTransmission(BACKLIGHT_I2C_ADDR);
  Wire.write(0x96);
  Wire.write(brightness_val);
  if (Wire.endTransmission() == 0) i2c_success = true;

  // 2. Alternate address (0x1E)
  Wire.beginTransmission(0x1E);
  Wire.write(0x86);
  Wire.write(brightness_val);
  if (Wire.endTransmission() == 0) i2c_success = true;

  // 3. Fallback GPIO PWM
  if (!i2c_success) {
    ledcAttach(BACKLIGHT_PIN, 5000, 8);
    ledcWrite(BACKLIGHT_PIN, brightness_val);
  }
}

void init_dsi_display() {
  esp_lcd_dsi_bus_config_t bus_config;
  memset(&bus_config, 0, sizeof(bus_config));
  bus_config.bus_id = 0;
  bus_config.num_data_lanes = 2;
  bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
  bus_config.lane_bit_rate_mbps = 1000;
  esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);

  esp_lcd_dpi_panel_config_t dpi_config;
  memset(&dpi_config, 0, sizeof(dpi_config));
  dpi_config.virtual_channel = 0;
  dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dpi_config.dpi_clock_freq_mhz = 60;
  dpi_config.in_color_format = LCD_COLOR_FMT_RGB565;

  dpi_config.video_timing.h_size = 480;
  dpi_config.video_timing.v_size = 1920;
  dpi_config.video_timing.hsync_pulse_width = 30;
  dpi_config.video_timing.hsync_back_porch = 30;
  dpi_config.video_timing.hsync_front_porch = 30;
  dpi_config.video_timing.vsync_pulse_width = 6;
  dpi_config.video_timing.vsync_back_porch = 6;
  dpi_config.video_timing.vsync_front_porch = 6;

  dpi_config.num_fbs = 2; // hardware double-buffering
  dpi_config.flags.use_dma2d = true;

  esp_lcd_new_panel_dpi(mipi_dsi_bus, &dpi_config, &dpi_panel);
  esp_lcd_panel_init(dpi_panel);
}

void init_framebuffer() {
  esp_lcd_dpi_panel_get_frame_buffer(
      dpi_panel, 2, (void **)&display_fbs[0], (void **)&display_fbs[1]);
}

void init_jpeg_decoder() {
  jpeg_decode_engine_cfg_t decode_eng_cfg;
  memset(&decode_eng_cfg, 0, sizeof(decode_eng_cfg));
  decode_eng_cfg.intr_priority = 0;
  decode_eng_cfg.timeout_ms = 1000;
  jpeg_new_decoder_engine(&decode_eng_cfg, &jpeg_decoder);

  memset(&jpeg_decode_cfg, 0, sizeof(jpeg_decode_cfg));
  jpeg_decode_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  jpeg_decode_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
  jpeg_decode_cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
}

// -------------------------------------------------------------
// BUTTON TASK (Core 0)
// Dedicated non-blocking FreeRTOS state machine for click detection
// -------------------------------------------------------------
void button_task(void *pvParameters) {
  bool stable_pressed = false;
  bool last_raw_pressed = false;
  unsigned long last_change_ms = 0;
  unsigned long press_start_ms = 0;
  unsigned long release_ms = 0;
  int click_count = 0;
  bool long_press_fired = false;

  for (;;) {
    bool raw_pressed = (digitalRead(BUTTON_PIN) == LOW);
    unsigned long now = millis();

    if (raw_pressed != last_raw_pressed) {
      last_change_ms = now;
      last_raw_pressed = raw_pressed;
    }

    if ((now - last_change_ms) >= BUTTON_DEBOUNCE_MS &&
        raw_pressed != stable_pressed) {
      stable_pressed = raw_pressed;

      if (stable_pressed) {
        press_start_ms = now;
        long_press_fired = false;
        click_count++;
      } else {
        if (!long_press_fired) {
          release_ms = now;
        }
      }
    }

    // Long press fires immediately once held past threshold (2000ms) - triggers reset
    if (stable_pressed && !long_press_fired &&
        (now - press_start_ms) >= BUTTON_LONG_PRESS_MS) {
      long_press_fired = true;
      click_count = 0;
      vTaskDelay(pdMS_TO_TICKS(50));
      ESP.restart();
    }

    // Resolve click sequence once multi-click window closes
    if (click_count > 0 && !stable_pressed &&
        (now - release_ms) > BUTTON_DOUBLE_MS) {
      ButtonAction act;
      if (click_count == 1) {
        act = BTN_ACTION_SINGLE;
      } else {
        act = BTN_ACTION_DOUBLE;
      }
      if (button_action_queue) {
        xQueueSend(button_action_queue, &act, 0);
      }
      click_count = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
  }
}

// -------------------------------------------------------------
// IMAGE RENDERING FUNCTIONS
// -------------------------------------------------------------
void show_image(int index) {
  if (index < 0 || index >= NUM_IMAGES) return;

  uint32_t len = image_jpg_lens[index];
  const uint8_t *ptr = image_jpg_ptrs[index];

  if (len > allocated_in_size) {
    Serial.printf("Error: Image size %u exceeds allocated buffer size %u!\n", len, allocated_in_size);
    return;
  }

  // Copy JPEG bytes from flash (PROGMEM) to the DMA-accessible PSRAM buffer
  memcpy(jpeg_input_buf, ptr, len);

  // Decode the embedded JPEG image
  uint32_t decoded_size = 0;
  esp_err_t dec_err = jpeg_decoder_process(
      jpeg_decoder, &jpeg_decode_cfg, jpeg_input_buf, len,
      (uint8_t *)jpeg_output_buf, 480 * 1920 * sizeof(uint16_t), &decoded_size);

  if (dec_err != ESP_OK) {
    Serial.printf("JPEG decode failed with error code: 0x%X\n", dec_err);
    return;
  }
  Serial.printf("JPEG decoded successfully. Size: %u bytes\n", decoded_size);

  // Rotate the 1920x480 landscape image by 90 degrees onto the 480x1920 portrait panel
  // Map display_fbs[0] and display_fbs[1] so both buffers show the same image
  for (int f = 0; f < 2; f++) {
    draw_fb = display_fbs[f];
    for (int sx = 0; sx < 1920; sx++) {
      uint16_t *dst_col = &draw_fb[sx * 480];
      for (int sy = 0; sy < 480; sy++) {
        dst_col[479 - sy] = jpeg_output_buf[sy * 1920 + sx];
      }
    }
  }

  // Draw bitmap to display panel
  esp_lcd_panel_draw_bitmap(dpi_panel, 0, 0, 480 - 1, 1920 - 1, display_fbs[0]);
}

// -------------------------------------------------------------
// ARDUINO SETUP & LOOP
// -------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("ESP32-P4 DSI Image Display Starting...");

  // Setup button PIN
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Create queue and spawn button listener task
  button_action_queue = xQueueCreate(4, sizeof(ButtonAction));
  if (button_action_queue == NULL) {
    Serial.println("Failed to create button action queue!");
  } else {
    xTaskCreatePinnedToCore(button_task, "button_task", 3072, NULL, 3, NULL, 0);
  }

  // Calculate the maximum image size across all arrays
  uint32_t max_image_len = 0;
  for (int i = 0; i < NUM_IMAGES; i++) {
    if (image_jpg_lens[i] > max_image_len) {
      max_image_len = image_jpg_lens[i];
    }
  }
  Serial.printf("Number of images: %d. Maximum JPEG size: %u bytes\n", NUM_IMAGES, max_image_len);

  // Allocate memory for JPEG input buffer (must support DMA)
  jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
      .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
  };
  size_t actual_allocated_in_size = 0;
  jpeg_input_buf = (uint8_t *)jpeg_alloc_decoder_mem(
      max_image_len, &tx_mem_cfg, &actual_allocated_in_size);
  if (jpeg_input_buf == NULL) {
    Serial.println("Failed to allocate JPEG input buffer!");
    while (1) delay(1000);
  }
  allocated_in_size = actual_allocated_in_size;
  Serial.printf("Allocated JPEG input buffer: %u bytes (requested %u)\n", allocated_in_size, max_image_len);

  // Allocate memory for JPEG output buffer (1920 * 480 * 2 bytes)
  jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {
      .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
  };
  size_t allocated_out_size = 0;
  jpeg_output_buf = (uint16_t *)jpeg_alloc_decoder_mem(
      480 * 1920 * sizeof(uint16_t), &rx_mem_cfg, &allocated_out_size);
  if (jpeg_output_buf == NULL) {
    Serial.println("Failed to allocate JPEG output buffer in PSRAM!");
    while (1) delay(1000);
  }

  // Initialize hardware components
  init_jpeg_decoder();
  reset_display();
  delay(200);

  init_mipi_ldo();
  init_backlight();
  init_dsi_display();
  init_framebuffer();

  if (display_fbs[0] == NULL || display_fbs[1] == NULL) {
    Serial.println("Failed to retrieve display framebuffers!");
    while (1) delay(1000);
  }

  Serial.println("Hardware initialized successfully.");

  // Decode and draw the first image
  show_image(current_image_idx);
  last_autoplay_ms = millis();
}

void loop() {
  // Check for button actions from queue
  ButtonAction act;
  if (button_action_queue != NULL &&
      xQueueReceive(button_action_queue, &act, 0) == pdTRUE) {
    if (act == BTN_ACTION_SINGLE) {
      current_image_idx = (current_image_idx + 1) % NUM_IMAGES;
      Serial.printf("Button single-press: showing image index %d (%d/%d)\n", current_image_idx, current_image_idx + 1, NUM_IMAGES);
      show_image(current_image_idx);
      last_autoplay_ms = millis(); // Reset autoplay timer on manual navigation
    } else if (act == BTN_ACTION_DOUBLE) {
      autoplay_enabled = !autoplay_enabled;
      Serial.printf("Button double-press: Autoplay mode %s\n", autoplay_enabled ? "ENABLED" : "DISABLED");
      last_autoplay_ms = millis(); // Reset autoplay timer
    }
  }

  // Handle Autoplay timer
  if (autoplay_enabled) {
    unsigned long now = millis();
    if (now - last_autoplay_ms >= 5000) {
      current_image_idx = (current_image_idx + 1) % NUM_IMAGES;
      Serial.printf("Autoplay: showing image index %d (%d/%d)\n", current_image_idx, current_image_idx + 1, NUM_IMAGES);
      show_image(current_image_idx);
      last_autoplay_ms = now;
    }
  }

  delay(20);
}
