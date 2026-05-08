#include "scr_debug.h"
#include "display.h"
#include "heap_monitor.h"
#include "ble_scanner.h"
#include "wifi_scanner.h"
#include <Arduino.h>
#include <lvgl.h>
#include <esp_heap_caps.h>

static lv_obj_t* scr_root = NULL;
static lv_obj_t* lbl_lines[9] = {};
static uint32_t last_update_ms = 0;

static void refresh_values() {
    uint32_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t min_ever = esp_get_minimum_free_heap_size();
    uint32_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    uint32_t psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t uptime   = millis() / 1000;

    WifiScannerState* ws = scanner_get_state();
    BleScannerState* bs = ble_scanner_get_state();

    lv_label_set_text_fmt(lbl_lines[0], "SRAM Free:   %lu KB", free_int / 1024);
    lv_label_set_text_fmt(lbl_lines[1], "SRAM Min:    %lu KB", min_ever / 1024);
    lv_label_set_text_fmt(lbl_lines[2], "Largest Blk: %lu KB", largest / 1024);
    lv_label_set_text_fmt(lbl_lines[3], "PSRAM Free:  %lu KB", psram / 1024);
    lv_label_set_text_fmt(lbl_lines[4], "WiFi: CH %d / %s",
        ws->current_channel, ws->scanning ? "ON" : "OFF");
    lv_label_set_text_fmt(lbl_lines[5], "BLE:  %s / %d dev",
        bs->scanning ? "SCAN" : "OFF", bs->device_count);
    lv_label_set_text_fmt(lbl_lines[6], "Uptime: %02lu:%02lu:%02lu",
        uptime / 3600, (uptime / 60) % 60, uptime % 60);

    // Color code SRAM free
    lv_color_t col = (free_int < HEAP_CRITICAL_THRESHOLD) ? COL_RED :
                     (free_int < HEAP_WARN_THRESHOLD) ? COL_ORANGE : COL_GREEN;
    lv_obj_set_style_text_color(lbl_lines[0], col, 0);
}

static void create() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);

    lv_obj_t* title = lv_label_create(scr_root);
    lv_label_set_text(title, "System Debug");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, COL_CYAN_DIM, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

    for (int i = 0; i < 9; i++) {
        lbl_lines[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_font(lbl_lines[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl_lines[i], COL_GRAY, 0);
        lv_obj_set_pos(lbl_lines[i], 50, 55 + i * 30);
    }
}

static void show() {
    refresh_values();
    lv_screen_load(scr_root);
    last_update_ms = millis();
}

static void hide() {}

static void destroy() {
    if (scr_root) {
        lv_obj_del(scr_root);
        scr_root = NULL;
        for (int i = 0; i < 9; i++) lbl_lines[i] = NULL;
        heap_monitor_log_baseline("debug_destroy");
    }
}

static void update() {
    uint32_t now = millis();
    if (now - last_update_ms >= 1000) {
        last_update_ms = now;
        refresh_values();
        lv_refr_now(display_get_disp());
    }
}

const ScreenDef scr_debug_def = {
    .name = "Debug",
    .group = GROUP_SYSTEM,
    .id = SCREEN_DEBUG,
    .create = create,
    .show = show,
    .hide = hide,
    .destroy = destroy,
    .update = update,
    .enc_mode = ENC_MENU
};
