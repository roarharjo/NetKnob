/* display.cpp — ST77916 QSPI driver + LVGL 9 setup + shared utilities
 *
 * Section A: Raw ESP-IDF SPI master driver for ST77916 360x360 QSPI panel
 * Section B: LVGL 9.2 display integration
 * Section C: Splash screen + shared RSSI helpers
 */

#include <Arduino.h>
#include <lvgl.h>
#include <driver/spi_master.h>
#include <soc/gpio_struct.h>
#include <esp_heap_caps.h>

#include "pins.h"
#include "display.h"

// ============================================================================
// Section A: QSPI Hardware Driver
// ============================================================================

#define LCD_HOST    SPI2_HOST
#define SPI_FREQ_HZ (40 * 1000 * 1000)  // 40 MHz

static spi_device_handle_t spi_dev;

// --- LCD init command table (Waveshare-specific, must be exact) ---

struct LcdInitCmd {
    uint8_t reg;
    uint8_t data[14];
    uint8_t len;
    uint16_t delay_ms;
};

static const LcdInitCmd lcd_init_cmds[] = {
    {0xF0, {0x28}, 1, 0},
    {0xF2, {0x28}, 1, 0},
    {0x73, {0xF0}, 1, 0},
    {0x7C, {0xD1}, 1, 0},
    {0x83, {0xE0}, 1, 0},
    {0x84, {0x61}, 1, 0},
    {0xF2, {0x82}, 1, 0},
    {0xF0, {0x00}, 1, 0},
    {0xF0, {0x01}, 1, 0},
    {0xF1, {0x01}, 1, 0},
    {0xB0, {0x56}, 1, 0},
    {0xB1, {0x4D}, 1, 0},
    {0xB2, {0x24}, 1, 0},
    {0xB4, {0x87}, 1, 0},
    {0xB5, {0x44}, 1, 0},
    {0xB6, {0x8B}, 1, 0},
    {0xB7, {0x40}, 1, 0},
    {0xB8, {0x86}, 1, 0},
    {0xBA, {0x00}, 1, 0},
    {0xBB, {0x08}, 1, 0},
    {0xBC, {0x08}, 1, 0},
    {0xBD, {0x00}, 1, 0},
    {0xC0, {0x80}, 1, 0},
    {0xC1, {0x10}, 1, 0},
    {0xC2, {0x37}, 1, 0},
    {0xC3, {0x80}, 1, 0},
    {0xC4, {0x10}, 1, 0},
    {0xC5, {0x37}, 1, 0},
    {0xC6, {0xA9}, 1, 0},
    {0xC7, {0x41}, 1, 0},
    {0xC8, {0x01}, 1, 0},
    {0xC9, {0xA9}, 1, 0},
    {0xCA, {0x41}, 1, 0},
    {0xCB, {0x01}, 1, 0},
    {0xD0, {0x91}, 1, 0},
    {0xD1, {0x68}, 1, 0},
    {0xD2, {0x68}, 1, 0},
    {0xF5, {0x00, 0xA5}, 2, 0},
    {0xDD, {0x4F}, 1, 0},
    {0xDE, {0x4F}, 1, 0},
    {0xF1, {0x10}, 1, 0},
    {0xF0, {0x00}, 1, 0},
    {0xF0, {0x02}, 1, 0},
    {0xE0, {0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, {0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, {0x10}, 1, 0},
    {0xF3, {0x10}, 1, 0},
    {0xE0, {0x07}, 1, 0},
    {0xE1, {0x00}, 1, 0},
    {0xE2, {0x00}, 1, 0},
    {0xE3, {0x00}, 1, 0},
    {0xE4, {0xE0}, 1, 0},
    {0xE5, {0x06}, 1, 0},
    {0xE6, {0x21}, 1, 0},
    {0xE7, {0x01}, 1, 0},
    {0xE8, {0x05}, 1, 0},
    {0xE9, {0x02}, 1, 0},
    {0xEA, {0xDA}, 1, 0},
    {0xEB, {0x00}, 1, 0},
    {0xEC, {0x00}, 1, 0},
    {0xED, {0x0F}, 1, 0},
    {0xEE, {0x00}, 1, 0},
    {0xEF, {0x00}, 1, 0},
    {0xF8, {0x00}, 1, 0},
    {0xF9, {0x00}, 1, 0},
    {0xFA, {0x00}, 1, 0},
    {0xFB, {0x00}, 1, 0},
    {0xFC, {0x00}, 1, 0},
    {0xFD, {0x00}, 1, 0},
    {0xFE, {0x00}, 1, 0},
    {0xFF, {0x00}, 1, 0},
    {0x60, {0x40}, 1, 0},
    {0x61, {0x04}, 1, 0},
    {0x62, {0x00}, 1, 0},
    {0x63, {0x42}, 1, 0},
    {0x64, {0xD9}, 1, 0},
    {0x65, {0x00}, 1, 0},
    {0x66, {0x00}, 1, 0},
    {0x67, {0x00}, 1, 0},
    {0x68, {0x00}, 1, 0},
    {0x69, {0x00}, 1, 0},
    {0x6A, {0x00}, 1, 0},
    {0x6B, {0x00}, 1, 0},
    {0x70, {0x40}, 1, 0},
    {0x71, {0x03}, 1, 0},
    {0x72, {0x00}, 1, 0},
    {0x73, {0x42}, 1, 0},
    {0x74, {0xD8}, 1, 0},
    {0x75, {0x00}, 1, 0},
    {0x76, {0x00}, 1, 0},
    {0x77, {0x00}, 1, 0},
    {0x78, {0x00}, 1, 0},
    {0x79, {0x00}, 1, 0},
    {0x7A, {0x00}, 1, 0},
    {0x7B, {0x00}, 1, 0},
    {0x80, {0x48}, 1, 0},
    {0x81, {0x00}, 1, 0},
    {0x82, {0x06}, 1, 0},
    {0x83, {0x02}, 1, 0},
    {0x84, {0xD6}, 1, 0},
    {0x85, {0x04}, 1, 0},
    {0x86, {0x00}, 1, 0},
    {0x87, {0x00}, 1, 0},
    {0x88, {0x48}, 1, 0},
    {0x89, {0x00}, 1, 0},
    {0x8A, {0x08}, 1, 0},
    {0x8B, {0x02}, 1, 0},
    {0x8C, {0xD8}, 1, 0},
    {0x8D, {0x04}, 1, 0},
    {0x8E, {0x00}, 1, 0},
    {0x8F, {0x00}, 1, 0},
    {0x90, {0x48}, 1, 0},
    {0x91, {0x00}, 1, 0},
    {0x92, {0x0A}, 1, 0},
    {0x93, {0x02}, 1, 0},
    {0x94, {0xDA}, 1, 0},
    {0x95, {0x04}, 1, 0},
    {0x96, {0x00}, 1, 0},
    {0x97, {0x00}, 1, 0},
    {0x98, {0x48}, 1, 0},
    {0x99, {0x00}, 1, 0},
    {0x9A, {0x0C}, 1, 0},
    {0x9B, {0x02}, 1, 0},
    {0x9C, {0xDC}, 1, 0},
    {0x9D, {0x04}, 1, 0},
    {0x9E, {0x00}, 1, 0},
    {0x9F, {0x00}, 1, 0},
    {0xA0, {0x48}, 1, 0},
    {0xA1, {0x00}, 1, 0},
    {0xA2, {0x05}, 1, 0},
    {0xA3, {0x02}, 1, 0},
    {0xA4, {0xD5}, 1, 0},
    {0xA5, {0x04}, 1, 0},
    {0xA6, {0x00}, 1, 0},
    {0xA7, {0x00}, 1, 0},
    {0xA8, {0x48}, 1, 0},
    {0xA9, {0x00}, 1, 0},
    {0xAA, {0x07}, 1, 0},
    {0xAB, {0x02}, 1, 0},
    {0xAC, {0xD7}, 1, 0},
    {0xAD, {0x04}, 1, 0},
    {0xAE, {0x00}, 1, 0},
    {0xAF, {0x00}, 1, 0},
    {0xB0, {0x48}, 1, 0},
    {0xB1, {0x00}, 1, 0},
    {0xB2, {0x09}, 1, 0},
    {0xB3, {0x02}, 1, 0},
    {0xB4, {0xD9}, 1, 0},
    {0xB5, {0x04}, 1, 0},
    {0xB6, {0x00}, 1, 0},
    {0xB7, {0x00}, 1, 0},
    {0xB8, {0x48}, 1, 0},
    {0xB9, {0x00}, 1, 0},
    {0xBA, {0x0B}, 1, 0},
    {0xBB, {0x02}, 1, 0},
    {0xBC, {0xDB}, 1, 0},
    {0xBD, {0x04}, 1, 0},
    {0xBE, {0x00}, 1, 0},
    {0xBF, {0x00}, 1, 0},
    {0xC0, {0x10}, 1, 0},
    {0xC1, {0x47}, 1, 0},
    {0xC2, {0x56}, 1, 0},
    {0xC3, {0x65}, 1, 0},
    {0xC4, {0x74}, 1, 0},
    {0xC5, {0x88}, 1, 0},
    {0xC6, {0x99}, 1, 0},
    {0xC7, {0x01}, 1, 0},
    {0xC8, {0xBB}, 1, 0},
    {0xC9, {0xAA}, 1, 0},
    {0xD0, {0x10}, 1, 0},
    {0xD1, {0x47}, 1, 0},
    {0xD2, {0x56}, 1, 0},
    {0xD3, {0x65}, 1, 0},
    {0xD4, {0x74}, 1, 0},
    {0xD5, {0x88}, 1, 0},
    {0xD6, {0x99}, 1, 0},
    {0xD7, {0x01}, 1, 0},
    {0xD8, {0xBB}, 1, 0},
    {0xD9, {0xAA}, 1, 0},
    {0xF3, {0x01}, 1, 0},
    {0xF0, {0x00}, 1, 0},
    {0x21, {0x00}, 1, 0},
    {0x11, {0x00}, 1, 120},
    {0x29, {0x00}, 1, 0},
    {0x36, {0x00}, 1, 0},
};

// --- Manual CS control ---

static inline void cs_low()  { GPIO.out_w1tc = (1UL << PIN_LCD_CS); }
static inline void cs_high() { GPIO.out_w1ts = (1UL << PIN_LCD_CS); }

// --- SPI bus and device init ---

static void spi_init()
{
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num    = PIN_LCD_D0;
    bus_cfg.miso_io_num    = PIN_LCD_D1;
    bus_cfg.sclk_io_num    = PIN_LCD_CLK;
    bus_cfg.quadwp_io_num  = PIN_LCD_D2;
    bus_cfg.quadhd_io_num  = PIN_LCD_D3;
    bus_cfg.max_transfer_sz = 65536;
    bus_cfg.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.clock_speed_hz = SPI_FREQ_HZ;
    dev_cfg.mode           = 0;
    dev_cfg.spics_io_num   = -1;  // manual CS
    dev_cfg.queue_size     = 1;
    dev_cfg.flags          = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY;

    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &dev_cfg, &spi_dev));
    spi_device_acquire_bus(spi_dev, portMAX_DELAY);
}

