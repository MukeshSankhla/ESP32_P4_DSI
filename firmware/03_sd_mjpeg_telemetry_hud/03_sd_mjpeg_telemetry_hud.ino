#include "SD_MMC.h"
#include "driver/jpeg_decode.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <ArduinoJson.h>
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
#define LCD_BRIGHTNESS_PCT 10 // LCD Backlight Brightness Percentage (0-100)

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

volatile uint8_t current_animation = 1;
const uint8_t MAX_ANIMATIONS = 45;
volatile bool Vertical = true; // true for Vertical, false for Horizontal

// ---------------------------------------------------------------
// LAYOUT INDEX (single click cycles background, double click cycles
// this). 15 total layouts, grouped by "family":
//   1  CARDS            - premium 4-card grid (classic)
//   2  DIALS             - 3 large minimalist dials
//   3  SIDEBAR            - asymmetric sidebar + system-load panel
//   4  TOP BAR             - slim full-width status bar, top
//   5  BOTTOM BAR           - slim full-width status bar, bottom
//   6  CORNER MINIMAL        - pure sci-fi corner brackets, no fills
//   7  CONCENTRIC RINGS       - single focal 3-ring cluster
//   8  LEFT RAIL                - vertical stack, left edge only
//   9  RIGHT RAIL                - vertical stack, right edge only
//   10 DIAGONAL CASCADE          - asymmetric staggered card sizes
//   11 TARGETING HUD               - brackets + aggregate center ring
//   12 SPLIT BAR                    - top clock strip + twin dials
//   13 CIRCULAR CLUSTER               - tight arc of 3 small dials
//   14 TEXT ONLY MINIMAL                - no cards/gauges at all
//   15 ASK BAR                           - floating pill command bar
// All layouts are designed to keep the MJPEG background clearly
// visible - cards default to a light 25% glass tint (or fully
// transparent outlines) instead of a heavy 50% fill.
// ---------------------------------------------------------------
volatile bool use_custom_layout = false;
volatile bool in_live_view_mode = false;
volatile bool force_video_exit = false;
String live_view_bg = "";
StaticJsonDocument<2048> current_layout_doc;

volatile uint8_t current_layout = 1;
const uint8_t MAX_LAYOUTS = 15;

std::vector<String> g_playlist;

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
  Serial.printf("[SD_MMC] Scanned %s: Discovered %d MJPEG clips.\n", dirPath, (int)g_playlist.size());
}

// Target playback frame rate (e.g. 30 or 60)
const uint8_t TARGET_FPS = 24;

// JPEG frame buffer parameters
#define JPEG_BUF_SIZE (256 * 1024) // 256 KB buffer for compressed JPEG frame

// ===============================================================
// Global handles
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
// TELEMETRY DATA & MULTI-CORE SETUP
// ===============================================================
struct TelemetryData {
  float cpu_temp;
  float gpu_temp;
  uint8_t cpu_usage;
  uint8_t gpu_usage;
  uint8_t mem_usage;
  bool active;
  char date[16];
  char time[16];
};

TelemetryData g_telemetry = {0.0f, 0.0f, 0, 0, 0, false, "N/A", "N/A"};
SemaphoreHandle_t telemetry_mutex = NULL;
uint32_t last_telemetry_time = 0;

// ===============================================================
// BUTTON ACTION QUEUE (produced by button_task, consumed by video loop)
// ===============================================================
enum ButtonAction : uint8_t {
  BTN_ACTION_SINGLE = 1, // next background, saved to NVS
  BTN_ACTION_DOUBLE = 2, // next layout, saved to NVS
  BTN_ACTION_TRIPLE = 3  // toggle Vertical mode, saved to NVS
  // Long-press is handled entirely inside button_task (immediate restart).
};
QueueHandle_t button_action_queue = NULL;

// -------------------------------------------------------------
// CYBERPUNK 8x8 ASCII BITMAP FONT
// -------------------------------------------------------------
const uint8_t font8x8[97][8] PROGMEM = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // (space)
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00}, // !
    {0x6C, 0x6C, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00}, // "
    {0x6C, 0x6C, 0xFE, 0x6C, 0xFE, 0x6C, 0x6C, 0x00}, // #
    {0x18, 0x7C, 0xC0, 0x78, 0x0E, 0xF8, 0x18, 0x00}, // $
    {0x00, 0xC6, 0xCC, 0x18, 0x30, 0x66, 0xC6, 0x00}, // %
    {0x38, 0x6C, 0x38, 0x76, 0xDC, 0xCC, 0x76, 0x00}, // &
    {0x60, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00}, // '
    {0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00}, // (
    {0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00}, // )
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, // *
    {0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00}, // +
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30}, // ,
    {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00}, // -
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00}, // .
    {0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00}, // /
    {0x7C, 0xC6, 0xCE, 0xDE, 0xF6, 0xE6, 0x7C, 0x00}, // 0
    {0x30, 0x70, 0x30, 0x30, 0x30, 0x30, 0xFC, 0x00}, // 1
    {0x7E, 0xC6, 0x0C, 0x18, 0x30, 0x60, 0xFC, 0x00}, // 2
    {0x7E, 0xC6, 0x0C, 0x3C, 0x0C, 0xC6, 0x7E, 0x00}, // 3
    {0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x1E, 0x00}, // 4
    {0xFC, 0xC0, 0xF8, 0x0C, 0x0C, 0xC6, 0x7E, 0x00}, // 5
    {0x3C, 0x60, 0xC0, 0xF8, 0xC6, 0xC6, 0x7E, 0x00}, // 6
    {0xFE, 0xC6, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00}, // 7
    {0x7E, 0xC6, 0xC6, 0x7E, 0xC6, 0xC6, 0x7E, 0x00}, // 8
    {0x7E, 0xC6, 0xC6, 0x7E, 0x06, 0x0C, 0x78, 0x00}, // 9
    {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00}, // :
    {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x30}, // ;
    {0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x00}, // <
    {0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00}, // =
    {0x30, 0x18, 0x0C, 0x06, 0x0C, 0x18, 0x30, 0x00}, // >
    {0x7C, 0xC6, 0x0C, 0x18, 0x18, 0x00, 0x18, 0x00}, // ?
    {0x7E, 0xC6, 0xDE, 0xDE, 0xDE, 0xC0, 0x7E, 0x00}, // @
    {0x30, 0x78, 0xCC, 0xCC, 0xFC, 0xCC, 0xCC, 0x00}, // A
    {0xFC, 0x66, 0x66, 0x7C, 0x66, 0x66, 0xFC, 0x00}, // B
    {0x3C, 0x66, 0xC0, 0xC0, 0xC0, 0x66, 0x3C, 0x00}, // C
    {0xF8, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0xF8, 0x00}, // D
    {0xFE, 0x62, 0x68, 0x78, 0x68, 0x62, 0xFE, 0x00}, // E
    {0xFE, 0x62, 0x68, 0x78, 0x68, 0x60, 0xF0, 0x00}, // F
    {0x3C, 0x66, 0xC0, 0xC0, 0xCE, 0x66, 0x3E, 0x00}, // G
    {0xCC, 0xCC, 0xCC, 0xFC, 0xCC, 0xCC, 0xCC, 0x00}, // H
    {0x78, 0x30, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00}, // I
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0xCC, 0x78, 0x00}, // J
    {0xE6, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0xE6, 0x00}, // K
    {0xF0, 0x60, 0x60, 0x60, 0x60, 0x62, 0xFE, 0x00}, // L
    {0xC6, 0xEE, 0xFE, 0xFE, 0xD6, 0xC6, 0xC6, 0x00}, // M
    {0xC6, 0xE6, 0xF6, 0xDE, 0xCE, 0xC6, 0xC6, 0x00}, // N
    {0x38, 0x6C, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x00}, // O
    {0xFC, 0x66, 0x66, 0x7C, 0x60, 0x60, 0xF0, 0x00}, // P
    {0x78, 0xCC, 0xCC, 0xCC, 0xDC, 0x78, 0x1C, 0x00}, // Q
    {0xFC, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0xE6, 0x00}, // R
    {0x7E, 0xC2, 0xC0, 0x7E, 0x06, 0x86, 0x7C, 0x00}, // S
    {0xFC, 0xB4, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00}, // T
    {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x78, 0x00}, // U
    {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x78, 0x30, 0x00}, // V
    {0xC6, 0xC6, 0xC6, 0xD6, 0xFE, 0xEE, 0xC6, 0x00}, // W
    {0xC6, 0xC6, 0x6C, 0x38, 0x6C, 0xC6, 0xC6, 0x00}, // X
    {0xCC, 0xCC, 0xCC, 0x78, 0x30, 0x30, 0x78, 0x00}, // Y
    {0xFE, 0xC6, 0x8C, 0x18, 0x32, 0x66, 0xFE, 0x00}, // Z
    {0x78, 0x60, 0x60, 0x60, 0x60, 0x60, 0x78, 0x00}, // [
    {0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x02, 0x00}, // backslash
    {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00}, // ]
    {0x10, 0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00, 0x00}, // ^
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}, // _
    {0x30, 0x30, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, // `
    {0x00, 0x00, 0x78, 0x0C, 0x7C, 0xCC, 0x76, 0x00}, // a
    {0xE0, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x00}, // b
    {0x00, 0x00, 0x3C, 0x66, 0x60, 0x66, 0x3C, 0x00}, // c
    {0x1C, 0x0C, 0x7D, 0xCC, 0xCC, 0xCC, 0x76, 0x00}, // d
    {0x00, 0x00, 0x7E, 0xC6, 0xFC, 0xC0, 0x7E, 0x00}, // e
    {0x38, 0x6C, 0x60, 0xF0, 0x60, 0x60, 0xF0, 0x00}, // f
    {0x00, 0x00, 0x76, 0xCC, 0xCC, 0x7C, 0x0C, 0xF8}, // g
    {0xE0, 0x60, 0x6C, 0x76, 0x66, 0x66, 0xE6, 0x00}, // h
    {0x30, 0x00, 0x70, 0x30, 0x30, 0x30, 0x78, 0x00}, // i
    {0x0C, 0x00, 0x0C, 0x0C, 0x0C, 0xCC, 0x78, 0x00}, // j
    {0xE0, 0x60, 0x66, 0x6C, 0x78, 0x6C, 0xE6, 0x00}, // k
    {0x70, 0x30, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00}, // l
    {0x00, 0x00, 0xCC, 0xFE, 0xD6, 0xC6, 0xC6, 0x00}, // m
    {0x00, 0x00, 0xDC, 0x66, 0x66, 0x66, 0x66, 0x00}, // n
    {0x00, 0x00, 0x38, 0x6C, 0xC6, 0x6C, 0x38, 0x00}, // o
    {0x00, 0x00, 0xFC, 0x66, 0x66, 0x7C, 0x60, 0xF0}, // p
    {0x00, 0x00, 0x7E, 0xCC, 0xCC, 0x7C, 0x0C, 0x1E}, // q
    {0x00, 0x00, 0xDC, 0x66, 0x60, 0x60, 0xF0, 0x00}, // r
    {0x00, 0x00, 0x7E, 0xC0, 0x7C, 0x06, 0xFC, 0x00}, // s
    {0x30, 0x30, 0xFC, 0x30, 0x30, 0x34, 0x18, 0x00}, // t
    {0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0x76, 0x00}, // u
    {0x00, 0x00, 0xCC, 0xCC, 0xCC, 0x78, 0x30, 0x00}, // v
    {0x00, 0x00, 0xC6, 0xD6, 0xFE, 0xEE, 0xC6, 0x00}, // w
    {0x00, 0x00, 0xC6, 0x6C, 0x38, 0x6C, 0xC6, 0x00}, // x
    {0x00, 0x00, 0xCC, 0xCC, 0xCC, 0x7C, 0x0C, 0xF8}, // y
    {0x00, 0x00, 0xFE, 0x8C, 0x18, 0x32, 0xFE, 0x00}, // z
    {0x1C, 0x30, 0x30, 0xE0, 0x30, 0x30, 0x1C, 0x00}, // {
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, // |
    {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00}, // }
    {0x00, 0x00, 0x3C, 0x66, 0x00, 0x00, 0x00, 0x00}, // ~
    {0xFF, 0x81, 0xBD, 0xA5, 0xA5, 0xBD, 0x81,
     0xFF} // Degree symbol mapping / solid block
};

