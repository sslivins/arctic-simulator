/*
 * Display driver for M5Stack Atom S3 / S3R — Implementation
 *
 * GC9107 128×128 RGB565 LCD driven over SPI.
 * Framebuffer is double-buffered in internal DMA-capable RAM when
 * possible, falling back to regular malloc.
 *
 * Hardware variants:
 *   Atom S3R – MOSI:21 CLK:15 CS:14 DC:42 RST:48  BL: I2C 0x30
 *   Atom S3  – MOSI:21 CLK:17 CS:15 DC:33 RST:34  BL: GPIO 16
 *
 * Pin assignments come from Kconfig (Display & Button menu).
 */
#include "display.h"
#include "register_map.h"
#include "modbus_slave.h"
#include "recorder.h"
#include "playback.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <algorithm>

static const char* TAG = "display";

// ============================================================================
// LCD constants
// ============================================================================

static constexpr int LCD_W = 128;
static constexpr int LCD_H = 128;
static constexpr int CHAR_W = 6;         // 5 pixels + 1 gap
static constexpr int CHAR_H = 9;         // 7 pixels + 2 gap

// ── Colours (RGB565, byte-swapped for big-endian SPI) ──────────────────
static inline constexpr uint16_t swap16(uint16_t v) {
    return (v >> 8) | (v << 8);
}

static constexpr uint16_t COL_BLACK     = 0x0000;
static constexpr uint16_t COL_WHITE     = swap16(0xFFFF);
static constexpr uint16_t COL_RED       = swap16(0xF800);
static constexpr uint16_t COL_GREEN     = swap16(0x07E0);
static constexpr uint16_t COL_BLUE      = swap16(0x001F);
static constexpr uint16_t COL_CYAN      = swap16(0x07FF);
static constexpr uint16_t COL_YELLOW    = swap16(0xFFE0);
static constexpr uint16_t COL_ORANGE    = swap16(0xFD20);
static constexpr uint16_t COL_GRAY      = swap16(0x8410);
static constexpr uint16_t COL_DARK_GRAY = swap16(0x4208);
static constexpr uint16_t COL_DARK_BLUE = swap16(0x0010);

// ============================================================================
// 5×7 bitmap font (ASCII 32–126)
// ============================================================================