// --- Core SPI write with variable cmd/addr/data and optional QIO ---

static void write_cmd_addr_data(uint8_t cmd_bits, uint32_t cmd, uint8_t addr_bits,
                                uint32_t address, const uint8_t *data, size_t length,
                                uint8_t bus_width = 1) {
    spi_transaction_ext_t desc = {};
    desc.base.flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_DUMMY;
    if (bus_width == 4) desc.base.flags |= SPI_TRANS_MODE_QIO;
    desc.command_bits = cmd_bits;
    desc.address_bits = addr_bits;
    desc.dummy_bits = 0;
    desc.base.cmd = cmd;
    desc.base.addr = address;

    const size_t max_chunk = 32768;
    do {
        size_t chunk = (length > max_chunk) ? max_chunk : length;
        if (data && chunk) {
            desc.base.length = chunk * 8;
            desc.base.tx_buffer = data;
            length -= chunk;
            data += chunk;
        } else {
            length = 0;
            desc.base.length = 0;
        }
        spi_device_polling_start(spi_dev, (spi_transaction_t *)&desc, portMAX_DELAY);
        spi_device_polling_end(spi_dev, portMAX_DELAY);
        desc.command_bits = 0;
        desc.address_bits = 0;
    } while (length != 0);
}

// --- Register write (single SPI line): cmd=0x02, addr=reg<<8 ---