// ===============================================================
// RENDER PRIMITIVES FOR HUD DRAWING (landscape 1920x480, rotated onto
// a physical 480x1920 panel)
// ===============================================================

// Single source of truth for the rotation transform. Everything else
// (plain pixel writes, translucent "glass" blends) is built on top of
// this so the coordinate math only lives in one place.
inline bool mapLandscapeToPhysical(int vx, int vy, int &x, int &y) {
  if (Vertical) {
    x = vx;
    y = vy;
  } else {
    if (vx < 0 || vx >= 1920 || vy < 0 || vy >= 480)
      return false;
    y = vx;
    x = 479 - vy;
  }
  return (x >= 0 && x < 480 && y >= 0 && y < 1920);
}

inline void drawPixelLandscape(int vx, int vy, uint16_t color) {
  int x, y;
  if (!mapLandscapeToPhysical(vx, vy, x, y) || draw_fb == NULL)
    return;
  draw_fb[y * 480 + x] = color;
}

// Blends the existing pixel toward white (light_mode) or black (dark
// mode). `strength` controls how strong the tint is:
//   1 = 50% (heavy, legacy "frosted glass" look)
//   2 = 25% (light glass - DEFAULT, keeps the video background clearly
//       visible through cards/gauges)
//   3 = ~12.5% (barely-there tint, used for "ask bar" style pills)
//   4 = ~6% (near invisible, just enough to separate text from a busy
//       background)
// Cards, gauges and every new minimal layout now default to strength
// 2 instead of the old hard-coded 50% blend, so the MJPEG background
// stays visible everywhere instead of being washed out.
inline void blendPixelLandscape(int vx, int vy, bool light_mode, int strength = 2) {
  int x, y;
  if (!mapLandscapeToPhysical(vx, vy, x, y) || draw_fb == NULL)
    return;
  if (strength < 1)
    strength = 1;
  if (strength > 4)
    strength = 4;
  uint16_t c = draw_fb[y * 480 + x];
  uint16_t r_c = (c >> 11) & 0x1F;
  uint16_t g_c = (c >> 5) & 0x3F;
  uint16_t b_c = c & 0x1F;
  if (light_mode) {
    r_c = r_c + ((31 - r_c) >> strength);
    g_c = g_c + ((63 - g_c) >> strength);
    b_c = b_c + ((31 - b_c) >> strength);
  } else {
    r_c = r_c - (r_c >> strength);
    g_c = g_c - (g_c >> strength);
    b_c = b_c - (b_c >> strength);
  }
  draw_fb[y * 480 + x] = (r_c << 11) | (g_c << 5) | b_c;
}

void drawCharLandscape(int vx, int vy, char c, uint16_t color, int scale) {
  if (c < 32 || c > 127)
    return;
  int font_idx = c - 32;
  for (int row = 0; row < 8; row++) {
    uint8_t row_data = pgm_read_byte(&(font8x8[font_idx][row]));
    for (int col = 0; col < 8; col++) {
      if (row_data & (0x80 >> col)) {
        for (int sy = 0; sy < scale; sy++) {
          int py = vy + row * scale + sy;
          for (int sx = 0; sx < scale; sx++) {
            int px = vx + col * scale + sx;
            drawPixelLandscape(px, py, color);
          }
        }
      }
    }
  }
}

// -------------------------------------------------------------
// ITALIC TYPOGRAPHY ENGINE (Slanted dynamic rendering)
// -------------------------------------------------------------
void drawItalicStringLandscape(int x, int y, const char *str, uint16_t color,
                               int scale) {
  int cur_x = x + 2;
  const char *s = str;
  while (*s) {
    char c = *s;
    if (c >= 32 && c <= 127) {
      for (int i = 0; i < 8; i++) {
        uint8_t row_data = pgm_read_byte(&(font8x8[c - 32][i]));
        int shift = (7 - i) / 2 * scale;
        for (int j = 0; j < 8; j++) {
          if (row_data & (0x80 >> j)) {
            for (int sx = 0; sx < scale; sx++) {
              for (int sy = 0; sy < scale; sy++) {
                drawPixelLandscape(cur_x + j * scale + sx + shift,
                                   (y + 2) + i * scale + sy, 0x0000);
              }
            }
          }
        }
      }
    }
    cur_x += 8 * scale;
    s++;
  }

  cur_x = x;
  s = str;
  while (*s) {
    char c = *s;
    if (c >= 32 && c <= 127) {
      for (int i = 0; i < 8; i++) {
        uint8_t row_data = pgm_read_byte(&(font8x8[c - 32][i]));
        int shift = (7 - i) / 2 * scale;
        for (int j = 0; j < 8; j++) {
          if (row_data & (0x80 >> j)) {
            for (int sx = 0; sx < scale; sx++) {
              for (int sy = 0; sy < scale; sy++) {
                drawPixelLandscape(cur_x + j * scale + sx + shift,
                                   y + i * scale + sy, color);
              }
            }
          }
        }
      }
    }
    cur_x += 8 * scale;
    s++;
  }
}

void drawStringLandscape(int vx, int vy, const char *str, uint16_t color,
                         int scale, int spacing = 1) {
  int cur_vx = vx;
  while (*str) {
    drawCharLandscape(cur_vx, vy, *str, color, scale);
    cur_vx += 8 * scale + spacing * scale;
    str++;
  }
}

int getStringWidthLandscape(const char *str, int scale, int spacing = 1) {
  int len = strlen(str);
  if (len == 0)
    return 0;
  return len * 8 * scale + (len - 1) * spacing * scale;
}

void drawStringWithShadowLandscape(int vx, int vy, const char *str,
                                   uint16_t color, int scale,
                                   uint16_t shadow_color = 0x0000) {
  drawStringLandscape(vx + scale, vy + scale, str, shadow_color, scale);
  drawStringLandscape(vx, vy, str, color, scale);
}

void drawCenteredStringLandscape(int cx, int cy, const char *str,
                                 uint16_t color, int scale, int spacing = 1) {
  int w = getStringWidthLandscape(str, scale, spacing);
  drawStringWithShadowLandscape(cx - w / 2, cy - (4 * scale), str, color,
                                scale);
}

void drawCenteredItalicStringLandscape(int cx, int cy, const char *str,
                                       uint16_t color, int scale,
                                       int spacing = 1) {
  int w = getStringWidthLandscape(str, scale, spacing);
  drawItalicStringLandscape(cx - w / 2, cy - (4 * scale), str, color, scale);
}

// Picks the largest scale (up to max_scale) whose rendered width still
// fits inside max_width. This is what keeps every readout from ever
// overflowing its card/gauge, regardless of how many digits telemetry
// throws at it.
int autoFitScaleLandscape(const char *str, int max_width, int max_scale) {
  for (int s = max_scale; s >= 1; s--) {
    if (getStringWidthLandscape(str, s) <= max_width)
      return s;
  }
  return 1;
}

// Draws a two-line "big value / small caption" readout, auto-sized and
// vertically centered so it never spills past the given diameter. Used
// inside every gauge for a consistent, professional look.
// `max_value_scale` now defaults to 4 (was hard-capped at 3), and the
// usable width was bumped from 80% to 88% of the inner diameter -
// together these let bigger gauges show noticeably bigger digits
// instead of leaving unused space around a small readout.
void drawGaugeReadoutLandscape(int cx, int cy, const char *value_str,
                               const char *caption_str, uint16_t value_color,
                               uint16_t caption_color, int inner_diameter,
                               int max_value_scale = 4) {
  int max_w = (inner_diameter * 88) / 100;
  if (max_w < 20)
    max_w = 20;

  int value_scale = autoFitScaleLandscape(value_str, max_w, max_value_scale);
  int caption_scale = autoFitScaleLandscape(caption_str, max_w, 2);
  if (caption_scale > 1)
    caption_scale = 1; // caption always stays visually secondary

  int gap = 6;
  int value_h = 8 * value_scale;
  int caption_h = 8 * caption_scale;

  // Shrink further if the two lines together are taller than the circle.
  while (value_scale > 1 && (value_h + gap + caption_h) > inner_diameter) {
    value_scale--;
    value_h = 8 * value_scale;
  }

  int block_h = value_h + gap + caption_h;
  int value_cy = cy - block_h / 2 + value_h / 2;
  int caption_cy = value_cy + value_h / 2 + gap + caption_h / 2;

  drawCenteredStringLandscape(cx, value_cy, value_str, value_color,
                              value_scale);
  drawCenteredStringLandscape(cx, caption_cy, caption_str, caption_color,
                              caption_scale);
}

