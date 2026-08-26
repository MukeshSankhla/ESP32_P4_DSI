#include "SD_MMC.h"
#include "driver/jpeg_decode.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <Preferences.h>
#include <Wire.h>
#include <vector>

// ===============================================================
// Hardware Pin Configuration
// ===============================================================
#define LCD_RST_PIN 23   // Reset GPIO for Waveshare/DFRobot DSI LCD
#define BACKLIGHT_PIN 26 // Fallback GPIO backlight pin
#define I2C_SDA_PIN 7    // SDA for I2C backlight controller
#define I2C_SCL_PIN 8    // SCL for I2C backlight controller
#define BACKLIGHT_I2C_ADDR 0x45
#define BACKLIGHT_REG_BRIGHTNESS 0x86
#define LCD_BRIGHTNESS_PCT 40 // LCD Backlight Brightness Percentage (0-100)

// SD Card Pin Configuration for DFRobot FireBeetle 2 ESP32-P4
#define SD_MMC_CLK 43
#define SD_MMC_CMD 44
#define SD_MMC_D0 39
#define SD_MMC_D1 40
#define SD_MMC_D2 41
#define SD_MMC_D3 42

// Button
#define BUTTON_PIN 32
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_DOUBLE_MS 350
#define BUTTON_LONG_PRESS_MS 2000
#define BUTTON_POLL_MS 5

// ===============================================================
// Global Playback & Mode State
// ===============================================================
volatile uint8_t current_animation = 1;
const uint8_t MAX_ANIMATIONS = 45;
volatile bool Vertical = true; // true for Vertical (/Vertical), false for Horizontal (/Horizontal)

std::vector<String> g_playlist;

// Target playback frame rate (e.g. 24 or 30)
const uint8_t TARGET_FPS = 24;

// JPEG frame buffer parameters
#define JPEG_BUF_SIZE (256 * 1024) // 256 KB buffer for compressed JPEG frame

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
uint8_t *jpeg_frame_buf = NULL;
uint16_t *jpeg_output_buf = NULL;

// ===============================================================
// BUTTON ACTION QUEUE
// Production event queue produced by button_task (Core 0), consumed by video loop (Core 1)
// ===============================================================
enum ButtonAction : uint8_t {
  BTN_ACTION_SINGLE = 1, // Next background animation, saved to NVS
  BTN_ACTION_DOUBLE = 2  // Toggle Vertical mode (Vertical <-> Horizontal), saved to NVS
};
QueueHandle_t button_action_queue = NULL;

// -------------------------------------------------------------
// DYNAMIC SD PLAYLIST SCANNER
// -------------------------------------------------------------
void scan_active_folder() {
  g_playlist.clear();
  g_playlist.reserve(64); // Pre-allocate vector capacity to prevent heap fragmentation
  const char *dirPath = Vertical ? "/Vertical" : "/Horizontal";
  File root = SD_MMC.open(dirPath);
  if (!root || !root.isDirectory()) {
    root = SD_MMC.open("/");
  }
  if (root) {
    File file = root.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        const char *fname = file.name();
        size_t len = strlen(fname);
        if (len >= 6 && (strcasecmp(fname + len - 6, ".mjpeg") == 0)) {
          if (fname[0] != '/') {
            char fullpath[128];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", dirPath, fname);
            g_playlist.push_back(String(fullpath));
          } else {
            g_playlist.push_back(String(fname));
          }
        }
      }
      file = root.openNextFile();
    }
    root.close();
  }
}

// -------------------------------------------------------------
// SCREEN FILL PRIMITIVE
// Fast 16-bit color fill with memset fast-path for black (0x0000)
// -------------------------------------------------------------
void clear_screen(uint16_t color) {
  const size_t fb_pixels = 480 * 1920;
  for (int f = 0; f < 2; f++) {
    if (display_fbs[f] != NULL) {
      if (color == 0x0000) {
        memset(display_fbs[f], 0, fb_pixels * sizeof(uint16_t));
      } else {
        uint16_t *fb = display_fbs[f];
        for (size_t i = 0; i < fb_pixels; i++) {
          fb[i] = color;
        }
      }
    }
  }
}

// -------------------------------------------------------------
// BUTTON TASK (Core 0)
// Dedicated non-blocking FreeRTOS state machine for instant multi-click & long-press detection
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

    // Long press fires immediately once held past threshold (2000ms)
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