static void lcd_cmd(uint8_t reg, const uint8_t *data, uint16_t len)
{
    cs_low();
    write_cmd_addr_data(8, 0x02, 24, (uint32_t)reg << 8, data, len, 1);
    cs_high();
}

// --- Pixel draw: set window then push pixels via QSPI ---

static void lcd_draw_bitmap(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                            const uint8_t *data, size_t len) {
    uint8_t ca[] = {(uint8_t)(x0>>8), (uint8_t)x0, (uint8_t)(x1>>8), (uint8_t)x1};
    lcd_cmd(0x2A, ca, 4);
    uint8_t ra[] = {(uint8_t)(y0>>8), (uint8_t)y0, (uint8_t)(y1>>8), (uint8_t)y1};
    lcd_cmd(0x2B, ra, 4);
    cs_low();
    write_cmd_addr_data(8, 0x32, 24, 0x2C << 8, data, len, 4);
    cs_high();
}

// --- Hardware reset (active-low on GPIO 21, 10ms pulse) ---

static void lcd_hw_reset()
{
    pinMode(PIN_LCD_RST, OUTPUT);
    digitalWrite(PIN_LCD_RST, HIGH);
    delay(10);
    digitalWrite(PIN_LCD_RST, LOW);
    delay(10);
    digitalWrite(PIN_LCD_RST, HIGH);
    delay(120);
}