// Draws a premium glassmorphic panel with colored border and translucent
// body.
//   filled          - when false, the interior is left completely
//                      untouched so the video background shows through
//                      100% (outline-only "sci-fi HUD" look).
//   blend_strength   - forwarded to blendPixelLandscape (see above);
//                      default 2 = light 25% tint instead of the old
//                      fixed 50%.
//   corner_radius     - -1 (default) keeps the old behaviour (rounded
//                      corners capped at 15px). Pass h/2 for a full
//                      capsule/"pill" shape (used by the Ask Bar and
//                      slim status-bar layouts), or 0 for sharp corners.
void drawGlassCardLandscape(int vx, int vy, int w, int h, uint16_t border_color,
                            bool light_mode, bool filled = true,
                            int blend_strength = 2, int corner_radius = -1) {
  int r = (corner_radius >= 0) ? corner_radius : ((h / 2 < 15) ? h / 2 : 15);
  if (r > h / 2)
    r = h / 2;
  if (r > w / 2)
    r = w / 2;
  if (r < 0)
    r = 0;
  int r_sq = r * r;
  int r_in_sq = (r >= 2) ? (r - 2) * (r - 2) : 0;

  for (int dvy = 0; dvy < h; dvy++) {
    for (int dvx = 0; dvx < w; dvx++) {
      bool skip = false;
      bool border = false;

      if (dvx < r && dvy < r) {
        int d2 = (r - dvx) * (r - dvx) + (r - dvy) * (r - dvy);
        if (d2 > r_sq)
          skip = true;
        else if (d2 >= r_in_sq)
          border = true;
      } else if (dvx >= w - r && dvy < r) {
        int d2 =
            (dvx - (w - 1 - r)) * (dvx - (w - 1 - r)) + (r - dvy) * (r - dvy);
        if (d2 > r_sq)
          skip = true;
        else if (d2 >= r_in_sq)
          border = true;
      } else if (dvx < r && dvy >= h - r) {
        int d2 =
            (r - dvx) * (r - dvx) + (dvy - (h - 1 - r)) * (dvy - (h - 1 - r));
        if (d2 > r_sq)
          skip = true;
        else if (d2 >= r_in_sq)
          border = true;
      } else if (dvx >= w - r && dvy >= h - r) {
        int d2 = (dvx - (w - 1 - r)) * (dvx - (w - 1 - r)) +
                 (dvy - (h - 1 - r)) * (dvy - (h - 1 - r));
        if (d2 > r_sq)
          skip = true;
        else if (d2 >= r_in_sq)
          border = true;
      } else if (dvy < 2 || dvy >= h - 2) {
        border = true;
      } else if (dvx < 2 || dvx >= w - 2) {
        border = true;
      }

      if (skip)
        continue;

      int pvx = vx + dvx;
      int pvy = vy + dvy;
      if (border) {
        drawPixelLandscape(pvx, pvy, border_color);
      } else if (filled) {
        blendPixelLandscape(pvx, pvy, light_mode, blend_strength);
      }
      // else: fully transparent - the background video pixel is left
      // completely untouched, giving a pure outline HUD element.
    }
  }
}

// Draws a stylized segmented progress bar
void drawProgressBarLandscape(int vx, int vy, int w, int h, int percent,
                              uint16_t color, uint16_t bg_color) {
  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;

  for (int dvy = 0; dvy < h; dvy++) {
    for (int dvx = 0; dvx < w; dvx++) {
      drawPixelLandscape(vx + dvx, vy + dvy, bg_color);
    }
  }

  int fill_w = (w * percent) / 100;
  for (int dvy = 0; dvy < h; dvy++) {
    for (int dvx = 0; dvx < fill_w; dvx++) {
      drawPixelLandscape(vx + dvx, vy + dvy, color);
    }
  }

  // Digital segment grid effect: black vertical slits every 8 pixels
  for (int dvx = 8; dvx < w; dvx += 8) {
    for (int dvy = 0; dvy < h; dvy++) {
      drawPixelLandscape(vx + dvx, vy + dvy, 0x0000);
    }
  }
}

// -------------------------------------------------------------
// NEW MINIMAL/SCI-FI PRIMITIVES
// Used by the 12 new layouts below (thin status bars, corner
// brackets, ask bar). Kept generic so they can be reused across
// layouts instead of duplicating pixel math.
// -------------------------------------------------------------
void drawHLineLandscape(int vx, int vy, int len, int thickness,
                        uint16_t color) {
  for (int t = 0; t < thickness; t++)
    for (int i = 0; i < len; i++)
      drawPixelLandscape(vx + i, vy + t, color);
}

void drawVLineLandscape(int vx, int vy, int len, int thickness,
                        uint16_t color) {
  for (int t = 0; t < thickness; t++)
    for (int i = 0; i < len; i++)
      drawPixelLandscape(vx + t, vy + i, color);
}

// Four sci-fi "targeting reticle" corner brackets around a region.
// Draws nothing but the brackets themselves - the rest of the region
// stays fully transparent, which is what makes this the most minimal
// framing option in the whole layout set.
void drawCornerBracketsLandscape(int vx, int vy, int w, int h, uint16_t color,
                                 int bracket_len, int thickness) {
  // top-left
  drawHLineLandscape(vx, vy, bracket_len, thickness, color);
  drawVLineLandscape(vx, vy, thickness, bracket_len, color);
  // top-right
  drawHLineLandscape(vx + w - bracket_len, vy, bracket_len, thickness, color);
  drawVLineLandscape(vx + w - thickness, vy, thickness, bracket_len, color);
  // bottom-left
  drawHLineLandscape(vx, vy + h - thickness, bracket_len, thickness, color);
  drawVLineLandscape(vx, vy + h - bracket_len, thickness, bracket_len, color);
  // bottom-right
  drawHLineLandscape(vx + w - bracket_len, vy + h - thickness, bracket_len,
                     thickness, color);
  drawVLineLandscape(vx + w - thickness, vy + h - bracket_len, thickness,
                     bracket_len, color);
}

// Inline "LABEL [====------] VALUE" telemetry row - used by the status
// bar and ask bar layouts to pack a lot of information into a very
// thin strip without needing a card behind it.
void drawTelemetrySegmentLandscape(int vx, int vy, int w, const char *label,
                                   const char *value_str, int percent,
                                   uint16_t accent, uint16_t text_c,
                                   uint16_t track_c, int scale = 2) {
  drawStringWithShadowLandscape(vx, vy, label, text_c, scale);
  int label_w = getStringWidthLandscape(label, scale) + 16;
  int value_w = getStringWidthLandscape(value_str, scale);
  int bar_w = w - label_w - value_w - 16;
  if (bar_w < 24)
    bar_w = 24;
  int bar_h = (scale >= 2) ? 10 : 6;
  int text_h = 8 * scale;
  int bar_y = vy + text_h / 2 - bar_h / 2;
  drawProgressBarLandscape(vx + label_w, bar_y, bar_w, bar_h, percent, accent,
                           track_c);
  drawStringWithShadowLandscape(vx + label_w + bar_w + 16, vy, value_str,
                                accent, scale);
}

// -------------------------------------------------------------
// SPEEDOMETER GAUGE RENDERER
// Shares the progress bar's "digital segment" language (radial divider
// lines) and the card's glass-blend interior (now lighter by default),
// so gauges, bars and cards all read as one consistent design system.
// -------------------------------------------------------------
void drawSpeedometerGaugeLandscape(int cx, int cy, int radius, int thickness,
                                   int percentage, uint16_t fill_color,
                                   uint16_t track_color, bool light_mode,
                                   float start_angle = 225.0f,
                                   float sweep_angle = 270.0f,
                                   int blend_strength = 2) {
  if (percentage < 0)
    percentage = 0;
  if (percentage > 100)
    percentage = 100;

  int r_in = radius - thickness;
  if (r_in < 1)
    r_in = 1;
  int r_out_sq = radius * radius;
  int r_in_sq = r_in * r_in;
  int r_glass = (r_in > 4)
                    ? (r_in - 3)
                    : r_in; // small gap so the glass disc sits inside the ring
  int r_glass_sq = r_glass * r_glass;

  float fill_sweep = (percentage / 100.0f) * sweep_angle;
  const float segment_count =
      18.0f; // ~ matches the progress bar's 8px slit cadence visually
  const float segment_deg = sweep_angle / segment_count;

  for (int y = -radius; y <= radius; y++) {
    for (int x = -radius; x <= radius; x++) {
      int d_sq = x * x + y * y;

      if (d_sq <= r_glass_sq) {
        blendPixelLandscape(cx + x, cy + y, light_mode, blend_strength);
        continue;
      }

      if (d_sq < r_in_sq || d_sq > r_out_sq)
        continue;

      float px_angle = atan2f((float)x, (float)-y) * 180.0f / (float)M_PI;
      if (px_angle < 0)
        px_angle += 360.0f;
      float rel_angle = px_angle - start_angle;
      if (rel_angle < 0)
        rel_angle += 360.0f;
      if (rel_angle > sweep_angle)
        continue;

      uint16_t color = (rel_angle <= fill_sweep) ? fill_color : track_color;

      float seg_mod = fmodf(rel_angle, segment_deg);
      if (seg_mod < 1.2f && rel_angle > 0.5f)
        color = 0x0000; // divider tick

      drawPixelLandscape(cx + x, cy + y, color);
    }
  }
}

