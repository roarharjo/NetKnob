#pragma once

#include <lvgl.h>

// --- Hardware + LVGL lifecycle ---
void display_init();
void display_splash();
void display_animate_splash(uint16_t duration_ms);
void display_clear();
void display_mark_dirty();
bool display_is_dirty();
void display_flush();

// --- Shared for screen modules ---
lv_display_t* display_get_disp();

// Color palette — neon/cyber aesthetic
#define COL_BG          lv_color_black()
#define COL_CYAN        lv_color_make(0x00, 0xE5, 0xFF)
#define COL_CYAN_DIM    lv_color_make(0x00, 0x60, 0x80)
#define COL_GREEN       lv_color_make(0x00, 0xFF, 0x80)
#define COL_ORANGE      lv_color_make(0xFF, 0xA0, 0x00)
#define COL_RED         lv_color_make(0xFF, 0x30, 0x30)
#define COL_WHITE       lv_color_white()
#define COL_GRAY        lv_color_make(0x60, 0x70, 0x80)
#define COL_DARK        lv_color_make(0x18, 0x1C, 0x24)
#define COL_SELECTED    lv_color_make(0x00, 0x30, 0x40)

// RSSI helpers (shared by WiFi + BLE screens)
lv_color_t rssi_color(int8_t rssi);
const char* rssi_bars(int8_t rssi);
uint16_t rssi_to_arc(int8_t rssi);
lv_color_t rssi_arc_color(int8_t rssi);