// --- Send init register table ---

static void lcd_send_init_cmds()
{
    // COLMOD must be set BEFORE the init table — tells panel we're sending RGB565
    uint8_t colmod = 0x55;  // 16-bit color
    lcd_cmd(0x3A, &colmod, 1);

    const int count = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
    for (int i = 0; i < count; i++) {
        lcd_cmd(lcd_init_cmds[i].reg, lcd_init_cmds[i].data, lcd_init_cmds[i].len);
        if (lcd_init_cmds[i].delay_ms > 0) {
            delay(lcd_init_cmds[i].delay_ms);
        }
    }

    // Extra delay after DISPON for stability
    delay(20);
}

// --- Backlight (PWM via ledcAttach/ledcWrite on GPIO 47) ---

static void lcd_backlight_on()
{
    ledcSetup(0, 1000, 8);           // channel 0, 1 kHz, 8-bit
    ledcAttachPin(PIN_LCD_BL, 0);    // attach GPIO to channel 0
    ledcWrite(0, 200);               // ~78% brightness
}

// --- Full LCD hardware init ---

static void lcd_init()
{
    // CS pin: manual GPIO output
    pinMode(PIN_LCD_CS, OUTPUT);
    cs_high();

    spi_init();
    lcd_hw_reset();
    lcd_send_init_cmds();
    // Backlight stays OFF — turned on after first frame in display_splash()

    Serial.println("[display] LCD hardware initialized");
}

// ============================================================================
// Section B: LVGL 9.2 Setup
// ============================================================================

static lv_display_t *lvgl_disp = NULL;
static uint8_t *lvgl_buf = NULL;

// --- LVGL flush callback ---

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint16_t x0 = area->x1, y0 = area->y1;
    uint16_t x1 = area->x2, y1 = area->y2;
    size_t len = (x1 - x0 + 1) * (y1 - y0 + 1) * 2;
    lcd_draw_bitmap(x0, y0, x1, y1, px_map, len);
    lv_display_flush_ready(disp);
}

// --- LVGL init ---