// Composes card + header + gauge + auto-fit readout in one bounds-safe
// call. gauge_radius/gauge_thickness of 0 mean "auto-size to the card".
//
// Sizing was reworked so cards no longer leave large empty margins:
//   - header vertical position is derived from its own rendered height
//     instead of a fixed +30px offset, so short/tall headers both sit
//     snugly against the top edge.
//   - the gauge radius is allowed to grow up to (min(w,h)/2 - 4) instead
//     of being hard-capped at 100px, so big cards get visibly big dials.
//   - minimum radius raised slightly (40px) so small cards never render
//     a tiny, lost-looking gauge.
void drawStatCardLandscape(int vx, int vy, int w, int h, const char *header,
                           const char *value_str, const char *caption_str,
                           int percent, uint16_t border_c, uint16_t fill_c,
                           uint16_t track_c, uint16_t text_c, bool light_mode,
                           int gauge_radius = 0, int gauge_thickness = 0,
                           bool filled = true, int blend_strength = 2) {
  drawGlassCardLandscape(vx, vy, w, h, border_c, light_mode, filled,
                         blend_strength);

  int pad = 16;
  int header_scale = 2;
  while (header_scale > 1 &&
         getStringWidthLandscape(header, header_scale) > (w - pad * 2)) {
    header_scale--;
  }
  int header_h = 8 * header_scale;
  int header_cy = vy + pad + header_h / 2 + 4;
  drawCenteredStringLandscape(vx + w / 2, header_cy, header, text_c,
                              header_scale);

  int header_bottom = header_cy + header_h / 2 + 12;
  int card_bottom = vy + h - pad;
  int avail_h = card_bottom - header_bottom;
  int avail_w = w - pad * 2;

  if (gauge_radius <= 0) {
    gauge_radius = (avail_h < avail_w ? avail_h : avail_w) / 2;
    int max_r = ((w < h ? w : h) / 2) - 4;
    if (gauge_radius > max_r)
      gauge_radius = max_r;
    if (gauge_radius < 40)
      gauge_radius = 40;
  }
  if (gauge_thickness <= 0) {
    gauge_thickness = gauge_radius / 6;
    if (gauge_thickness < 10)
      gauge_thickness = 10;
    if (gauge_thickness > 26)
      gauge_thickness = 26;
  }

  int gauge_cx = vx + w / 2;
  int gauge_cy = header_bottom + gauge_radius;
  if (gauge_cy + gauge_radius > card_bottom) {
    gauge_cy = card_bottom - gauge_radius;
  }

  drawSpeedometerGaugeLandscape(gauge_cx, gauge_cy, gauge_radius,
                                gauge_thickness, percent, fill_c, track_c,
                                light_mode, 225.0f, 270.0f, blend_strength);

  int inner_d = 2 * (gauge_radius - gauge_thickness);
  drawGaugeReadoutLandscape(gauge_cx, gauge_cy, value_str, caption_str, text_c,
                            border_c, inner_d);
}