// clang-format off
static const uint8_t FONT_5x7[] = {
    0x00,0x00,0x00,0x00,0x00, // 32  (space)
    0x00,0x00,0x5F,0x00,0x00, // 33  !
    0x00,0x07,0x00,0x07,0x00, // 34  "
    0x14,0x7F,0x14,0x7F,0x14, // 35  #
    0x24,0x2A,0x7F,0x2A,0x12, // 36  $
    0x23,0x13,0x08,0x64,0x62, // 37  %
    0x36,0x49,0x55,0x22,0x50, // 38  &
    0x00,0x05,0x03,0x00,0x00, // 39  '
    0x00,0x1C,0x22,0x41,0x00, // 40  (
    0x00,0x41,0x22,0x1C,0x00, // 41  )
    0x08,0x2A,0x1C,0x2A,0x08, // 42  *
    0x08,0x08,0x3E,0x08,0x08, // 43  +
    0x00,0x50,0x30,0x00,0x00, // 44  ,
    0x08,0x08,0x08,0x08,0x08, // 45  -
    0x00,0x60,0x60,0x00,0x00, // 46  .
    0x20,0x10,0x08,0x04,0x02, // 47  /
    0x3E,0x51,0x49,0x45,0x3E, // 48  0
    0x00,0x42,0x7F,0x40,0x00, // 49  1
    0x42,0x61,0x51,0x49,0x46, // 50  2
    0x21,0x41,0x45,0x4B,0x31, // 51  3
    0x18,0x14,0x12,0x7F,0x10, // 52  4
    0x27,0x45,0x45,0x45,0x39, // 53  5
    0x3C,0x4A,0x49,0x49,0x30, // 54  6
    0x01,0x71,0x09,0x05,0x03, // 55  7
    0x36,0x49,0x49,0x49,0x36, // 56  8
    0x06,0x49,0x49,0x29,0x1E, // 57  9
    0x00,0x36,0x36,0x00,0x00, // 58  :
    0x00,0x56,0x36,0x00,0x00, // 59  ;
    0x00,0x08,0x14,0x22,0x41, // 60  <
    0x14,0x14,0x14,0x14,0x14, // 61  =
    0x41,0x22,0x14,0x08,0x00, // 62  >
    0x02,0x01,0x51,0x09,0x06, // 63  ?
    0x32,0x49,0x79,0x41,0x3E, // 64  @
    0x7E,0x11,0x11,0x11,0x7E, // 65  A
    0x7F,0x49,0x49,0x49,0x36, // 66  B
    0x3E,0x41,0x41,0x41,0x22, // 67  C
    0x7F,0x41,0x41,0x22,0x1C, // 68  D
    0x7F,0x49,0x49,0x49,0x41, // 69  E
    0x7F,0x09,0x09,0x01,0x01, // 70  F
    0x3E,0x41,0x41,0x51,0x32, // 71  G
    0x7F,0x08,0x08,0x08,0x7F, // 72  H
    0x00,0x41,0x7F,0x41,0x00, // 73  I
    0x20,0x40,0x41,0x3F,0x01, // 74  J
    0x7F,0x08,0x14,0x22,0x41, // 75  K
    0x7F,0x40,0x40,0x40,0x40, // 76  L
    0x7F,0x02,0x04,0x02,0x7F, // 77  M
    0x7F,0x04,0x08,0x10,0x7F, // 78  N
    0x3E,0x41,0x41,0x41,0x3E, // 79  O
    0x7F,0x09,0x09,0x09,0x06, // 80  P
    0x3E,0x41,0x51,0x21,0x5E, // 81  Q
    0x7F,0x09,0x19,0x29,0x46, // 82  R
    0x46,0x49,0x49,0x49,0x31, // 83  S
    0x01,0x01,0x7F,0x01,0x01, // 84  T
    0x3F,0x40,0x40,0x40,0x3F, // 85  U
    0x1F,0x20,0x40,0x20,0x1F, // 86  V
    0x3F,0x40,0x38,0x40,0x3F, // 87  W
    0x63,0x14,0x08,0x14,0x63, // 88  X
    0x07,0x08,0x70,0x08,0x07, // 89  Y
    0x61,0x51,0x49,0x45,0x43, // 90  Z
    0x00,0x00,0x7F,0x41,0x41, // 91  [
    0x02,0x04,0x08,0x10,0x20, // 92  backslash
    0x41,0x41,0x7F,0x00,0x00, // 93  ]
    0x04,0x02,0x01,0x02,0x04, // 94  ^
    0x40,0x40,0x40,0x40,0x40, // 95  _
    0x00,0x01,0x02,0x04,0x00, // 96  `
    0x20,0x54,0x54,0x54,0x78, // 97  a
    0x7F,0x48,0x44,0x44,0x38, // 98  b
    0x38,0x44,0x44,0x44,0x20, // 99  c
    0x38,0x44,0x44,0x48,0x7F, // 100 d
    0x38,0x54,0x54,0x54,0x18, // 101 e
    0x08,0x7E,0x09,0x01,0x02, // 102 f
    0x08,0x54,0x54,0x54,0x3C, // 103 g
    0x7F,0x08,0x04,0x04,0x78, // 104 h
    0x00,0x44,0x7D,0x40,0x00, // 105 i
    0x20,0x40,0x44,0x3D,0x00, // 106 j
    0x7F,0x10,0x28,0x44,0x00, // 107 k
    0x00,0x41,0x7F,0x40,0x00, // 108 l
    0x7C,0x04,0x18,0x04,0x78, // 109 m
    0x7C,0x08,0x04,0x04,0x78, // 110 n
    0x38,0x44,0x44,0x44,0x38, // 111 o
    0x7C,0x14,0x14,0x14,0x08, // 112 p
    0x08,0x14,0x14,0x18,0x7C, // 113 q
    0x7C,0x08,0x04,0x04,0x08, // 114 r
    0x48,0x54,0x54,0x54,0x20, // 115 s
    0x04,0x3F,0x44,0x40,0x20, // 116 t
    0x3C,0x40,0x40,0x20,0x7C, // 117 u
    0x1C,0x20,0x40,0x20,0x1C, // 118 v
    0x3C,0x40,0x30,0x40,0x3C, // 119 w
    0x44,0x28,0x10,0x28,0x44, // 120 x
    0x0C,0x50,0x50,0x50,0x3C, // 121 y
    0x44,0x64,0x54,0x4C,0x44, // 122 z
    0x00,0x08,0x36,0x41,0x00, // 123 {
    0x00,0x00,0x7F,0x00,0x00, // 124 |
    0x00,0x41,0x36,0x08,0x00, // 125 }
    0x10,0x08,0x08,0x10,0x08, // 126 ~
};
// clang-format on

