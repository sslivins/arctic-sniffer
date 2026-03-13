/*
 * Display driver for M5Stack Atom S3 / S3R
 *
 * GC9107 128×128 LCD via SPI.  Uses the esp_lcd ST7789 panel driver
 * as a structural wrapper with a custom GC9107 init sequence.
 *
 * Backlight: I2C LED controller at 0x30 (Atom S3R) with GPIO fallback
 * (Atom S3).  Button: active-low with internal pull-up, polled with
 * debounce.
 */

#include "display.h"

#include <cstring>
#include <cstdio>

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

static const char *TAG = "display";

namespace display {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int LCD_W  = 128;
static constexpr int LCD_H  = 128;
static constexpr int CHAR_W = 6;   // 5 pixels + 1 gap
static constexpr int CHAR_H = 9;   // 7 pixels + 2 gap

// RGB565 colours
static constexpr uint16_t COL_BLACK   = 0x0000;
static constexpr uint16_t COL_WHITE   = 0xFFFF;
static constexpr uint16_t COL_RED     = 0xF800;
static constexpr uint16_t COL_GREEN   = 0x07E0;
static constexpr uint16_t COL_GREY    = 0x7BEF;
static constexpr uint16_t COL_DKGREY  = 0x39E7;
static constexpr uint16_t COL_CYAN    = 0x07FF;

// ---------------------------------------------------------------------------
// 5×7 bitmap font (ASCII 0x20–0x7E)
// ---------------------------------------------------------------------------

static const uint8_t FONT_5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 0x20 ' '
    {0x00,0x00,0x5F,0x00,0x00}, // 0x21 '!'
    {0x00,0x07,0x00,0x07,0x00}, // 0x22 '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // 0x23 '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // 0x24 '$'
    {0x23,0x13,0x08,0x64,0x62}, // 0x25 '%'
    {0x36,0x49,0x55,0x22,0x50}, // 0x26 '&'
    {0x00,0x05,0x03,0x00,0x00}, // 0x27 '''
    {0x00,0x1C,0x22,0x41,0x00}, // 0x28 '('
    {0x00,0x41,0x22,0x1C,0x00}, // 0x29 ')'
    {0x08,0x2A,0x1C,0x2A,0x08}, // 0x2A '*'
    {0x08,0x08,0x3E,0x08,0x08}, // 0x2B '+'
    {0x00,0x50,0x30,0x00,0x00}, // 0x2C ','
    {0x08,0x08,0x08,0x08,0x08}, // 0x2D '-'
    {0x00,0x60,0x60,0x00,0x00}, // 0x2E '.'
    {0x20,0x10,0x08,0x04,0x02}, // 0x2F '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // 0x30 '0'
    {0x00,0x42,0x7F,0x40,0x00}, // 0x31 '1'
    {0x42,0x61,0x51,0x49,0x46}, // 0x32 '2'
    {0x21,0x41,0x45,0x4B,0x31}, // 0x33 '3'
    {0x18,0x14,0x12,0x7F,0x10}, // 0x34 '4'
    {0x27,0x45,0x45,0x45,0x39}, // 0x35 '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // 0x36 '6'
    {0x01,0x71,0x09,0x05,0x03}, // 0x37 '7'
    {0x36,0x49,0x49,0x49,0x36}, // 0x38 '8'
    {0x06,0x49,0x49,0x29,0x1E}, // 0x39 '9'
    {0x00,0x36,0x36,0x00,0x00}, // 0x3A ':'
    {0x00,0x56,0x36,0x00,0x00}, // 0x3B ';'
    {0x00,0x08,0x14,0x22,0x41}, // 0x3C '<'
    {0x14,0x14,0x14,0x14,0x14}, // 0x3D '='
    {0x41,0x22,0x14,0x08,0x00}, // 0x3E '>'
    {0x02,0x01,0x51,0x09,0x06}, // 0x3F '?'
    {0x32,0x49,0x79,0x41,0x3E}, // 0x40 '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 0x41 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 0x42 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 0x43 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 0x44 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 0x45 'E'
    {0x7F,0x09,0x09,0x01,0x01}, // 0x46 'F'
    {0x3E,0x41,0x41,0x51,0x32}, // 0x47 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 0x48 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 0x49 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 0x4A 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 0x4B 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 0x4C 'L'
    {0x7F,0x02,0x04,0x02,0x7F}, // 0x4D 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 0x4E 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 0x4F 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 0x50 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 0x51 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 0x52 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 0x53 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 0x54 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 0x55 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 0x56 'V'
    {0x7F,0x20,0x18,0x20,0x7F}, // 0x57 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 0x58 'X'
    {0x03,0x04,0x78,0x04,0x03}, // 0x59 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 0x5A 'Z'
    {0x00,0x00,0x7F,0x41,0x41}, // 0x5B '['
    {0x02,0x04,0x08,0x10,0x20}, // 0x5C '\'
    {0x41,0x41,0x7F,0x00,0x00}, // 0x5D ']'
    {0x04,0x02,0x01,0x02,0x04}, // 0x5E '^'
    {0x40,0x40,0x40,0x40,0x40}, // 0x5F '_'
    {0x00,0x01,0x02,0x04,0x00}, // 0x60 '`'
    {0x20,0x54,0x54,0x54,0x78}, // 0x61 'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 0x62 'b'
    {0x38,0x44,0x44,0x44,0x20}, // 0x63 'c'
    {0x38,0x44,0x44,0x48,0x7F}, // 0x64 'd'
    {0x38,0x54,0x54,0x54,0x18}, // 0x65 'e'
    {0x08,0x7E,0x09,0x01,0x02}, // 0x66 'f'
    {0x08,0x14,0x54,0x54,0x3C}, // 0x67 'g'
    {0x7F,0x08,0x04,0x04,0x78}, // 0x68 'h'
    {0x00,0x44,0x7D,0x40,0x00}, // 0x69 'i'
    {0x20,0x40,0x44,0x3D,0x00}, // 0x6A 'j'
    {0x00,0x7F,0x10,0x28,0x44}, // 0x6B 'k'
    {0x00,0x41,0x7F,0x40,0x00}, // 0x6C 'l'
    {0x7C,0x04,0x18,0x04,0x78}, // 0x6D 'm'
    {0x7C,0x08,0x04,0x04,0x78}, // 0x6E 'n'
    {0x38,0x44,0x44,0x44,0x38}, // 0x6F 'o'
    {0x7C,0x14,0x14,0x14,0x08}, // 0x70 'p'
    {0x08,0x14,0x14,0x18,0x7C}, // 0x71 'q'
    {0x7C,0x08,0x04,0x04,0x08}, // 0x72 'r'
    {0x48,0x54,0x54,0x54,0x20}, // 0x73 's'
    {0x04,0x3F,0x44,0x40,0x20}, // 0x74 't'
    {0x3C,0x40,0x40,0x20,0x7C}, // 0x75 'u'
    {0x1C,0x20,0x40,0x20,0x1C}, // 0x76 'v'
    {0x3C,0x40,0x30,0x40,0x3C}, // 0x77 'w'
    {0x44,0x28,0x10,0x28,0x44}, // 0x78 'x'
    {0x0C,0x50,0x50,0x50,0x3C}, // 0x79 'y'
    {0x44,0x64,0x54,0x4C,0x44}, // 0x7A 'z'
    {0x00,0x08,0x36,0x41,0x00}, // 0x7B '{'
    {0x00,0x00,0x7F,0x00,0x00}, // 0x7C '|'
    {0x00,0x41,0x36,0x08,0x00}, // 0x7D '}'
    {0x08,0x08,0x2A,0x1C,0x08}, // 0x7E '~'
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static esp_lcd_panel_handle_t s_panel = nullptr;
static esp_lcd_panel_io_handle_t s_io = nullptr;
static uint16_t *s_fb = nullptr;
static bool s_initialized = false;

// Blink state for recording indicator
static bool s_blink_on = false;

// Button debounce
static bool s_btn_last_raw    = false;
static bool s_btn_last_stable = false;
static int  s_btn_debounce    = 0;
static constexpr int DEBOUNCE_TICKS = 3;  // 3 × 50 ms = 150 ms

// ---------------------------------------------------------------------------
// Framebuffer helpers
// ---------------------------------------------------------------------------

static void fb_clear(uint16_t col)
{
    for (int i = 0; i < LCD_W * LCD_H; i++) s_fb[i] = col;
}

static void fb_pixel(int x, int y, uint16_t col)
{
    if (x >= 0 && x < LCD_W && y >= 0 && y < LCD_H)
        s_fb[y * LCD_W + x] = col;
}

static void fb_char(int x, int y, char c, uint16_t fg, int scale = 1)
{
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *glyph = FONT_5x7[c - 0x20];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        fb_pixel(x + col * scale + sx,
                                 y + row * scale + sy, fg);
            }
        }
    }
}