void draw_telemetry_hud() {
  TelemetryData local_tel;
  uint32_t last_time = 0;

  if (telemetry_mutex != NULL && xSemaphoreTake(telemetry_mutex, 0) == pdTRUE) {
    local_tel = g_telemetry;
    last_time = last_telemetry_time;
    xSemaphoreGive(telemetry_mutex);
  } else {
    return;
  }

  bool is_stale = (millis() - last_time > 5000) || !local_tel.active;
  if (is_stale && !in_live_view_mode)
    return;

  if (in_live_view_mode && is_stale) {
    local_tel.cpu_temp = 55.0;
    local_tel.cpu_usage = 45;
    local_tel.gpu_temp = 62.0;
    local_tel.gpu_usage = 85;
    local_tel.mem_usage = 60;
    strncpy(local_tel.time, "12:00 PM", sizeof(local_tel.time));
  }

  char cpu_temp_str[16], cpu_usage_str[24];
  char gpu_temp_str[16], gpu_usage_str[24];
  char ram_pct_str[16];
  snprintf(cpu_temp_str, sizeof(cpu_temp_str), "%.0fC", local_tel.cpu_temp);
  snprintf(cpu_usage_str, sizeof(cpu_usage_str), "%d%%",
           local_tel.cpu_usage);
  snprintf(gpu_temp_str, sizeof(gpu_temp_str), "%.0fC", local_tel.gpu_temp);
  snprintf(gpu_usage_str, sizeof(gpu_usage_str), "%d%%",
           local_tel.gpu_usage);
  snprintf(ram_pct_str, sizeof(ram_pct_str), "%d%%", local_tel.mem_usage);
  const char *ram_detail_str = "MEMORY USED";

  if (use_custom_layout) {
    bool light_mode = false;
    JsonArray widgets = current_layout_doc["widgets"];
    for (JsonObject widget : widgets) {
      const char *type = widget["type"] | "";
      int x = widget["x"] | 0;
      int y = widget["y"] | 0;
      int w = widget["w"] | 0;
      int h = widget["h"] | 0;
      const char *colorHex = widget["color"] | "";

      uint16_t color = 0xFFFF;
      if (colorHex[0] == '#' && strlen(colorHex) == 7) {
        long number = strtol(&colorHex[1], NULL, 16);
        int r = (number >> 16) & 0xFF;
        int g = (number >> 8) & 0xFF;
        int b = number & 0xFF;
        color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
      }

      if (strcmp(type, "Card") == 0) {
        drawGlassCardLandscape(x, y, w, h, color, light_mode, true, 2, 15);
      } else if (strcmp(type, "Gauge") == 0) {
        int radius = (w < h ? w : h) / 2;
        const char *source = widget["dataSource"] | "";
        if (strcmp(source, "null") == 0 || strlen(source) == 0)
          source = widget["text"] | "";
        const char *valStr = "0";
        const char *capStr = source;
        int percent = 0;
        if (strcmp(source, "CPU Usage") == 0) {
          valStr = cpu_usage_str;
          percent = local_tel.cpu_usage;
          capStr = "CPU";
        } else if (strcmp(source, "CPU Temp") == 0) {
          valStr = cpu_temp_str;
          percent = (int)local_tel.cpu_temp;
          capStr = "CPU";
        } else if (strcmp(source, "GPU Usage") == 0) {
          valStr = gpu_usage_str;
          percent = local_tel.gpu_usage;
          capStr = "GPU";
        } else if (strcmp(source, "GPU Temp") == 0) {
          valStr = gpu_temp_str;
          percent = (int)local_tel.gpu_temp;
          capStr = "GPU";
        } else if (strcmp(source, "RAM") == 0) {
          valStr = ram_pct_str;
          percent = local_tel.mem_usage;
          capStr = "RAM";
        } else {
          valStr = "0";
          capStr = "";
        }

        drawSpeedometerGaugeLandscape(x + w / 2, y + h / 2, radius, 15, percent,
                                      color, 0x2124, light_mode);
        int inner_d = 2 * (radius - 15);
        if (strlen(capStr) > 0 && strcmp(capStr, "None") != 0) {
          drawGaugeReadoutLandscape(x + w / 2, y + h / 2, valStr,
                                    capStr, 0xFFFF, color, inner_d);
        }
      } else if (strcmp(type, "ProgressBar") == 0) {
        const char *source = widget["dataSource"] | "";
        int percent = local_tel.cpu_usage;
        if (strcmp(source, "CPU Temp") == 0)
          percent = (int)local_tel.cpu_temp;
        else if (strcmp(source, "GPU Usage") == 0)
          percent = local_tel.gpu_usage;
        else if (strcmp(source, "GPU Temp") == 0)
          percent = (int)local_tel.gpu_temp;
        else if (strcmp(source, "RAM") == 0)
          percent = local_tel.mem_usage;
        drawProgressBarLandscape(x, y, w, h, percent, color, 0x2124);
      } else if (strcmp(type, "Text") == 0) {
        const char *text_ptr = widget["text"] | "";
        const char *source = widget["dataSource"] | "";
        char text_buf[64];
        if (strcmp(source, "CPU Usage") == 0)
          text_ptr = cpu_usage_str;
        else if (strcmp(source, "CPU Temp") == 0)
          text_ptr = cpu_temp_str;
        else if (strcmp(source, "GPU Usage") == 0)
          text_ptr = gpu_usage_str;
        else if (strcmp(source, "GPU Temp") == 0)
          text_ptr = gpu_temp_str;
        else if (strcmp(source, "RAM") == 0)
          text_ptr = ram_pct_str;
        else {
          strncpy(text_buf, text_ptr, sizeof(text_buf) - 1);
          text_buf[sizeof(text_buf) - 1] = '\0';
          text_ptr = text_buf;
        }
        drawStringWithShadowLandscape(x, y, text_ptr, color, 3);
      }
    }
    return;
  }

  int arrange_idx = current_layout - 1; // 0..14, see layout index comment above

  uint16_t border_c = 0x07FF; // Synapse Cyan
  uint16_t fill_c = 0x03EF;   // Blue
  uint16_t track_c = 0x2124;  // Dark Grey track
  uint16_t text_c = 0xFFFF;   // White Text
  bool light_mode = false;

  if (arrange_idx == 0) {
    // --- Layout 1: Premium 4-card grid ---
    drawStatCardLandscape(30, 20, 430, 440, "CPU CORE", cpu_usage_str,
                          cpu_temp_str, local_tel.cpu_usage, border_c, fill_c,
                          track_c, text_c, light_mode);
    drawStatCardLandscape(495, 20, 430, 440, "GPU ENGINE", gpu_usage_str,
                          gpu_temp_str, local_tel.gpu_usage, border_c, fill_c,
                          track_c, text_c, light_mode);
    drawStatCardLandscape(960, 20, 430, 440, "SYSTEM RAM", ram_pct_str,
                          ram_detail_str, local_tel.mem_usage, border_c, fill_c,
                          track_c, text_c, light_mode);

    // Card 4: Dedicated System Clock & Datalink Panel
    drawGlassCardLandscape(1425, 20, 465, 440, border_c, light_mode, false);
    drawCenteredStringLandscape(1657, 75, "DATALINK", border_c, 3);
    drawCenteredStringLandscape(1657, 125, "ACTIVE", fill_c, 2);
    drawCenteredStringLandscape(1657, 215, local_tel.time, text_c, 4);
    drawCenteredStringLandscape(1657, 285, local_tel.date, border_c, 2);
    drawProgressBarLandscape(1460, 365, 395, 26, local_tel.mem_usage, fill_c,
                             track_c);
    drawCenteredStringLandscape(1657, 415, "SYSTEM ONLINE", text_c, 2);

  } else if (arrange_idx == 1) {
    // --- Layout 2: 3 Large Minimalist Dials + Top Banner ---
    drawGlassCardLandscape(660, 10, 600, 54, border_c, light_mode, false, 2, 27);
    drawCenteredStringLandscape(960, 37, local_tel.time, text_c, 3);

    const int radius = 180, thickness = 22;
    const int centers[3] = {330, 960, 1590};
    const char *labels[3] = {"CPU", "GPU", "RAM"};
    const char *values[3] = {cpu_usage_str, gpu_usage_str, ram_pct_str};
    const char *captions[3] = {cpu_temp_str, gpu_temp_str, ram_detail_str};
    const int percents[3] = {local_tel.cpu_usage, local_tel.gpu_usage,
                             local_tel.mem_usage};

    for (int i = 0; i < 3; i++) {
      drawCenteredStringLandscape(centers[i], 85, labels[i], border_c, 3);
      drawSpeedometerGaugeLandscape(centers[i], 280, radius, thickness,
                                    percents[i], fill_c, track_c, light_mode);
      int inner_d = 2 * (radius - thickness);
      drawGaugeReadoutLandscape(centers[i], 280, values[i], captions[i], text_c,
                                border_c, inner_d, 4);
    }

  } else if (arrange_idx == 2) {
    // --- Layout 3: Asymmetric Sidebar + Live Clock Panel ---
    drawStatCardLandscape(30, 15, 340, 450, "CPU", cpu_usage_str, cpu_temp_str,
                          local_tel.cpu_usage, border_c, fill_c, track_c,
                          text_c, light_mode, 130, 18);
    drawStatCardLandscape(390, 15, 340, 450, "GPU", gpu_usage_str, gpu_temp_str,
                          local_tel.gpu_usage, border_c, fill_c, track_c,
                          text_c, light_mode, 130, 18);

    drawGlassCardLandscape(750, 15, 1140, 450, border_c, light_mode, false);
    drawStringWithShadowLandscape(790, 45, "SYSTEM LOAD", border_c, 3);
    drawCenteredStringLandscape(1650, 45, local_tel.time, text_c, 3);
    drawCenteredStringLandscape(1650, 85, local_tel.date, border_c, 2);

    drawStringWithShadowLandscape(790, 135, "CPU LOAD", text_c, 2);
    drawProgressBarLandscape(790, 170, 1060, 36, local_tel.cpu_usage, fill_c,
                             track_c);

    drawStringWithShadowLandscape(790, 245, "GPU LOAD", text_c, 2);
    drawProgressBarLandscape(790, 280, 1060, 36, local_tel.gpu_usage, fill_c,
                             track_c);

    drawStringWithShadowLandscape(790, 355, "MEMORY LOAD", text_c, 2);
    drawProgressBarLandscape(790, 390, 1060, 36, local_tel.mem_usage, fill_c,
                             track_c);

  } else if (arrange_idx == 3) {
    // --- Layout 4: Top Bar ---
    int bar_h = 90;
    drawGlassCardLandscape(20, 15, 1880, bar_h, border_c, light_mode, false, 2,
                           bar_h / 2);
    int seg_w = 420;
    int seg_y = 15 + bar_h / 2 - 12;
    drawTelemetrySegmentLandscape(70, seg_y, seg_w, "CPU", cpu_usage_str,
                                  local_tel.cpu_usage, fill_c, text_c, track_c);
    drawTelemetrySegmentLandscape(70 + seg_w + 20, seg_y, seg_w, "GPU", gpu_usage_str,
                                  local_tel.gpu_usage, fill_c, text_c, track_c);
    drawTelemetrySegmentLandscape(70 + (seg_w + 20) * 2, seg_y, seg_w, "RAM",
                                  ram_pct_str, local_tel.mem_usage, fill_c,
                                  text_c, track_c);
    drawCenteredStringLandscape(1740, 15 + bar_h / 2 - 14, local_tel.time, text_c,
                                3);
    drawCenteredStringLandscape(1740, 15 + bar_h / 2 + 16, local_tel.date, border_c,
                                2);

  } else if (arrange_idx == 4) {
    // --- Layout 5: Bottom Bar ---
    int bar_h = 90;
    int bar_y = 480 - 15 - bar_h;
    drawGlassCardLandscape(20, bar_y, 1880, bar_h, border_c, light_mode, false,
                           2, bar_h / 2);
    int seg_w = 420;
    int seg_y = bar_y + bar_h / 2 - 12;
    drawTelemetrySegmentLandscape(70, seg_y, seg_w, "CPU", cpu_usage_str,
                                  local_tel.cpu_usage, fill_c, text_c, track_c);
    drawTelemetrySegmentLandscape(70 + seg_w + 20, seg_y, seg_w, "GPU", gpu_usage_str,
                                  local_tel.gpu_usage, fill_c, text_c, track_c);
    drawTelemetrySegmentLandscape(70 + (seg_w + 20) * 2, seg_y, seg_w, "RAM",
                                  ram_pct_str, local_tel.mem_usage, fill_c,
                                  text_c, track_c);
    drawCenteredStringLandscape(1740, bar_y + bar_h / 2 - 14, local_tel.time,
                                text_c, 3);
    drawCenteredStringLandscape(1740, bar_y + bar_h / 2 + 16, local_tel.date,
                                border_c, 2);

  } else if (arrange_idx == 5) {
    // --- Layout 6: Corner Minimal ---
    drawCornerBracketsLandscape(20, 15, 1880, 450, border_c, 60, 4);

    drawStringWithShadowLandscape(60, 45, "CPU CORE", border_c, 2);
    drawStringWithShadowLandscape(60, 75, cpu_usage_str, text_c, 4);
    drawStringWithShadowLandscape(60, 125, cpu_temp_str, fill_c, 2);

    drawCenteredStringLandscape(1760, 45, "GPU ENGINE", border_c, 2);
    drawCenteredStringLandscape(1760, 85, gpu_usage_str, text_c, 4);
    drawCenteredStringLandscape(1760, 135, gpu_temp_str, fill_c, 2);

    drawStringWithShadowLandscape(60, 365, "SYSTEM RAM", border_c, 2);
    drawStringWithShadowLandscape(60, 395, ram_pct_str, text_c, 3);
    drawProgressBarLandscape(60, 435, 300, 12, local_tel.mem_usage, fill_c,
                             track_c);

    drawCenteredStringLandscape(1740, 385, local_tel.time, text_c, 4);
    drawCenteredStringLandscape(1740, 435, local_tel.date, border_c, 2);

  } else if (arrange_idx == 6) {
    // --- Layout 7: Concentric Rings ---
    int cx = 960, cy = 240;
    drawSpeedometerGaugeLandscape(cx, cy, 210, 20, local_tel.cpu_usage, fill_c,
                                  track_c, light_mode);
    drawSpeedometerGaugeLandscape(cx, cy, 170, 18, local_tel.gpu_usage, 0xFD20,
                                  track_c, light_mode);
    drawSpeedometerGaugeLandscape(cx, cy, 130, 16, local_tel.mem_usage, 0xF81F,
                                  track_c, light_mode);
    drawGaugeReadoutLandscape(cx, cy, cpu_usage_str, "CPU LOAD", text_c, border_c,
                              190, 4);

    drawStringWithShadowLandscape(cx + 250, 80, "CPU", fill_c, 2);
    drawStringWithShadowLandscape(cx + 250, 108, cpu_temp_str, text_c, 2);
    drawStringWithShadowLandscape(cx + 250, 200, "GPU", 0xFD20, 2);
    drawStringWithShadowLandscape(cx + 250, 228, gpu_temp_str, text_c, 2);
    drawStringWithShadowLandscape(cx + 250, 320, "RAM", 0xF81F, 2);
    drawStringWithShadowLandscape(cx + 250, 348, ram_pct_str, text_c, 2);

    drawGlassCardLandscape(60, 20, 400, 100, border_c, light_mode, false, 2, 15);
    drawCenteredStringLandscape(260, 50, local_tel.time, text_c, 3);
    drawCenteredStringLandscape(260, 90, local_tel.date, border_c, 2);

  } else if (arrange_idx == 7) {
    // --- Layout 8: Left Rail ---
    int card_w = 320, card_h = 135, gap = 12, start_y = 15;
    drawStatCardLandscape(20, start_y, card_w, card_h, "CPU", cpu_usage_str,
                          cpu_temp_str, local_tel.cpu_usage, border_c, fill_c,
                          track_c, text_c, light_mode, 55, 10, false, 2);
    drawStatCardLandscape(20, start_y + card_h + gap, card_w, card_h, "GPU",
                          gpu_usage_str, gpu_temp_str, local_tel.gpu_usage,
                          border_c, fill_c, track_c, text_c, light_mode, 55, 10,
                          false, 2);
    drawStatCardLandscape(20, start_y + 2 * (card_h + gap), card_w, card_h,
                          "RAM", ram_pct_str, ram_detail_str,
                          local_tel.mem_usage, border_c, fill_c, track_c,
                          text_c, light_mode, 55, 10, false, 2);

    drawGlassCardLandscape(1460, 20, 440, 110, border_c, light_mode, false, 2, 20);
    drawCenteredStringLandscape(1680, 55, local_tel.time, text_c, 4);
    drawCenteredStringLandscape(1680, 100, local_tel.date, border_c, 2);

  } else if (arrange_idx == 8) {
    // --- Layout 9: Right Rail ---
    int card_w = 320, card_h = 135, gap = 12, start_y = 15;
    int start_x = 1920 - 20 - card_w;
    drawStatCardLandscape(start_x, start_y, card_w, card_h, "CPU", cpu_usage_str,
                          cpu_temp_str, local_tel.cpu_usage, border_c, fill_c,
                          track_c, text_c, light_mode, 55, 10, false, 2);
    drawStatCardLandscape(start_x, start_y + card_h + gap, card_w, card_h,
                          "GPU", gpu_usage_str, gpu_temp_str,
                          local_tel.gpu_usage, border_c, fill_c, track_c,
                          text_c, light_mode, 55, 10, false, 2);
    drawStatCardLandscape(start_x, start_y + 2 * (card_h + gap), card_w, card_h,
                          "RAM", ram_pct_str, ram_detail_str,
                          local_tel.mem_usage, border_c, fill_c, track_c,
                          text_c, light_mode, 55, 10, false, 2);

    drawGlassCardLandscape(20, 20, 440, 110, border_c, light_mode, false, 2, 20);
    drawCenteredStringLandscape(240, 55, local_tel.time, text_c, 4);
    drawCenteredStringLandscape(240, 100, local_tel.date, border_c, 2);

  } else if (arrange_idx == 9) {
    // --- Layout 10: Diagonal Cascade ---
    drawStatCardLandscape(30, 20, 320, 220, "CPU CORE", cpu_usage_str, cpu_temp_str,
                          local_tel.cpu_usage, border_c, fill_c, track_c,
                          text_c, light_mode, 0, 0, false, 2);
    drawStatCardLandscape(520, 130, 420, 320, "GPU ENGINE", gpu_usage_str,
                          gpu_temp_str, local_tel.gpu_usage, border_c, fill_c,
                          track_c, text_c, light_mode, 0, 0, false, 2);
    
    drawGlassCardLandscape(1330, 20, 560, 440, border_c, light_mode, true, 2, 15);
    drawCenteredStringLandscape(1610, 70, "SYSTEM RAM", border_c, 3);
    drawCenteredStringLandscape(1610, 130, ram_pct_str, text_c, 4);
    drawProgressBarLandscape(1370, 185, 480, 28, local_tel.mem_usage, fill_c, track_c);

    drawCenteredStringLandscape(1610, 290, local_tel.time, text_c, 4);
    drawCenteredStringLandscape(1610, 350, local_tel.date, border_c, 2);
    drawCenteredStringLandscape(1610, 400, "DATALINK ACTIVE", fill_c, 2);

  } else if (arrange_idx == 10) {
    // --- Layout 11: Targeting HUD ---
    drawCornerBracketsLandscape(40, 15, 1840, 450, border_c, 60, 4);
    int cx = 960, cy = 210;
    int avg =
        (local_tel.cpu_usage + local_tel.gpu_usage + local_tel.mem_usage) / 3;
    drawSpeedometerGaugeLandscape(cx, cy, 140, 16, avg, fill_c, track_c,
                                  light_mode);
    char avg_str[8];
    snprintf(avg_str, sizeof(avg_str), "%d%%", avg);
    drawGaugeReadoutLandscape(cx, cy, avg_str, "LOAD", text_c, border_c,
                              180, 4);

    drawCenteredStringLandscape(960, 40, local_tel.time, text_c, 3);

    drawTelemetrySegmentLandscape(80, 415, 420, "CPU", cpu_usage_str,
                                  local_tel.cpu_usage, fill_c, text_c, track_c);
    drawTelemetrySegmentLandscape(750, 415, 420, "GPU", gpu_usage_str,
                                  local_tel.gpu_usage, fill_c, text_c, track_c);
    drawTelemetrySegmentLandscape(1420, 415, 420, "RAM", ram_pct_str,
                                  local_tel.mem_usage, fill_c, text_c, track_c);

  } else if (arrange_idx == 11) {
    // --- Layout 12: Split Bar ---
    drawGlassCardLandscape(560, 15, 800, 64, border_c, light_mode, false, 2,
                           32);
    drawCenteredStringLandscape(960, 47, local_tel.time, text_c, 3);

    drawSpeedometerGaugeLandscape(280, 280, 135, 16, local_tel.cpu_usage,
                                  fill_c, track_c, light_mode);
    drawGaugeReadoutLandscape(280, 280, cpu_usage_str, "CPU", text_c, border_c,
                              190, 4);

    drawSpeedometerGaugeLandscape(1640, 280, 135, 16, local_tel.gpu_usage,
                                  fill_c, track_c, light_mode);
    drawGaugeReadoutLandscape(1640, 280, gpu_usage_str, "GPU", text_c, border_c,
                              190, 4);

    drawStringWithShadowLandscape(650, 200, "MEMORY LOAD", text_c, 2);
    drawProgressBarLandscape(650, 235, 620, 36, local_tel.mem_usage, fill_c,
                             track_c);
    drawCenteredStringLandscape(960, 320, ram_pct_str, fill_c, 4);
    drawCenteredStringLandscape(960, 380, local_tel.date, border_c, 2);

  } else if (arrange_idx == 12) {
    // --- Layout 13: Circular Cluster ---
    int r = 100, thick = 14;
    int cy = 310;
    int cx1 = 1320, cx2 = 1540, cx3 = 1760;
    int inner = 2 * (r - thick);
    drawSpeedometerGaugeLandscape(cx1, cy, r, thick, local_tel.cpu_usage,
                                  fill_c, track_c, light_mode);
    drawGaugeReadoutLandscape(cx1, cy, cpu_usage_str, "CPU", text_c, border_c,
                              inner, 4);
    drawSpeedometerGaugeLandscape(cx2, cy, r, thick, local_tel.gpu_usage,
                                  fill_c, track_c, light_mode);
    drawGaugeReadoutLandscape(cx2, cy, gpu_usage_str, "GPU", text_c, border_c,
                              inner, 4);
    drawSpeedometerGaugeLandscape(cx3, cy, r, thick, local_tel.mem_usage,
                                  fill_c, track_c, light_mode);
    drawGaugeReadoutLandscape(cx3, cy, ram_pct_str, "RAM", text_c, border_c,
                              inner, 4);

    drawGlassCardLandscape(40, 30, 520, 130, border_c, light_mode, false, 2, 20);
    drawStringWithShadowLandscape(70, 75, local_tel.time, text_c, 4);
    drawStringWithShadowLandscape(70, 125, local_tel.date, border_c, 2);

  } else if (arrange_idx == 13) {
    // --- Layout 14: Text Only Minimal ---
    int x = 50, y = 40, gap = 70;
    drawStringWithShadowLandscape(x, y, "CPU CORE", border_c, 2);
    drawStringWithShadowLandscape(x + 180, y, cpu_usage_str, text_c, 3);
    drawStringWithShadowLandscape(x + 320, y, cpu_temp_str, fill_c, 2);
    drawProgressBarLandscape(x, y + 32, 450, 10, local_tel.cpu_usage, fill_c,
                             track_c);

    drawStringWithShadowLandscape(x, y + gap, "GPU ENGINE", border_c, 2);
    drawStringWithShadowLandscape(x + 180, y + gap, gpu_usage_str, text_c, 3);
    drawStringWithShadowLandscape(x + 320, y + gap, gpu_temp_str, fill_c, 2);
    drawProgressBarLandscape(x, y + gap + 32, 450, 10, local_tel.gpu_usage,
                             fill_c, track_c);

    drawStringWithShadowLandscape(x, y + 2 * gap, "SYSTEM RAM", border_c, 2);
    drawStringWithShadowLandscape(x + 180, y + 2 * gap, ram_pct_str, text_c, 3);
    drawProgressBarLandscape(x, y + 2 * gap + 32, 450, 10, local_tel.mem_usage,
                             fill_c, track_c);

    // Large Time & Date Panel on Right
    drawGlassCardLandscape(1340, 30, 530, 410, border_c, light_mode, false, 2, 20);
    drawCenteredStringLandscape(1605, 120, "SYSTEM CLOCK", border_c, 3);
    drawCenteredStringLandscape(1605, 210, local_tel.time, text_c, 4);
    drawCenteredStringLandscape(1605, 290, local_tel.date, fill_c, 3);
    drawCenteredStringLandscape(1605, 360, "STATUS: ONLINE", text_c, 2);

  } else {
    // --- Layout 15: Ask Bar (Floating Pill) ---
    int bar_w = 1480, bar_h = 90;
    int bar_x = (1920 - bar_w) / 2;
    int bar_y = 480 - 25 - bar_h;
    drawGlassCardLandscape(bar_x, bar_y, bar_w, bar_h, border_c, light_mode,
                           true, 3, bar_h / 2);

    int seg_y = bar_y + bar_h / 2 - 12;
    drawTelemetrySegmentLandscape(bar_x + 50, seg_y, 400, "CPU", cpu_usage_str,
                                  local_tel.cpu_usage, fill_c, text_c, track_c);
    drawTelemetrySegmentLandscape(bar_x + 510, seg_y, 400, "GPU", gpu_usage_str,
                                  local_tel.gpu_usage, fill_c, text_c, track_c);
    drawTelemetrySegmentLandscape(bar_x + 970, seg_y, 400, "RAM", ram_pct_str,
                                  local_tel.mem_usage, fill_c, text_c, track_c);

    drawGlassCardLandscape(660, 20, 600, 60, border_c, light_mode, false, 2, 30);
    drawCenteredStringLandscape(960, 50, local_tel.time, border_c, 3);
  }
}