// ============================================================================
// State
// ============================================================================
static esp_lcd_panel_handle_t s_panel = nullptr;
static esp_lcd_panel_io_handle_t s_io = nullptr;
static uint16_t* s_fb = nullptr;
static bool s_initialized = false;

// Button debounce
static bool s_btn_last_raw  = false;
static bool s_btn_last_stable = false;
static int  s_btn_debounce  = 0;
static constexpr int DEBOUNCE_TICKS = 3;  // 3 × 50 ms = 150 ms

// ============================================================================
// Low-level drawing helpers (operate on s_fb)
// ============================================================================

static void fbClear(uint16_t color) {
    for (int i = 0; i < LCD_W * LCD_H; i++) s_fb[i] = color;
}

static void fbFillRect(int x, int y, int w, int h, uint16_t color) {
    for (int j = y; j < y + h && j < LCD_H; j++)
        for (int i = x; i < x + w && i < LCD_W; i++)
            s_fb[j * LCD_W + i] = color;
}

static void fbHLine(int x, int y, int w, uint16_t color) {
    if (y < 0 || y >= LCD_H) return;
    for (int i = x; i < x + w && i < LCD_W; i++)
        if (i >= 0) s_fb[y * LCD_W + i] = color;
}

static void fbDrawChar(int x, int y, char c, uint16_t fg, uint16_t bg) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t* glyph = &FONT_5x7[(c - 32) * 5];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            int px = x + col;
            int py = y + row;
            if (px >= 0 && px < LCD_W && py >= 0 && py < LCD_H)
                s_fb[py * LCD_W + px] = (bits & (1 << row)) ? fg : bg;
        }
    }
    // 1-pixel spacing column
    for (int row = 0; row < 7; row++) {
        int px = x + 5;
        int py = y + row;
        if (px >= 0 && px < LCD_W && py >= 0 && py < LCD_H)
            s_fb[py * LCD_W + px] = bg;
    }
}

static void fbDrawStr(int x, int y, const char* str, uint16_t fg, uint16_t bg) {
    while (*str) {
        fbDrawChar(x, y, *str, fg, bg);
        x += CHAR_W;
        str++;
    }
    // Fill remaining row with background
    while (x < LCD_W) {
        for (int row = 0; row < 7; row++) {
            if (y + row >= 0 && y + row < LCD_H)
                s_fb[(y + row) * LCD_W + x] = bg;
        }
        x++;
    }
}

// Draw a filled circle (used for recording dot)
static void fbFillCircle(int cx, int cy, int r, uint16_t color) {
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                int px = cx + dx, py = cy + dy;
                if (px >= 0 && px < LCD_W && py >= 0 && py < LCD_H)
                    s_fb[py * LCD_W + px] = color;
            }
        }
    }
}

static void fbFlush() {
    if (s_panel && s_fb)
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb);
}

// ── Scaled character drawing ───────────────────────────────────────────