static void fb_string(int x, int y, const char *s, uint16_t fg, int scale = 1)
{
    while (*s) {
        fb_char(x, y, *s, fg, scale);
        x += CHAR_W * scale;
        s++;
    }
}

static void fb_rect(int x, int y, int w, int h, uint16_t col)
{
    for (int ry = y; ry < y + h && ry < LCD_H; ry++)
        for (int rx = x; rx < x + w && rx < LCD_W; rx++)
            fb_pixel(rx, ry, col);
}

/// Draw a filled circle (for recording dot)
static void fb_circle(int cx, int cy, int r, uint16_t col)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                fb_pixel(cx + dx, cy + dy, col);
}

static void fb_flush()
{
    if (s_panel && s_fb)
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb);
}

// ---------------------------------------------------------------------------
// LCD command helpers
// ---------------------------------------------------------------------------

static void cmd0(uint8_t cmd)
{
    esp_lcd_panel_io_tx_param(s_io, cmd, nullptr, 0);
}

static void cmd1(uint8_t cmd, uint8_t val)
{
    esp_lcd_panel_io_tx_param(s_io, cmd, &val, 1);
}

// ---------------------------------------------------------------------------
// Backlight helpers (I2C for S3R, GPIO fallback for S3)
// ---------------------------------------------------------------------------