// -------------------------------------------------------------
// ASYNCHRONOUS UART RX FREERTOS TASK (Core 0)
// -------------------------------------------------------------
void telemetry_task(void *pvParameters) {
  String inputBuffer = "";
  inputBuffer.reserve(2048);

  while (1) {
    while (Serial.available() > 0) {
      char c = Serial.read();
      if (c == '\n') {
        if (inputBuffer.startsWith("$TEL:")) {
          in_live_view_mode = false;
          float cpu_t = 0.0f;
          int cpu_u = 0;
          float gpu_t = 0.0f;
          int gpu_u = 0;
          int mem_u = 0;
          char d_str[16] = {0};
          char t_str[16] = {0};

          // $TEL:cpu_temp,cpu_usage,gpu_temp,gpu_usage,mem_usage,date,time
          int parsed = sscanf(inputBuffer.c_str(),
                              "$TEL:%f,%d,%f,%d,%d,%15[^,],%15[^\r\n]", &cpu_t,
                              &cpu_u, &gpu_t, &gpu_u, &mem_u, d_str, t_str);

          if (parsed >= 7) {
            if (telemetry_mutex != NULL &&
                xSemaphoreTake(telemetry_mutex, portMAX_DELAY) == pdTRUE) {
              g_telemetry.cpu_temp = cpu_t;
              g_telemetry.cpu_usage = (uint8_t)constrain(cpu_u, 0, 100);
              g_telemetry.gpu_temp = gpu_t;
              g_telemetry.gpu_usage = (uint8_t)constrain(gpu_u, 0, 100);
              g_telemetry.mem_usage = (uint8_t)constrain(mem_u, 0, 100);
              strncpy(g_telemetry.date, d_str, sizeof(g_telemetry.date) - 1);
              strncpy(g_telemetry.time, t_str, sizeof(g_telemetry.time) - 1);
              g_telemetry.active = true;
              last_telemetry_time = millis();
              xSemaphoreGive(telemetry_mutex);
            }
          }
        } else if (inputBuffer.startsWith("$CMD:LIST_BGS")) {
          const char *dirPath = Vertical ? "/Vertical" : "/Horizontal";
          File root = SD_MMC.open(dirPath);
          Serial.print("$BGLIST:");
          if (root) {
            File file = root.openNextFile();
            while (file) {
              if (!file.isDirectory()) {
                const char *fname = file.name();
                size_t flen = strlen(fname);
                if (flen >= 6 && strcasecmp(fname + flen - 6, ".mjpeg") == 0) {
                  if (fname[0] != '/') {
                    Serial.printf("%s/%s,", dirPath, fname);
                  } else {
                    Serial.printf("%s,", fname);
                  }
                }
              }
              file = root.openNextFile();
            }
            root.close();
          }
          Serial.println();
        } else if (inputBuffer.startsWith("$CMD:LIVE_VIEW:")) {
          in_live_view_mode = true;
          // Parse JSON directly from the input buffer at offset 15
          DeserializationError err = deserializeJson(current_layout_doc,
                                                      inputBuffer.c_str() + 15);
          if (!err) {
            use_custom_layout = true;
            const char *bg = current_layout_doc["bg"] | "";
            if (bg[0] != '\0') {
              live_view_bg = bg;
              force_video_exit = true;
            }
            Serial.println("[SYS] Live view updated.");
          } else {
            Serial.println("[SYS] JSON parse error.");
          }
        } else if (inputBuffer.startsWith("$CMD:SAVE_LAYOUT:")) {
          File file = SD_MMC.open("/layout.json", FILE_WRITE);
          if (file) {
            // Write directly from the input buffer at offset 17
            file.print(inputBuffer.c_str() + 17);
            file.close();
            Serial.println("[SD_MMC] Layout saved.");
          }
        }
        inputBuffer = "";
      } else {
        if (inputBuffer.length() < 2048) {
          inputBuffer += c;
        } else {
          inputBuffer = "";
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// -------------------------------------------------------------
// BUTTON TASK (Core 0) - dedicated, non-blocking state machine.
// Runs independently of video/JPEG timing so no click is ever missed,
// and long-press fires the instant the hold threshold is crossed
// instead of waiting for release.
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
        // Just pressed
        press_start_ms = now;
        long_press_fired = false;
        click_count++;
      } else {
        // Just released
        if (!long_press_fired) {
          release_ms = now;
        }
      }
    }

    // Long press fires immediately once held past threshold - no need to wait for release.
    if (stable_pressed && !long_press_fired &&
        (now - press_start_ms) >= BUTTON_LONG_PRESS_MS) {
      long_press_fired = true;
      click_count = 0;
      Serial.println("[Button] Long press detected - restarting board...");
      vTaskDelay(pdMS_TO_TICKS(50));
      ESP.restart();
    }

    // Resolve click sequence once the multi-click window closes.
    if (click_count > 0 && !stable_pressed &&
        (now - release_ms) > BUTTON_DOUBLE_MS) {
      ButtonAction act;
      if (click_count == 1) {
        act = BTN_ACTION_SINGLE;
      } else if (click_count == 2) {
        act = BTN_ACTION_DOUBLE;
      } else {
        act = BTN_ACTION_TRIPLE;
      }
      if (button_action_queue)
        xQueueSend(button_action_queue, &act, 0);
      click_count = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
  }
}

// Advances + persists current_animation, returns true (caller should
// stop the clip currently playing and move to the next one).
bool perform_single_click_action() {
  current_animation++;
  if (current_animation > MAX_ANIMATIONS)
    current_animation = 1;

  Preferences prefs;
  prefs.begin("anim_prefs", false);
  prefs.putInt("last_anim", current_animation);
  prefs.end();

  Serial.printf("[Button] Single click -> background #%d (saved to NVS)\n",
                current_animation);
  return true;
}

// Advances + persists current_layout. Playback is not interrupted.
void perform_double_click_action() {
  current_layout++;
  if (current_layout > MAX_LAYOUTS)
    current_layout = 1;

  Preferences prefs;
  prefs.begin("anim_prefs", false);
  prefs.putInt("last_layout", current_layout);
  prefs.end();

  Serial.printf("[Button] Double click -> layout #%d (saved to NVS)\n",
                current_layout);

  // Brief flash for tactile feedback that the layout changed.
  if (display_fbs[0] != NULL) {
    clear_screen(0xFFFF);
    delay(60);
  }
}

// Toggles + persists Vertical mode. Interrupts the currently playing video so
// the new folder/mode animation starts playing immediately.
bool perform_triple_click_action() {
  Vertical = !Vertical;
  scan_active_folder();

  Preferences prefs;
  prefs.begin("anim_prefs", false);
  prefs.putBool("is_vertical", Vertical);
  prefs.end();

  Serial.printf("[Button] Triple click -> Vertical mode: %s (saved to NVS)\n",
                Vertical ? "TRUE (/Vertical)" : "FALSE (/Horizontal)");

  // Green flash for tactile feedback that the orientation mode changed.
  if (display_fbs[0] != NULL) {
    clear_screen(0x07E0); // Green color in RGB565
    delay(100);
  }
  return true;
}

// Drains any pending button actions. Returns true if the current video
// clip should be aborted (i.e. single or triple click occurred).
bool process_button_queue() {
  ButtonAction act;
  bool should_break = false;
  while (button_action_queue != NULL &&
         xQueueReceive(button_action_queue, &act, 0) == pdTRUE) {
    if (act == BTN_ACTION_SINGLE) {
      should_break = perform_single_click_action() || should_break;
    } else if (act == BTN_ACTION_DOUBLE) {
      perform_double_click_action();
    } else if (act == BTN_ACTION_TRIPLE) {
      should_break = perform_triple_click_action() || should_break;
    }
  }
  return should_break;
}

// Sleeps until target_time_us while still draining the button queue in
// small slices, so frame-pacing delays never swallow a click.
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
  Serial.println("Resetting display hardware...");
  pinMode(LCD_RST_PIN, OUTPUT);
  digitalWrite(LCD_RST_PIN, LOW);
  delay(50);
  digitalWrite(LCD_RST_PIN, HIGH);
  delay(120);
}

void init_mipi_ldo() {
  Serial.println("Initializing internal LDO to power D-PHY (2.5V)...");
  esp_ldo_channel_config_t ldo_cfg;
  memset(&ldo_cfg, 0, sizeof(ldo_cfg));
  ldo_cfg.chan_id = 3;
  ldo_cfg.voltage_mv = 2500;

  esp_err_t err = esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy);
  if (err == ESP_OK) {
    Serial.println("LDO channel 3 successfully acquired and enabled (2.5V)");
  } else {
    Serial.printf("LDO channel acquisition failed! Error: 0x%x\n", err);
  }
}

