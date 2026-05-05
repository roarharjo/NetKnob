#include "scr_settings.h"
#include "display.h"
#include "settings.h"
#include "safe_lock.h"
#include "haptic.h"
#include <lvgl.h>
#include <stdio.h>

#define SETTING_COUNT 9
#define VISIBLE_ROWS 7

enum SettingType { ST_TOGGLE, ST_VALUE, ST_CHOICE, ST_ACTION };

struct SettingDef {
    const char* name;
    SettingType type;
};

static const SettingDef setting_defs[SETTING_COUNT] = {
    { "Lock enabled",      ST_TOGGLE },
    { "Change code",       ST_ACTION },
    { "Lock timeout",      ST_VALUE  },
    { "WiFi region",       ST_CHOICE },
    { "Brightness",        ST_VALUE  },
    { "Haptic feedback",   ST_TOGGLE },
    { "Haptic strength",   ST_CHOICE },
    { "Auto-scan boot",    ST_TOGGLE },
    { "Splash duration",   ST_VALUE  },
};

static lv_obj_t* scr_root = NULL;
static lv_obj_t* lbl_title = NULL;
static lv_obj_t* lbl_name[VISIBLE_ROWS] = {};
static lv_obj_t* lbl_value[VISIBLE_ROWS] = {};

static uint8_t cursor = 0;
static uint8_t scroll_offset = 0;
static bool editing = false;

static void get_value_str(uint8_t idx, char* buf, size_t len) {
    const Settings* s = settings_get();
    switch (idx) {
        case 0: snprintf(buf, len, "%s", s->lock_enabled ? "ON" : "OFF"); break;
        case 1: snprintf(buf, len, "..."); break;
        case 2: snprintf(buf, len, "%d min", s->lock_timeout_min); break;
        case 3: {
            const char* regions[] = { "ETSI", "FCC", "Japan" };
            snprintf(buf, len, "%s", regions[s->wifi_region]);
            break;
        }
        case 4: snprintf(buf, len, "%d%%", s->display_brightness); break;
        case 5: snprintf(buf, len, "%s", s->haptic_enabled ? "ON" : "OFF"); break;
        case 6: {
            const char* str[] = { "Weak", "Med", "Strong" };
            snprintf(buf, len, "%s", str[s->haptic_strength]);
            break;
        }
        case 7: snprintf(buf, len, "%s", s->auto_scan_on_boot ? "ON" : "OFF"); break;
        case 8: snprintf(buf, len, "%d s", s->splash_duration_sec); break;
    }
}

static void update_display() {
    for (int i = 0; i < VISIBLE_ROWS; i++) {
        uint8_t idx = scroll_offset + i;
        if (idx >= SETTING_COUNT) {
            lv_obj_add_flag(lbl_name[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_value[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(lbl_name[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_value[i], LV_OBJ_FLAG_HIDDEN);

        lv_label_set_text(lbl_name[i], setting_defs[idx].name);
        char vbuf[16];
        get_value_str(idx, vbuf, sizeof(vbuf));
        lv_label_set_text(lbl_value[i], vbuf);

        bool selected = (idx == cursor);
        lv_obj_set_style_text_color(lbl_name[i], selected ? COL_CYAN : COL_GRAY, 0);
        lv_obj_set_style_text_color(lbl_value[i],
            (selected && editing) ? COL_GREEN : (selected ? COL_CYAN : COL_GRAY), 0);
    }
    lv_refr_now(display_get_disp());
}

// Helper: clamp uint8_t addition
static uint8_t clamp_add(uint8_t val, int8_t delta, uint8_t lo, uint8_t hi) {
    int16_t result = (int16_t)val + delta;
    if (result < lo) return lo;
    if (result > hi) return hi;
    return (uint8_t)result;
}

static void create() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);

    lbl_title = lv_label_create(scr_root);
    lv_label_set_text(lbl_title, "Settings");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_title, COL_CYAN_DIM, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 30);

    for (int i = 0; i < VISIBLE_ROWS; i++) {
        int y = 65 + i * 38;
        lbl_name[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_font(lbl_name[i], &lv_font_montserrat_14, 0);
        lv_obj_set_pos(lbl_name[i], 40, y);

        lbl_value[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_font(lbl_value[i], &lv_font_montserrat_14, 0);
        lv_obj_set_pos(lbl_value[i], 240, y);
    }
}

static void show() {
    editing = false;
    update_display();
    lv_screen_load(scr_root);
}

static void hide() {}
static void update() {}

void scr_settings_on_encoder(int8_t delta) {
    const Settings* s = settings_get();
    if (!editing) {
        int8_t new_cursor = (int8_t)cursor + delta;
        if (new_cursor < 0) new_cursor = 0;
        if (new_cursor >= SETTING_COUNT) new_cursor = SETTING_COUNT - 1;
        cursor = new_cursor;
        if (cursor < scroll_offset) scroll_offset = cursor;
        if (cursor >= scroll_offset + VISIBLE_ROWS) scroll_offset = cursor - VISIBLE_ROWS + 1;
    } else {
        switch (cursor) {
            case 2: settings_set_lock_timeout(clamp_add(s->lock_timeout_min, delta, 0, 60)); break;
            case 3: settings_set_wifi_region((s->wifi_region + delta + 3) % 3); break;
            case 4: settings_set_brightness(clamp_add(s->display_brightness, delta * 5, 10, 100)); break;
            case 6: settings_set_haptic_strength((s->haptic_strength + delta + 3) % 3); break;
            case 8: settings_set_splash_duration(clamp_add(s->splash_duration_sec, delta, 0, 5)); break;
        }
    }
    haptic_click();
    update_display();
}

void scr_settings_on_tap() {
    Settings* s = settings_get_mut();
    if (editing) {
        editing = false;
        haptic_double_click();
    } else {
        switch (setting_defs[cursor].type) {
            case ST_TOGGLE:
                switch (cursor) {
                    case 0:
                        if (!s->lock_enabled && !settings_has_lock_code()) {
                            // Enabling lock but no code set — set code first
                            settings_set_lock_enabled(true);
                            safe_lock_start_set_code();
                            navigation_goto(SCREEN_SAFE_LOCK);
                        } else {
                            settings_set_lock_enabled(!s->lock_enabled);
                        }
                        break;
                    case 5: settings_set_haptic_enabled(!s->haptic_enabled); break;
                    case 7: settings_set_auto_scan(!s->auto_scan_on_boot); break;
                }
                haptic_click();
                break;
            case ST_VALUE:
            case ST_CHOICE:
                editing = true;
                haptic_click();
                break;
            case ST_ACTION:
                // Change code: enter set-code flow on lock screen
                safe_lock_start_set_code();
                navigation_goto(SCREEN_SAFE_LOCK);
                haptic_double_click();
                break;
        }
    }
    update_display();
}

const ScreenDef scr_settings_def = {
    .name = "Settings",
    .group = GROUP_SYSTEM,
    .id = SCREEN_SETTINGS,
    .create = create,
    .show = show,
    .hide = hide,
    .destroy = NULL,
    .update = update,
    .enc_mode = ENC_SETTINGS
};