#ifdef CONFIG_SNIFFER_LCD_BL_I2C
static bool s_bl_i2c_ok = false;

static void bl_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    i2c_master_write_to_device(I2C_NUM_1, 0x30, buf, 2, pdMS_TO_TICKS(100));
}
#endif

// ---------------------------------------------------------------------------
// init()
// ---------------------------------------------------------------------------

esp_err_t init()
{
    // Allocate framebuffer — prefer DMA-capable internal RAM
    s_fb = (uint16_t *)heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t),
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_fb) s_fb = (uint16_t *)malloc(LCD_W * LCD_H * sizeof(uint16_t));
    if (!s_fb) {
        ESP_LOGE(TAG, "Failed to allocate framebuffer");
        return ESP_ERR_NO_MEM;
    }

    // SPI bus
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num     = CONFIG_SNIFFER_LCD_MOSI;
    bus_cfg.sclk_io_num     = CONFIG_SNIFFER_LCD_CLK;
    bus_cfg.miso_io_num     = -1;
    bus_cfg.quadwp_io_num   = -1;
    bus_cfg.quadhd_io_num   = -1;
    bus_cfg.max_transfer_sz = LCD_W * LCD_H * sizeof(uint16_t);
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    // LCD panel IO (SPI)
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.dc_gpio_num      = CONFIG_SNIFFER_LCD_DC;
    io_cfg.cs_gpio_num      = CONFIG_SNIFFER_LCD_CS;
    io_cfg.pclk_hz          = 40 * 1000 * 1000;
    io_cfg.lcd_cmd_bits     = 8;
    io_cfg.lcd_param_bits   = 8;
    io_cfg.spi_mode         = 0;
    io_cfg.trans_queue_depth = 10;
    esp_lcd_panel_io_handle_t io_handle = nullptr;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                   &io_cfg, &io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD IO init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_io = io_handle;

    // ST7789 panel wrapper (we'll send GC9107 init ourselves)
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num  = CONFIG_SNIFFER_LCD_RST;
    panel_cfg.rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_cfg.bits_per_pixel  = 16;
    err = esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_reset(s_panel);
    vTaskDelay(pdMS_TO_TICKS(100));

    // GC9107 init sequence (matching M5GFX Panel_GC9107)
    cmd0(0xFE); cmd0(0xEF);  // Inter Register Enable
    cmd1(0xB0, 0xC0); cmd1(0xB2, 0x2F); cmd1(0xB3, 0x03);
    cmd1(0xB6, 0x19); cmd1(0xB7, 0x01); cmd1(0xAC, 0xCB);
    cmd1(0xAB, 0x0E); cmd1(0xB4, 0x04); cmd1(0xA8, 0x19);
    cmd1(0xB8, 0x08); cmd1(0xE8, 0x24); cmd1(0xE9, 0x48);
    cmd1(0xEA, 0x22); cmd1(0xC6, 0x30); cmd1(0xC7, 0x18);
    // Gamma
    uint8_t gamma_pos[] = {0x1F, 0x28, 0x04, 0x3E, 0x2A, 0x2E, 0x20, 0x00,
                           0x0C, 0x06, 0x27, 0x28};
    esp_lcd_panel_io_tx_param(s_io, 0xF0, gamma_pos, sizeof(gamma_pos));
    uint8_t gamma_neg[] = {0x00, 0x04, 0x04, 0x07, 0x05, 0x25, 0x2D, 0x44,
                           0x45, 0x52, 0x26, 0x2E};
    esp_lcd_panel_io_tx_param(s_io, 0xF1, gamma_neg, sizeof(gamma_neg));

    cmd1(0x3A, 0x55);  // Pixel format: RGB565
    cmd1(0x36, 0x08);  // MADCTL: rotation 0 + BGR
    cmd0(0x11);        // Sleep Out
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd0(0x29);        // Display ON

    esp_lcd_panel_set_gap(s_panel, 0, 32);  // GC9107 128×128 at offset (0,32)

    // --- Backlight ---
    bool bl_ok = false;