void init_backlight() {
  Serial.println("Initializing screen backlight...");
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  Serial.println("--- Starting I2C Bus Scan ---");
  int devices_found = 0;
  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.printf("Found I2C device at address 0x%02X\n", address);
      devices_found++;
    }
  }
  if (devices_found == 0) {
    Serial.println("No I2C devices responded on SDA/SCL pins!");
  }
  Serial.println("--- I2C Scan Complete ---");

  uint8_t brightness_val = (LCD_BRIGHTNESS_PCT * 255) / 100;
  bool i2c_success = false;

  // 1. Standard Waveshare backlight MCU
  Wire.beginTransmission(BACKLIGHT_I2C_ADDR);
  Wire.write(BACKLIGHT_REG_BRIGHTNESS);
  Wire.write(brightness_val);
  uint8_t error_45_86 = Wire.endTransmission();

  Wire.beginTransmission(BACKLIGHT_I2C_ADDR);
  Wire.write(0x96);
  Wire.write(brightness_val);
  uint8_t error_45_96 = Wire.endTransmission();

  if (error_45_86 == 0 || error_45_96 == 0) {
    i2c_success = true;
    Serial.printf(
        "Backlight set via I2C 0x45 (Reg 0x86 err: %d, Reg 0x96 err: %d).\n",
        error_45_86, error_45_96);
  }

  // 2. Alternate address 0x1E
  Wire.beginTransmission(0x1E);
  Wire.write(0x86);
  Wire.write(brightness_val);
  uint8_t error_1e_86 = Wire.endTransmission();

  Wire.beginTransmission(0x1E);
  Wire.write(0x96);
  Wire.write(brightness_val);
  uint8_t error_1e_96 = Wire.endTransmission();

  Wire.beginTransmission(0x1E);
  Wire.write(0x00);
  Wire.write(brightness_val);
  uint8_t error_1e_00 = Wire.endTransmission();

  Wire.beginTransmission(0x1E);
  Wire.write(0x01);
  Wire.write(brightness_val);
  uint8_t error_1e_01 = Wire.endTransmission();

  if (error_1e_86 == 0 || error_1e_96 == 0 || error_1e_00 == 0 ||
      error_1e_01 == 0) {
    i2c_success = true;
    Serial.printf("Backlight set via I2C 0x1E (Reg 0x86 err: %d, Reg 0x96 err: "
                  "%d, Reg 0x00: %d, Reg 0x01: %d).\n",
                  error_1e_86, error_1e_96, error_1e_00, error_1e_01);
  }

  // 3. Fallback to GPIO PWM
  if (!i2c_success) {
    Serial.printf(
        "Backlight I2C failed. Initializing GPIO %d PWM fallback...\n",
        BACKLIGHT_PIN);
    ledcAttach(BACKLIGHT_PIN, 5000, 8);
    ledcWrite(BACKLIGHT_PIN, brightness_val);
  }
}