static void fbDrawCharScaled(int x, int y, char c, uint16_t fg, uint16_t bg,
                              int scale) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t* glyph = &FONT_5x7[(c - 32) * 5];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            uint16_t color = (bits & (1 << row)) ? fg : bg;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + col * scale + sx;
                    int py = y + row * scale + sy;
                    if (px >= 0 && px < LCD_W && py >= 0 && py < LCD_H)
                        s_fb[py * LCD_W + px] = color;
                }
            }
        }
    }
    // Spacing column(s)
    for (int sy = 0; sy < 7 * scale; sy++) {
        for (int sx = 0; sx < scale; sx++) {
            int px = x + 5 * scale + sx;
            int py = y + sy;
            if (px >= 0 && px < LCD_W && py >= 0 && py < LCD_H)
                s_fb[py * LCD_W + px] = bg;
        }
    }
}

static void fbDrawStrCentered(int y, const char* str, uint16_t fg, uint16_t bg,
                               int scale) {
    int len = (int)strlen(str);
    int charW = 6 * scale;           // (5 glyph + 1 gap) * scale
    int totalW = len * charW;
    int x = (LCD_W - totalW) / 2;
    if (x < 0) x = 0;
    for (int i = 0; i < len; i++) {
        fbDrawCharScaled(x + i * charW, y, str[i], fg, bg, scale);
    }
}

// Hollow circle (ring)
static void fbCircleRing(int cx, int cy, int r_outer, int r_inner,
                          uint16_t color) {
    int ro2 = r_outer * r_outer;
    int ri2 = r_inner * r_inner;
    for (int dy = -r_outer; dy <= r_outer; dy++) {
        for (int dx = -r_outer; dx <= r_outer; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= ro2 && d2 >= ri2) {
                int px = cx + dx, py = cy + dy;
                if (px >= 0 && px < LCD_W && py >= 0 && py < LCD_H)
                    s_fb[py * LCD_W + px] = color;
            }
        }
    }
}

// Blink state — toggled each refresh (~500 ms)
static bool s_blink_on = true;

// ============================================================================
// UI rendering
// ============================================================================

/// Provisioning mode — show AP name for WiFi setup.
static void renderProvisioningUI() {
    const esp_app_desc_t* app = esp_app_get_description();

    fbClear(COL_BLACK);

    // "Arctic Sim" at 2x scale, centered
    fbDrawStrCentered(8, "Arctic Sim", COL_WHITE, COL_BLACK, 2);

    // Version at 1x scale, centered
    char ver[36];
    snprintf(ver, sizeof(ver), "v%s", app->version);
    fbDrawStrCentered(30, ver, COL_GRAY, COL_BLACK, 1);

    // Separator
    fbHLine(16, 44, LCD_W - 32, COL_DARK_GRAY);

    // AP network name, centered, bright green
    const char* ap = wifi::getAPName();
    fbDrawStrCentered(58, ap, COL_GREEN, COL_BLACK, 1);

    // Hints
    fbDrawStrCentered(80, "No password", COL_DARK_GRAY, COL_BLACK, 1);
    fbDrawStrCentered(94, "192.168.4.1", COL_YELLOW, COL_BLACK, 1);
}