#ifdef CONFIG_SNIFFER_LCD_BL_I2C
    {
        // I2C backlight (Atom S3R) — LED driver at 0x30
        i2c_config_t i2c_cfg = {};
        i2c_cfg.mode = I2C_MODE_MASTER;
        i2c_cfg.sda_io_num = (gpio_num_t)CONFIG_SNIFFER_LCD_BL_I2C_SDA;
        i2c_cfg.scl_io_num = (gpio_num_t)CONFIG_SNIFFER_LCD_BL_I2C_SCL;
        i2c_cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
        i2c_cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
        i2c_cfg.master.clk_speed = 400000;
        if (i2c_param_config(I2C_NUM_1, &i2c_cfg) == ESP_OK &&
            i2c_driver_install(I2C_NUM_1, I2C_MODE_MASTER, 0, 0, 0) == ESP_OK) {
            bl_reg(0x00, 0x40);   // soft reset
            vTaskDelay(pdMS_TO_TICKS(10));
            bl_reg(0x08, 0x01);   // enable output
            bl_reg(0x70, 0x00);   // direct PWM mode
            bl_reg(0x0E, 0x80);   // brightness ~50%
            s_bl_i2c_ok = true;
            bl_ok = true;
            ESP_LOGI(TAG, "I2C backlight enabled");
        }
    }
#endif
    if (!bl_ok) {
        // GPIO backlight fallback (Atom S3)
        gpio_config_t bl_cfg = {};
        bl_cfg.pin_bit_mask = 1ULL << CONFIG_SNIFFER_LCD_BL;
        bl_cfg.mode = GPIO_MODE_OUTPUT;
        gpio_config(&bl_cfg);
        gpio_set_level((gpio_num_t)CONFIG_SNIFFER_LCD_BL, 1);
        ESP_LOGI(TAG, "GPIO backlight enabled");
    }

    // --- Button ---
    gpio_config_t btn = {};
    btn.pin_bit_mask = 1ULL << CONFIG_SNIFFER_BTN_GPIO;
    btn.mode         = GPIO_MODE_INPUT;
    btn.pull_up_en   = GPIO_PULLUP_ENABLE;
    btn.pull_down_en = GPIO_PULLDOWN_DISABLE;
    btn.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&btn);

    s_initialized = true;

    // Initial splash
    fb_clear(COL_BLACK);
    fb_string(16, 50, "Arctic", COL_CYAN, 2);
    fb_string(10, 72, "Sniffer", COL_WHITE, 2);
    fb_flush();

    ESP_LOGI(TAG, "Display initialized (128x128 GC9107)");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// refresh() — redraw the status UI