void init_dsi_display() {
  Serial.println("Creating MIPI DSI bus...");
  esp_lcd_dsi_bus_config_t bus_config;
  memset(&bus_config, 0, sizeof(bus_config));
  bus_config.bus_id = 0;
  bus_config.num_data_lanes = 2;
  bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
  bus_config.lane_bit_rate_mbps = 1000;

  esp_err_t err = esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);
  if (err != ESP_OK) {
    Serial.printf("DSI Bus creation failed! Error: 0x%x\n", err);
    return;
  }

  Serial.println("Creating DPI Panel interface...");
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

  err = esp_lcd_new_panel_dpi(mipi_dsi_bus, &dpi_config, &dpi_panel);
  if (err != ESP_OK) {
    Serial.printf("DPI Panel creation failed! Error: 0x%x\n", err);
    return;
  }

  Serial.println("Initializing panel driver...");
  ESP_ERROR_CHECK(esp_lcd_panel_init(dpi_panel));
  Serial.println("DSI/DPI panel is initialized.");
}

void init_framebuffer() {
  Serial.println("Retrieving double framebuffer pointers from driver...");
  esp_err_t err = esp_lcd_dpi_panel_get_frame_buffer(
      dpi_panel, 2, (void **)&display_fbs[0], (void **)&display_fbs[1]);
  if (err == ESP_OK && display_fbs[0] != NULL && display_fbs[1] != NULL) {
    Serial.printf("Framebuffers mapped at addresses: %p and %p\n",
                  display_fbs[0], display_fbs[1]);
  } else {
    Serial.printf("Failed to map framebuffers! Error: 0x%x\n", err);
  }
}

bool init_sd_card() {
  Serial.println("Setting SD MMC Pin mapping...");
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0, SD_MMC_D1, SD_MMC_D2,
                 SD_MMC_D3);

  Serial.println("Mounting SD card...");
  if (SD_MMC.begin("/sdcard", false, false, 20000)) {
    Serial.println("SD Card mounted successfully (4-bit 20MHz mode).");
    return true;
  }

  SD_MMC.end();
  delay(100);
  if (SD_MMC.begin("/sdcard", false)) {
    Serial.println("SD Card mounted successfully (4-bit mode).");
    return true;
  }

  SD_MMC.end();
  delay(100);
  Serial.println("4-bit mount failed. Attempting 1-bit mode fallback...");
  if (SD_MMC.begin("/sdcard", true)) {
    Serial.println("SD Card mounted successfully (1-bit mode).");
    return true;
  }

  SD_MMC.end();
  return false;
}

void init_jpeg_decoder() {
  Serial.println("Initializing Hardware JPEG Decoder Engine...");
  jpeg_decode_engine_cfg_t decode_eng_cfg;
  memset(&decode_eng_cfg, 0, sizeof(decode_eng_cfg));
  decode_eng_cfg.intr_priority = 0;
  decode_eng_cfg.timeout_ms = 1000;

  esp_err_t err = jpeg_new_decoder_engine(&decode_eng_cfg, &jpeg_decoder);
  if (err == ESP_OK) {
    Serial.println("Hardware JPEG decoder engine initialized.");
  } else {
    Serial.printf("JPEG engine initialization failed! Error: 0x%x\n", err);
  }

  memset(&jpeg_decode_cfg, 0, sizeof(jpeg_decode_cfg));
  jpeg_decode_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  jpeg_decode_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
  jpeg_decode_cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
}

void clear_screen(uint16_t color) {
  const size_t fb_pixels = 480 * 1920;
  for (int f = 0; f < 2; f++) {
    if (display_fbs[f] != NULL) {
      if (color == 0x0000) {
        memset(display_fbs[f], 0, fb_pixels * sizeof(uint16_t));
      } else {
        // Fill with uniform 16-bit color pattern
        uint16_t *fb = display_fbs[f];
        for (size_t i = 0; i < fb_pixels; i++) {
          fb[i] = color;
        }
      }
    }
  }
}

// -------------------------------------------------------------
// PLAY MJPEG AND OVERLAY TELEMETRY
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
    Serial.printf("Failed to open video file: %s after retries!\n", path);
    current_animation++;
    if (current_animation > MAX_ANIMATIONS)
      current_animation = 1;

    missing_count++;
    if (missing_count > 5) {
      current_animation =
          1; // Auto-reset back to index 1 on repeated missing files
      missing_count = 0;
      delay(500);
    }
    return;
  }
  missing_count = 0;
  Serial.printf("Playing video file: %s\n", path);

  const int BLOCK_SIZE = 4096;
  static uint8_t block_buf[BLOCK_SIZE];

  bool inside_frame = false;
  uint32_t write_pos = 0;
  uint32_t frame_count = 0;
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

    if (force_video_exit) {
      force_video_exit = false;
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
              frame_count++;

              if (Vertical) {
                // Vertical mode: 480x1920 portrait MJPEG direct copy
                memcpy(draw_fb, jpeg_output_buf, 480 * 1920 * sizeof(uint16_t));
              } else {
                // Optimized 90-degree pixel rotation (1920x480 -> 480x1920)
                // Pulls row pointer calculations outside the inner loop.
                for (int sx = 0; sx < 1920; sx++) {
                  uint16_t *dst_col = &draw_fb[sx * 480];
                  for (int sy = 0; sy < 480; sy++) {
                    dst_col[479 - sy] = jpeg_output_buf[sy * 1920 + sx];
                  }
                }
              }

              draw_telemetry_hud();

              esp_lcd_panel_draw_bitmap(dpi_panel, 0, 0, 480 - 1, 1920 - 1,
                                        draw_fb);
              current_fb_idx = 1 - current_fb_idx;

              next_frame_time_us += target_frame_time_us;
              if (paced_wait_and_check_button(next_frame_time_us)) {
                exit_video = true;
              }
            } else {
              static unsigned long last_err_print = 0;
              if (millis() - last_err_print > 3000) {
                Serial.printf("JPEG Decode Error: 0x%x! (Size: %d bytes)\n",
                              dec_err, write_pos);
                last_err_print = millis();
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
  Serial.setRxBufferSize(2048);
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n===== ESP32-P4 Cyber HUD Telemetry Receiver =====");

  telemetry_mutex = xSemaphoreCreateMutex();
  button_action_queue = xQueueCreate(4, sizeof(ButtonAction));
  if (button_action_queue == NULL) {
    Serial.println("WARNING: Failed to create button action queue!");
  }

  jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
      .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
  };
  size_t allocated_in_size = 0;
  jpeg_frame_buf = (uint8_t *)jpeg_alloc_decoder_mem(JPEG_BUF_SIZE, &tx_mem_cfg,
                                                     &allocated_in_size);
  if (jpeg_frame_buf == NULL) {
    Serial.println("CRITICAL ERROR: Failed to allocate JPEG input buffer!");
    while (1)
      ;
  }

  jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {
      .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
  };
  size_t allocated_out_size = 0;
  jpeg_output_buf = (uint16_t *)jpeg_alloc_decoder_mem(
      480 * 1920 * sizeof(uint16_t), &rx_mem_cfg, &allocated_out_size);
  if (jpeg_output_buf == NULL) {
    Serial.println("CRITICAL ERROR: Failed to allocate JPEG output buffer via "
                   "jpeg_alloc_decoder_mem!");
    while (1)
      ;
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
    Serial.println(
        "CRITICAL ERROR: MicroSD Card could not be mounted! Halting.");
    while (1)
      ;
  }

  delay(1000); // Allow SD card internal controller to stabilize after power-up

  File layoutFile = SD_MMC.open("/layout.json", FILE_READ);
  if (layoutFile) {
    DeserializationError err = deserializeJson(current_layout_doc, layoutFile);
    if (!err) {
      use_custom_layout = true;
      Serial.println("Loaded custom layout from SD.");
    }
    layoutFile.close();
  }

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
  current_layout = prefs.getInt("last_layout", 1);
  if (current_layout < 1 || current_layout > MAX_LAYOUTS)
    current_layout = 1;
  Vertical = prefs.getBool("is_vertical", true);
  prefs.end();

  Serial.printf("[NVS] Restored settings: Animation #%d, Layout #%d, Vertical mode: %s\n",
                current_animation, current_layout, Vertical ? "TRUE" : "FALSE");

  if (display_fbs[0] != NULL && display_fbs[1] != NULL) {
    clear_screen(0x0000);

    xTaskCreatePinnedToCore(telemetry_task, "telemetry_task", 4096, NULL, 2,
                            NULL, 0);
    xTaskCreatePinnedToCore(button_task, "button_task", 3072, NULL, 3, NULL, 0);

    Serial.println(
        "[SYS] Telemetry + button tasks spawned on Core 0. Display loop starting...");
  } else {
    Serial.println("[DSI] CRITICAL ERROR: Display Framebuffers are NULL!");
    while (1)
      ;
  }
}

void loop() {
  if (display_fbs[0] != NULL && display_fbs[1] != NULL) {
    if (in_live_view_mode && live_view_bg != "") {
      char filepath[64];
      snprintf(filepath, sizeof(filepath), "/%s", live_view_bg.c_str());
      play_mjpeg_video(filepath);
    } else {
      if (g_playlist.empty()) {
        scan_active_folder();
        if (g_playlist.empty()) {
          Serial.println("No MJPEG videos found on SD! Retrying in 1s...");
          delay(1000);
          init_sd_card(); // Auto-remount SD_MMC if disconnected
          return;
        }
      }

      if (current_animation < 1 || current_animation > (int)g_playlist.size()) {
        current_animation = 1;
      }

      play_mjpeg_video(g_playlist[current_animation - 1].c_str());
    }

    // Yield small delay between clips so SD_MMC driver releases file descriptors cleanly
    vTaskDelay(pdMS_TO_TICKS(20));
  } else {
    delay(1000);
  }
}