/// Normal mode — big record button.
static void renderNormalUI() {
    const esp_app_desc_t* app = esp_app_get_description();

    fbClear(COL_BLACK);

    // "Arctic Sim" at 2x scale, centered
    fbDrawStrCentered(4, "Arctic Sim", COL_WHITE, COL_BLACK, 2);

    // Version at 1x scale, centered
    char ver[36];
    snprintf(ver, sizeof(ver), "v%s", app->version);
    fbDrawStrCentered(22, ver, COL_GRAY, COL_BLACK, 1);

    if (recorder::isAvailable()) {
        // ── Big record button (iPhone-style) ───────────────────────────
        const int cx = LCD_W / 2;   // 64
        const int cy = 80;          // vertical center of button area
        const int r_fill = 32;      // inner filled circle
        const int r_ring = 36;      // outer ring
        const int ring_w = 3;       // ring thickness

        // Outer white ring — always visible
        fbCircleRing(cx, cy, r_ring, r_ring - ring_w, COL_WHITE);

        // Red filled circle — solid when idle, blinks when recording
        bool recording = recorder::isRecording();
        if (!recording || s_blink_on) {
            fbFillCircle(cx, cy, r_fill, COL_RED);
        }

        // Toggle blink for next refresh cycle
        if (recording) {
            s_blink_on = !s_blink_on;
        } else {
            s_blink_on = true;
        }

        // ── Storage bar (bottom of screen) ─────────────────────────────
        auto rec = recorder::getStatus();
        if (rec.bytes_total > 0) {
            const int bar_x = 10;
            const int bar_w = LCD_W - 20;      // 108 px
            const int bar_y = LCD_H - 8;       // y = 120
            const int bar_h = 4;

            // Outline
            fbHLine(bar_x, bar_y - 1,     bar_w, COL_DARK_GRAY);
            fbHLine(bar_x, bar_y + bar_h, bar_w, COL_DARK_GRAY);

            float pct = (float)rec.bytes_used / (float)rec.bytes_total;
            if (pct > 1.0f) pct = 1.0f;
            int fill_w = (int)(pct * bar_w);

            // Color: green < 60%, yellow < 85%, red >= 85%
            uint16_t bar_col = COL_GREEN;
            if (pct >= 0.85f) bar_col = COL_RED;
            else if (pct >= 0.60f) bar_col = COL_YELLOW;

            if (fill_w > 0) {
                fbFillRect(bar_x, bar_y, fill_w, bar_h, bar_col);
            }
        }
    } else {
        // ── No PSRAM — show web-mode indicator ─────────────────────────
        fbDrawStrCentered(60, "Web Mode", COL_CYAN, COL_BLACK, 2);
        fbDrawStrCentered(86, "Stream via API", COL_DARK_GRAY, COL_BLACK, 1);
    }
}

static void renderUI() {
    if (wifi::getMode() == wifi::Mode::PROVISIONING) {
        renderProvisioningUI();
    } else {
        renderNormalUI();
    }
}

// ============================================================================
// Public API — init
// ============================================================================