// ---------------------------------------------------------------------------

void refresh(const char *ip, bool recording, size_t rec_used, size_t rec_limit,
             size_t rec_entries)
{
    if (!s_initialized) return;

    // Toggle blink state each refresh (~500ms)
    s_blink_on = !s_blink_on;

    fb_clear(COL_BLACK);

    // App name — top
    fb_string(4, 4, "Arctic Sniffer", COL_CYAN, 1);

    // Version
    const esp_app_desc_t *app = esp_app_get_description();
    char ver[24];
    snprintf(ver, sizeof(ver), "v%s", app->version);
    fb_string(4, 16, ver, COL_GREY, 1);

    // Separator line
    fb_rect(4, 28, 120, 1, COL_DKGREY);

    // IP address
    if (ip && ip[0]) {
        fb_string(4, 34, "IP:", COL_GREY, 1);
        fb_string(22, 34, ip, COL_WHITE, 1);
    } else {
        fb_string(4, 34, "No WiFi", COL_RED, 1);
    }

    // Separator
    fb_rect(4, 46, 120, 1, COL_DKGREY);

    // Recording status
    if (recording) {
        // Blinking red circle
        if (s_blink_on) {
            fb_circle(12, 58, 5, COL_RED);
        }
        fb_string(22, 54, "REC", COL_RED, 1);

        // Entry count
        char entries_str[32];
        snprintf(entries_str, sizeof(entries_str), "%u entries",
                 (unsigned)rec_entries);
        fb_string(4, 68, entries_str, COL_WHITE, 1);

        // Memory usage bar
        fb_string(4, 82, "Memory:", COL_GREY, 1);

        // Progress bar background
        int bar_x = 4;
        int bar_y = 94;
        int bar_w = 120;
        int bar_h = 10;
        fb_rect(bar_x, bar_y, bar_w, bar_h, COL_DKGREY);

        // Progress bar fill
        int pct = rec_limit > 0 ? (int)((rec_used * 100) / rec_limit) : 0;
        if (pct > 100) pct = 100;
        int fill_w = (bar_w * pct) / 100;
        uint16_t bar_col = pct < 80 ? COL_GREEN : COL_RED;
        fb_rect(bar_x, bar_y, fill_w, bar_h, bar_col);

        // Percentage text
        char pct_str[16];
        snprintf(pct_str, sizeof(pct_str), "%d%%", pct);
        // Center the percentage text on the bar
        int text_x = bar_x + (bar_w - (int)strlen(pct_str) * CHAR_W) / 2;
        fb_string(text_x, bar_y + 2, pct_str, COL_WHITE, 1);

        // Remaining bytes
        size_t remaining = rec_limit > rec_used ? rec_limit - rec_used : 0;
        char rem_str[32];
        if (remaining >= 1024) {
            snprintf(rem_str, sizeof(rem_str), "%uKB free",
                     (unsigned)(remaining / 1024));
        } else {
            snprintf(rem_str, sizeof(rem_str), "%uB free",
                     (unsigned)remaining);
        }
        fb_string(4, 108, rem_str, COL_GREY, 1);

    } else {
        fb_string(4, 54, "Ready", COL_GREEN, 1);
        fb_string(4, 68, "Press btn", COL_GREY, 1);
        fb_string(4, 78, "to record", COL_GREY, 1);
    }

    fb_flush();
}

// ---------------------------------------------------------------------------
// button_pressed() — debounced poll
// ---------------------------------------------------------------------------

bool button_pressed()
{
    if (!s_initialized) return false;

    bool raw = (gpio_get_level((gpio_num_t)CONFIG_SNIFFER_BTN_GPIO) == 0);
    if (raw != s_btn_last_raw) {
        s_btn_debounce = 0;
        s_btn_last_raw = raw;
    } else {
        if (s_btn_debounce < DEBOUNCE_TICKS) s_btn_debounce++;
    }

    bool stable = s_btn_last_stable;
    if (s_btn_debounce >= DEBOUNCE_TICKS) stable = raw;

    bool pressed = (stable && !s_btn_last_stable);
    s_btn_last_stable = stable;
    return pressed;
}

}  // namespace display