// Advances + persists current_animation
bool perform_single_click_action() {
  current_animation++;
  if (current_animation > MAX_ANIMATIONS)
    current_animation = 1;

  Preferences prefs;
  prefs.begin("anim_prefs", false);
  prefs.putInt("last_anim", current_animation);
  prefs.end();

  return true;
}

// Toggles + persists Vertical mode between /Vertical & /Horizontal
bool perform_double_click_action() {
  Vertical = !Vertical;
  scan_active_folder();

  Preferences prefs;
  prefs.begin("anim_prefs", false);
  prefs.putBool("is_vertical", Vertical);
  prefs.end();

  // Green flash feedback for mode toggle
  if (display_fbs[0] != NULL) {
    clear_screen(0x07E0); // Green color in RGB565
    delay(100);
  }
  return true;
}

// Drains pending button actions. Returns true if current clip should abort.
bool process_button_queue() {
  ButtonAction act;
  bool should_break = false;
  while (button_action_queue != NULL &&
         xQueueReceive(button_action_queue, &act, 0) == pdTRUE) {
    if (act == BTN_ACTION_SINGLE) {
      should_break = perform_single_click_action() || should_break;
    } else if (act == BTN_ACTION_DOUBLE) {
      should_break = perform_double_click_action() || should_break;
    }
  }
  return should_break;
}

// Precise frame pacing with non-blocking button queue checks
bool paced_wait_and_check_button(unsigned long target_time_us) {
  bool should_break = false;
  while (true) {
    unsigned long now_us = micros();
    if (now_us >= target_time_us)
      break;
    unsigned long remaining_us = target_time_us - now_us;
    unsigned long chunk_us = (remaining_us > 2000) ? 2000 : remaining_us;
    delayMicroseconds(chunk_us);
    if (process_button_queue())
      should_break = true;
  }
  return should_break;
}

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

bool init_sd_card() {
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0, SD_MMC_D1, SD_MMC_D2,
                 SD_MMC_D3);

  if (SD_MMC.begin("/sdcard", false, false, 20000)) {
    return true;
  }
  SD_MMC.end();
  delay(100);
  if (SD_MMC.begin("/sdcard", false)) {
    return true;
  }
  SD_MMC.end();
  delay(100);
  if (SD_MMC.begin("/sdcard", true)) {
    return true;
  }
  SD_MMC.end();
  return false;
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
// PLAY MJPEG VIDEO PIPELINE
// -------------------------------------------------------------
void play_mjpeg_video(const char *path) {
  static int missing_count = 0;
  File videoFile;

  // Attempt up to 3 retries with 100ms pauses for cold SD card stabilization
  for (int retry = 0; retry < 3; retry++) {
    videoFile = SD_MMC.open(path, FILE_READ);
    if (videoFile)
      break;

    char alt_path[64];
    strncpy(alt_path, path, sizeof(alt_path) - 1);
    alt_path[sizeof(alt_path) - 1] = '\0';
    char *last_slash = strrchr(alt_path, '/');
    if (last_slash && *(last_slash + 1) != '\0') {
      *(last_slash + 1) = toupper((unsigned char)*(last_slash + 1));
    }
    videoFile = SD_MMC.open(alt_path, FILE_READ);
    if (videoFile)
      break;

    delay(100);
  }

  if (!videoFile) {
    current_animation++;
    if (current_animation > MAX_ANIMATIONS)
      current_animation = 1;

    missing_count++;
    if (missing_count > 5) {
      current_animation = 1;
      missing_count = 0;
      delay(500);
    }
    return;
  }
  missing_count = 0;

  const int BLOCK_SIZE = 4096;
  static uint8_t block_buf[BLOCK_SIZE];

  bool inside_frame = false;
  uint32_t write_pos = 0;
  bool exit_video = false;

  static const unsigned long target_frame_time_us = 1000000UL / TARGET_FPS;
  unsigned long next_frame_time_us = micros();

  while (!exit_video && videoFile.available()) {
    int bytes_read = videoFile.read(block_buf, BLOCK_SIZE);
    if (bytes_read <= 0)
      break;

    if (process_button_queue()) {
      exit_video = true;
      break;
    }

    for (int i = 0; i < bytes_read; i++) {
      uint8_t b = block_buf[i];

      if (!inside_frame) {
        if (b == 0xD8 && write_pos > 0 &&
            jpeg_frame_buf[write_pos - 1] == 0xFF) {
          jpeg_frame_buf[0] = 0xFF;
          jpeg_frame_buf[1] = 0xD8;
          inside_frame = true;
          write_pos = 2;
        } else {
          jpeg_frame_buf[0] = b;
          write_pos = 1;
        }
      } else {
        if (write_pos < JPEG_BUF_SIZE) {
          jpeg_frame_buf[write_pos++] = b;
        } else {
          inside_frame = false;
          write_pos = 0;
          continue;
        }

        if (b == 0xD9 && write_pos >= 2 &&
            jpeg_frame_buf[write_pos - 2] == 0xFF) {
          uint32_t decoded_size = 0;
          draw_fb = display_fbs[1 - current_fb_idx];

          if (draw_fb != NULL && jpeg_output_buf != NULL) {
            esp_err_t dec_err = jpeg_decoder_process(
                jpeg_decoder, &jpeg_decode_cfg, jpeg_frame_buf, write_pos,
                (uint8_t *)jpeg_output_buf, 480 * 1920 * 2, &decoded_size);

            if (dec_err == ESP_OK) {
              if (Vertical) {
                // Vertical mode: 480x1920 portrait MJPEG direct copy
                memcpy(draw_fb, jpeg_output_buf, 480 * 1920 * sizeof(uint16_t));
              } else {
                // Horizontal mode: 1920x480 landscape MJPEG 90° rotation onto 480x1920 physical panel
                for (int sx = 0; sx < 1920; sx++) {
                  uint16_t *dst_col = &draw_fb[sx * 480];
                  for (int sy = 0; sy < 480; sy++) {
                    dst_col[479 - sy] = jpeg_output_buf[sy * 1920 + sx];
                  }
                }
              }

              esp_lcd_panel_draw_bitmap(dpi_panel, 0, 0, 480 - 1, 1920 - 1,
                                        draw_fb);
              current_fb_idx = 1 - current_fb_idx;

              next_frame_time_us += target_frame_time_us;
              if (paced_wait_and_check_button(next_frame_time_us)) {
                exit_video = true;
              }
            }
          }

          inside_frame = false;
          write_pos = 0;

          if (exit_video)
            break;
        }
      }
    }
  }

  videoFile.close();
}