namespace display {

esp_err_t init() {
    if (s_initialized) return ESP_OK;

    ESP_LOGI(TAG, "Initializing LCD (MOSI:%d CLK:%d CS:%d DC:%d RST:%d BL:%d)",
             CONFIG_SIMULATOR_LCD_MOSI, CONFIG_SIMULATOR_LCD_CLK,
             CONFIG_SIMULATOR_LCD_CS, CONFIG_SIMULATOR_LCD_DC,
             CONFIG_SIMULATOR_LCD_RST, CONFIG_SIMULATOR_LCD_BL);

    // ── Allocate framebuffer ───────────────────────────────────────────
    s_fb = (uint16_t*)heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t),
                                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_fb) {
        s_fb = (uint16_t*)malloc(LCD_W * LCD_H * sizeof(uint16_t));
    }
    if (!s_fb) {
        ESP_LOGE(TAG, "Failed to allocate framebuffer (%d bytes)",
                 LCD_W * LCD_H * (int)sizeof(uint16_t));
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0, LCD_W * LCD_H * sizeof(uint16_t));

    // ── SPI bus ────────────────────────────────────────────────────────
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num    = CONFIG_SIMULATOR_LCD_MOSI;
    bus_cfg.sclk_io_num    = CONFIG_SIMULATOR_LCD_CLK;
    bus_cfg.miso_io_num    = -1;
    bus_cfg.quadwp_io_num  = -1;
    bus_cfg.quadhd_io_num  = -1;
    bus_cfg.max_transfer_sz = LCD_W * LCD_H * sizeof(uint16_t);

    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    // ── LCD panel IO (SPI) ─────────────────────────────────────────────
    esp_lcd_panel_io_handle_t io_handle = nullptr;
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.dc_gpio_num      = CONFIG_SIMULATOR_LCD_DC;
    io_cfg.cs_gpio_num      = CONFIG_SIMULATOR_LCD_CS;
    io_cfg.pclk_hz          = 40 * 1000 * 1000;  // 40 MHz (matches M5GFX)
    io_cfg.lcd_cmd_bits     = 8;
    io_cfg.lcd_param_bits   = 8;
    io_cfg.spi_mode         = 0;
    io_cfg.trans_queue_depth = 10;

    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg,
                                   &io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel IO init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_io = io_handle;

    // ── ST7789 panel (structural wrapper for draw_bitmap / set_gap) ────
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num  = CONFIG_SIMULATOR_LCD_RST;
    panel_cfg.rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_cfg.bits_per_pixel  = 16;

    err = esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel create failed: %s", esp_err_to_name(err));
        return err;
    }

    // Hardware reset
    esp_lcd_panel_reset(s_panel);
    vTaskDelay(pdMS_TO_TICKS(120));

    // ── Full GC9107 init (matching M5GFX Panel_GC9107 exactly) ─────────
    // We do NOT call esp_lcd_panel_init() — it sends a SW reset (0x01)
    // that wipes registers, and its ST7789 COLMOD/MADCTL values may
    // differ from what GC9107 needs.  Instead we send the complete
    // sequence from M5GFX manually.
    {
        auto cmd0 = [&](uint8_t c) {
            esp_lcd_panel_io_tx_param(io_handle, c, nullptr, 0);
        };
        auto cmd1 = [&](uint8_t c, uint8_t d) {
            esp_lcd_panel_io_tx_param(io_handle, c, &d, 1);
        };

        // Inter Register Enable
        cmd0(0xFE); vTaskDelay(pdMS_TO_TICKS(5));
        cmd0(0xEF); vTaskDelay(pdMS_TO_TICKS(5));

        // GC9107-specific config (values from M5GFX Panel_GC9107)
        cmd1(0xB0, 0xC0);
        cmd1(0xB2, 0x2F);
        cmd1(0xB3, 0x03);
        cmd1(0xB6, 0x19);
        cmd1(0xB7, 0x01);
        cmd1(0xAC, 0xCB);
        cmd1(0xAB, 0x0E);
        cmd1(0xB4, 0x04);
        cmd1(0xA8, 0x19);
        cmd1(0xB8, 0x08);

        cmd1(0xE8, 0x24);
        cmd1(0xE9, 0x48);
        cmd1(0xEA, 0x22);
        cmd1(0xC6, 0x30);
        cmd1(0xC7, 0x18);

        // Positive gamma
        {
            const uint8_t gp[] = {
                0x01,0x2B,0x23,0x3C,0xB7,0x12,0x17,0x60,
                0x00,0x06,0x0C,0x17,0x12,0x1F
            };
            esp_lcd_panel_io_tx_param(io_handle, 0xF0, gp, sizeof(gp));
        }
        // Negative gamma
        {
            const uint8_t gn[] = {
                0x05,0x2E,0x2D,0x44,0xD6,0x15,0x17,0xA0,
                0x02,0x0D,0x0D,0x1A,0x18,0x1F
            };
            esp_lcd_panel_io_tx_param(io_handle, 0xF1, gn, sizeof(gn));
        }

        // Pixel format: RGB565
        cmd1(0x3A, 0x55);

        // MADCTL: rotation 0 + BGR
        cmd1(0x36, 0x08);

        // Sleep Out
        cmd0(0x11);
        vTaskDelay(pdMS_TO_TICKS(120));

        // Display ON
        cmd0(0x29);
        vTaskDelay(pdMS_TO_TICKS(20));

        ESP_LOGI(TAG, "GC9107 init sequence complete (M5GFX-matched)");
    }

    // GC9107 offset: 128×128 window at (0, 32) inside 128×160 memory
    esp_lcd_panel_set_gap(s_panel, 0, 32);
    // No display inversion — GC9107 defaults are correct

    // ── Backlight ON ───────────────────────────────────────────────────
    // Atom S3R: I2C LED controller at 0x30 (SDA=45, SCL=0)
    // Atom S3:  PWM on GPIO 16 (fallback)
    {
        bool bl_ok = false;
#ifdef CONFIG_SIMULATOR_LCD_BL_I2C
        // I2C backlight (Atom S3R)
        i2c_config_t i2c_cfg = {};
        i2c_cfg.mode             = I2C_MODE_MASTER;
        i2c_cfg.sda_io_num       = (gpio_num_t)CONFIG_SIMULATOR_LCD_BL_I2C_SDA;
        i2c_cfg.scl_io_num       = (gpio_num_t)CONFIG_SIMULATOR_LCD_BL_I2C_SCL;
        i2c_cfg.sda_pullup_en    = GPIO_PULLUP_ENABLE;
        i2c_cfg.scl_pullup_en    = GPIO_PULLUP_ENABLE;
        i2c_cfg.master.clk_speed = 400000;

        if (i2c_param_config(I2C_NUM_1, &i2c_cfg) == ESP_OK &&
            i2c_driver_install(I2C_NUM_1, I2C_MODE_MASTER, 0, 0, 0) == ESP_OK)
        {
            auto bl_reg = [](uint8_t reg, uint8_t val) -> esp_err_t {
                uint8_t buf[2] = { reg, val };
                return i2c_master_write_to_device(I2C_NUM_1, 0x30,
                                                  buf, sizeof(buf),
                                                  pdMS_TO_TICKS(100));
            };
            bl_reg(0x00, 0x40);              // soft reset
            vTaskDelay(pdMS_TO_TICKS(2));
            bl_reg(0x08, 0x01);              // enable output
            bl_reg(0x70, 0x00);              // direct PWM mode
            bl_reg(0x0E, 0x80);              // brightness ~50%
            bl_ok = true;
            ESP_LOGI(TAG, "Backlight: I2C (0x30) brightness=128");
        } else {
            ESP_LOGW(TAG, "I2C backlight init failed, trying GPIO fallback");
        }
#endif
        if (!bl_ok) {
            // GPIO backlight fallback (Atom S3 / custom boards)
            gpio_config_t bl = {};
            bl.pin_bit_mask = 1ULL << CONFIG_SIMULATOR_LCD_BL;
            bl.mode         = GPIO_MODE_OUTPUT;
            gpio_config(&bl);
            gpio_set_level((gpio_num_t)CONFIG_SIMULATOR_LCD_BL, 1);
            ESP_LOGI(TAG, "Backlight: GPIO %d HIGH", CONFIG_SIMULATOR_LCD_BL);
        }
    }

    // ── Button input (active-low with pull-up) ─────────────────────────
    gpio_config_t btn = {};
    btn.pin_bit_mask = 1ULL << CONFIG_SIMULATOR_BTN_GPIO;
    btn.mode         = GPIO_MODE_INPUT;
    btn.pull_up_en   = GPIO_PULLUP_ENABLE;
    btn.pull_down_en = GPIO_PULLDOWN_DISABLE;
    btn.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&btn);

    s_initialized = true;
    ESP_LOGI(TAG, "LCD initialised (%dx%d), button on GPIO %d",
             LCD_W, LCD_H, CONFIG_SIMULATOR_BTN_GPIO);

    // Draw initial frame
    renderUI();
    fbFlush();

    return ESP_OK;
}

// ============================================================================
// Public API — refresh
// ============================================================================

void refresh() {
    if (!s_initialized) return;
    renderUI();
    fbFlush();
}

// ============================================================================
// Public API — button polling
// ============================================================================

bool checkButton() {
    if (!s_initialized) return false;

    bool raw = (gpio_get_level((gpio_num_t)CONFIG_SIMULATOR_BTN_GPIO) == 0);

    if (raw != s_btn_last_raw) {
        s_btn_debounce = 0;
        s_btn_last_raw = raw;
    } else {
        if (s_btn_debounce < DEBOUNCE_TICKS) {
            s_btn_debounce++;
        }
    }

    bool stable = s_btn_last_stable;
    if (s_btn_debounce >= DEBOUNCE_TICKS) {
        stable = raw;
    }

    // Detect rising edge of "pressed" (stable transitions true)
    bool pressed = (stable && !s_btn_last_stable);
    s_btn_last_stable = stable;
    return pressed;
}

}  // namespace display