static void lvgl_init()
{
    lv_init();

    // Use Arduino millis() as LVGL tick source (wrapper for type match)
    lv_tick_set_cb([]() -> uint32_t { return (uint32_t)millis(); });

    // Allocate DMA-capable draw buffer: 360 x 36 lines x 2 bytes/pixel
    const uint32_t buf_size = LCD_H_RES * 36 * 2;
    lvgl_buf = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    assert(lvgl_buf);

    // Create LVGL display (LVGL 9 API)
    lvgl_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(lvgl_disp, flush_cb);
    lv_display_set_buffers(lvgl_disp, lvgl_buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Set default screen to black immediately — prevents white flash before splash
    lv_obj_t *default_scr = lv_display_get_screen_active(lvgl_disp);
    lv_obj_set_style_bg_color(default_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(default_scr, LV_OPA_COVER, 0);
    lv_refr_now(lvgl_disp);

    Serial.println("[display] LVGL initialized");
}

lv_display_t* display_get_disp() {
    return lvgl_disp;
}

// ============================================================================
// Section C: Screen Rendering
// ============================================================================

// --- Static LVGL object pointers for UI elements ---
static lv_obj_t *lbl_splash    = NULL;
static lv_obj_t *scr_splash    = NULL;
static lv_obj_t *arc_splash    = NULL;

// Dirty flag pattern
static bool dirty = false;

// --- Public API ---

void display_init()
{
    lcd_init();
    lvgl_init();
}

// --- Splash: matrix rain with title fade-in ---

#define MATRIX_COLS 8
#define MATRIX_ROWS 12
#define SPLASH_TOTAL_MS 5000    // Total splash duration
#define TITLE_FADE_START 1500   // Title starts fading in at 1.5s
#define TITLE_FADE_END 3000     // Title fully visible at 3s

void display_splash()
{
    scr_splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_splash, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr_splash, LV_OPA_COVER, 0);

    // --- Matrix rain grid (background layer) ---
    static const char hex_chars[] = "0123456789ABCDEF<>{}[]/:;!@#";
    lv_obj_t *cells[MATRIX_COLS][MATRIX_ROWS];
    int16_t drop_y[MATRIX_COLS];
    uint8_t drop_speed[MATRIX_COLS];
    int col_spacing = 40;
    int row_spacing = 26;
    int x_off = (360 - MATRIX_COLS * col_spacing) / 2;

    for (int c = 0; c < MATRIX_COLS; c++) {
        drop_y[c] = -(random(MATRIX_ROWS));
        drop_speed[c] = 2 + random(3);
        for (int r = 0; r < MATRIX_ROWS; r++) {
            cells[c][r] = lv_label_create(scr_splash);
            char ch[2] = {hex_chars[random(sizeof(hex_chars) - 1)], 0};
            lv_label_set_text(cells[c][r], ch);
            lv_obj_set_pos(cells[c][r], x_off + c * col_spacing, r * row_spacing + 15);
            lv_obj_set_style_text_color(cells[c][r], lv_color_black(), 0);
        }
    }

    // --- Arc ring (mid layer) ---
    arc_splash = lv_arc_create(scr_splash);
    lv_obj_set_size(arc_splash, 320, 320);
    lv_obj_center(arc_splash);
    lv_arc_set_rotation(arc_splash, 270);
    lv_arc_set_bg_angles(arc_splash, 0, 360);
    lv_arc_set_range(arc_splash, 0, 1000);
    lv_arc_set_value(arc_splash, 10);
    lv_obj_set_style_arc_color(arc_splash, COL_DARK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_splash, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_splash, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_splash, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_splash, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc_splash, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(arc_splash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(arc_splash, 0, 0);  // Start invisible

    // --- Title labels (top layer, start invisible) ---
    // Glow layer — slightly offset, dimmer, creates glow effect
    lv_obj_t *lbl_glow = lv_label_create(scr_splash);
    lv_label_set_text(lbl_glow, "NetKnob");
    lv_obj_set_style_text_font(lbl_glow, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_glow, lv_color_make(0x00, 0x60, 0x80), 0);
    lv_obj_align(lbl_glow, LV_ALIGN_CENTER, 1, -9);
    lv_obj_set_style_opa(lbl_glow, 0, 0);

    lbl_splash = lv_label_create(scr_splash);
    lv_label_set_text(lbl_splash, "NetKnob");
    lv_obj_set_style_text_font(lbl_splash, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_splash, COL_CYAN, 0);
    lv_obj_align(lbl_splash, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_opa(lbl_splash, 0, 0);

    lv_obj_t *lbl_sub = lv_label_create(scr_splash);
    lv_label_set_text(lbl_sub, "wifi scanner");
    lv_obj_set_style_text_font(lbl_sub, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_sub, COL_GRAY, 0);
    lv_obj_align(lbl_sub, LV_ALIGN_CENTER, 0, 22);
    lv_obj_set_style_opa(lbl_sub, 0, 0);

    lv_obj_t *lbl_ver = lv_label_create(scr_splash);
    lv_label_set_text(lbl_ver, "v1.0");
    lv_obj_set_style_text_color(lbl_ver, COL_CYAN_DIM, 0);
    lv_obj_align(lbl_ver, LV_ALIGN_CENTER, 0, 46);
    lv_obj_set_style_opa(lbl_ver, 0, 0);

    lv_screen_load(scr_splash);
    lv_refr_now(lvgl_disp);
    lcd_backlight_on();

    // --- Animation loop ---
    lv_color_t trail_bright[] = {
        lv_color_make(0x80, 0xFF, 0x80),   // Head: soft white-green
        lv_color_make(0x00, 0x90, 0x40),   // Bright green
        lv_color_make(0x00, 0x60, 0x30),   // Mid green
        lv_color_make(0x00, 0x38, 0x1C),   // Fading
        lv_color_make(0x00, 0x20, 0x10),   // Dim
        lv_color_make(0x00, 0x10, 0x08),   // Very dim
    };
    lv_color_t trail_dim[] = {
        lv_color_make(0x00, 0x20, 0x10),
        lv_color_make(0x00, 0x14, 0x0A),
        lv_color_make(0x00, 0x0C, 0x06),
        lv_color_make(0x00, 0x08, 0x04),
        lv_color_make(0x00, 0x04, 0x02),
        lv_color_make(0x00, 0x02, 0x01),
    };
    int trail_len = 6;

    uint32_t start = millis();
    uint8_t frame = 0;

    while (millis() - start < SPLASH_TOTAL_MS) {
        uint32_t elapsed = millis() - start;
        frame++;

        // --- Rain fade factor: 0-255, 255=full brightness, decreases after title starts ---
        uint8_t rain_bright = 255;
        if (elapsed > TITLE_FADE_START) {
            uint32_t fade_elapsed = elapsed - TITLE_FADE_START;
            uint32_t fade_duration = TITLE_FADE_END - TITLE_FADE_START;
            if (fade_elapsed >= fade_duration) rain_bright = 40;
            else rain_bright = 255 - (uint8_t)((uint32_t)(255 - 40) * fade_elapsed / fade_duration);
        }

        // --- Update rain drops ---
        for (int c = 0; c < MATRIX_COLS; c++) {
            if (frame % drop_speed[c] != 0) continue;
            drop_y[c]++;
            if (drop_y[c] > MATRIX_ROWS + trail_len) {
                drop_y[c] = -(random(5));
                drop_speed[c] = 2 + random(3);
            }
            for (int r = 0; r < MATRIX_ROWS; r++) {
                int dist = drop_y[c] - r;
                if (dist >= 0 && dist < trail_len) {
                    // Blend between bright and dim trails based on rain_bright
                    lv_color_t col;
                    if (rain_bright > 128) {
                        col = trail_bright[dist];
                    } else {
                        col = trail_dim[dist];
                    }
                    lv_obj_set_style_text_color(cells[c][r], col, 0);
                    if (dist == 0) {
                        char ch[2] = {hex_chars[random(sizeof(hex_chars) - 1)], 0};
                        lv_label_set_text(cells[c][r], ch);
                    }
                } else {
                    lv_obj_set_style_text_color(cells[c][r], lv_color_black(), 0);
                }
            }
        }

        // --- Title fade-in ---
        if (elapsed >= TITLE_FADE_START) {
            uint32_t fade_elapsed = elapsed - TITLE_FADE_START;
            uint32_t fade_duration = TITLE_FADE_END - TITLE_FADE_START;
            uint8_t title_opa = (fade_elapsed >= fade_duration) ? 255
                : (uint8_t)((uint32_t)255 * fade_elapsed / fade_duration);

            lv_obj_set_style_opa(lbl_splash, title_opa, 0);
            lv_obj_set_style_opa(lbl_glow, title_opa / 2, 0);  // Glow at half opacity
            lv_obj_set_style_opa(arc_splash, title_opa, 0);

            // Subtitle fades in slightly later
            uint8_t sub_opa = 0;
            if (fade_elapsed > fade_duration / 3) {
                uint32_t sub_elapsed = fade_elapsed - fade_duration / 3;
                sub_opa = (sub_elapsed >= fade_duration * 2 / 3) ? 255
                    : (uint8_t)((uint32_t)255 * sub_elapsed / (fade_duration * 2 / 3));
            }
            lv_obj_set_style_opa(lbl_sub, sub_opa, 0);

            // Version fades in last
            uint8_t ver_opa = 0;
            if (fade_elapsed > fade_duration * 2 / 3) {
                uint32_t ver_elapsed = fade_elapsed - fade_duration * 2 / 3;
                ver_opa = (ver_elapsed >= fade_duration / 3) ? 255
                    : (uint8_t)((uint32_t)255 * ver_elapsed / (fade_duration / 3));
            }
            lv_obj_set_style_opa(lbl_ver, ver_opa, 0);

            // Arc fills during fade
            uint16_t arc_progress = (uint16_t)((uint32_t)fade_elapsed * 980 / fade_duration) + 10;
            if (arc_progress > 998) arc_progress = 998;
            lv_arc_set_value(arc_splash, arc_progress);
        }

        lv_refr_now(lvgl_disp);
        lv_timer_handler();
        delay(30);
    }

    Serial.println("[display] splash complete");
}

// display_animate_splash is no longer needed — animation is built into display_splash
void display_animate_splash(uint16_t duration_ms)
{
    // No-op — splash animation is now self-contained
}

void display_clear()
{
    // Create a fresh blank screen and switch to it
    lv_obj_t *new_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(new_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(new_scr, LV_OPA_COVER, 0);

    lv_obj_t *old_scr = lv_screen_active();
    lv_screen_load(new_scr);
    if (old_scr && old_scr != new_scr) {
        lv_obj_delete(old_scr);
    }

    // Null ALL static LVGL pointers — splash
    lbl_splash = NULL;
    scr_splash = NULL;
    arc_splash = NULL;

    dirty = true;
}

void display_mark_dirty()
{
    dirty = true;
}

bool display_is_dirty()
{
    return dirty;
}

void display_flush()
{
    if (dirty) {
        lv_refr_now(lvgl_disp);
        dirty = false;
    }
}

// --- Helper functions ---

lv_color_t rssi_color(int8_t rssi) {
    if (rssi > -50) return COL_GREEN;
    if (rssi > -70) return COL_ORANGE;
    return COL_RED;
}

// Arc driven by strongest RSSI: -30 dBm = 980, -90 dBm = 20 (range 0-1000)
uint16_t rssi_to_arc(int8_t rssi) {
    if (rssi >= -30) return 980;
    if (rssi <= -90) return 20;
    uint16_t val = (uint16_t)(rssi + 90) * 960 / 60 + 20;
    if (val < 20) val = 20;
    if (val > 980) val = 980;
    return val;
}

// Arc color based on strongest RSSI
lv_color_t rssi_arc_color(int8_t rssi) {
    if (rssi > -50) return COL_GREEN;
    if (rssi > -70) return COL_CYAN;
    return COL_ORANGE;
}

const char* rssi_bars(int8_t rssi) {
    if (rssi > -40) return "||||";
    if (rssi > -55) return "||| ";
    if (rssi > -65) return "||  ";
    if (rssi > -75) return "|   ";
    return "    ";
}