// -------------------------------------------------------------
// ARDUINO SETUP & MAIN LOOP
// -------------------------------------------------------------
void setup() {
  button_action_queue = xQueueCreate(4, sizeof(ButtonAction));

  jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
      .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
  };
  size_t allocated_in_size = 0;
  jpeg_frame_buf = (uint8_t *)jpeg_alloc_decoder_mem(JPEG_BUF_SIZE, &tx_mem_cfg,
                                                     &allocated_in_size);
  if (jpeg_frame_buf == NULL) {
    while (1) ;
  }

  jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {
      .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
  };
  size_t allocated_out_size = 0;
  jpeg_output_buf = (uint16_t *)jpeg_alloc_decoder_mem(
      480 * 1920 * sizeof(uint16_t), &rx_mem_cfg, &allocated_out_size);
  if (jpeg_output_buf == NULL) {
    while (1) ;
  }

  bool sd_ok = false;
  int sd_retries = 0;
  while (sd_retries < 5) {
    if (init_sd_card()) {
      sd_ok = true;
      break;
    }
    sd_retries++;
    delay(500);
  }
  if (!sd_ok) {
    while (1) ;
  }

  delay(1000); // SD card power-up stabilization

  init_jpeg_decoder();
  reset_display();
  delay(200);

  init_mipi_ldo();
  init_backlight();
  init_dsi_display();
  init_framebuffer();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Preferences prefs;
  prefs.begin("anim_prefs", false);
  current_animation = prefs.getInt("last_anim", 1);
  if (current_animation < 1 || current_animation > MAX_ANIMATIONS)
    current_animation = 1;
  Vertical = prefs.getBool("is_vertical", true);
  prefs.end();

  if (display_fbs[0] != NULL && display_fbs[1] != NULL) {
    clear_screen(0x0000);
    xTaskCreatePinnedToCore(button_task, "button_task", 3072, NULL, 3, NULL, 0);
  } else {
    while (1) ;
  }
}

void loop() {
  if (display_fbs[0] != NULL && display_fbs[1] != NULL) {
    if (g_playlist.empty()) {
      scan_active_folder();
      if (g_playlist.empty()) {
        delay(1000);
        init_sd_card();
        return;
      }
    }

    if (current_animation < 1 || current_animation > (int)g_playlist.size()) {
      current_animation = 1;
    }

    play_mjpeg_video(g_playlist[current_animation - 1].c_str());

    // Yield small delay between clips for SD_MMC handle cleanup
    vTaskDelay(pdMS_TO_TICKS(20));
  } else {
    delay(1000);
  }
}
