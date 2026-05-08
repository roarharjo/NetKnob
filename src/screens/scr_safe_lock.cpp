#include "scr_safe_lock.h"
#include "display.h"
#include "safe_lock.h"
#include "haptic.h"
#include "settings.h"
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>

static lv_obj_t* scr_root = NULL;
static lv_obj_t* lbl_title = NULL;       // "LOCKED" or "SET CODE" or "CONFIRM CODE"
static lv_obj_t* lbl_position = NULL;    // Current dial number (large)
static lv_obj_t* lbl_direction = NULL;   // "CW >>>" or "<<< CCW"
static lv_obj_t* lbl_progress = NULL;    // "1 / 3", "2 / 3", etc.
static lv_obj_t* lbl_status = NULL;      // Status messages

// Debug bypass
static uint8_t tap_count = 0;
static uint32_t first_tap_ms = 0;
#define BYPASS_TAPS 5
#define BYPASS_WINDOW_MS 2000

static void update_display() {
    const SafeLockState* s = safe_lock_get_state();

    // Title based on mode
    switch (s->mode) {
        case LOCKMODE_VERIFY:      lv_label_set_text(lbl_title, "LOCKED"); break;
        case LOCKMODE_SET_NEW:     lv_label_set_text(lbl_title, "SET CODE"); break;
        case LOCKMODE_SET_CONFIRM: lv_label_set_text(lbl_title, "CONFIRM CODE"); break;
    }
    lv_obj_set_style_text_color(lbl_title,
        s->mode == LOCKMODE_VERIFY ? COL_RED : COL_CYAN, 0);

    // Position number
    lv_label_set_text_fmt(lbl_position, "%d", s->current_position);

    // Direction arrow and progress
    switch (s->phase) {
        case LOCK_DIGIT_1_CW:
            lv_label_set_text(lbl_direction, "CW >>>");
            lv_label_set_text(lbl_progress, "1 / 3");
            lv_obj_set_style_text_color(lbl_direction, COL_CYAN, 0);
            break;
        case LOCK_DIGIT_2_CCW:
            lv_label_set_text(lbl_direction, "<<< CCW");
            lv_label_set_text(lbl_progress, "2 / 3");
            lv_obj_set_style_text_color(lbl_direction, COL_ORANGE, 0);
            break;
        case LOCK_DIGIT_3_CW:
            lv_label_set_text(lbl_direction, "CW >>>");
            lv_label_set_text(lbl_progress, "3 / 3");
            lv_obj_set_style_text_color(lbl_direction, COL_CYAN, 0);
            break;
        case LOCK_OPEN_CCW:
            lv_label_set_text(lbl_direction, "<<< CCW to confirm");
            lv_label_set_text(lbl_progress, "");
            lv_obj_set_style_text_color(lbl_direction, COL_GREEN, 0);
            break;
        case LOCK_SUCCESS:
            lv_label_set_text(lbl_direction, "");
            lv_label_set_text(lbl_progress, "");
            if (s->mode == LOCKMODE_VERIFY) {
                lv_label_set_text(lbl_status, "UNLOCKED");
            } else {
                lv_label_set_text(lbl_status, "CODE SAVED");
            }
            lv_obj_set_style_text_color(lbl_status, COL_GREEN, 0);
            break;
        case LOCK_FAILED:
            lv_label_set_text(lbl_direction, "");
            lv_label_set_text(lbl_progress, "");
            if (s->mode == LOCKMODE_SET_CONFIRM) {
                lv_label_set_text(lbl_status, "MISMATCH");
            } else {
                lv_label_set_text(lbl_status, "WRONG CODE");
            }
            lv_obj_set_style_text_color(lbl_status, COL_RED, 0);
            break;
    }

    // Normal status
    if (s->phase < LOCK_SUCCESS) {
        if (safe_lock_is_locked_out()) {
            lv_label_set_text(lbl_status, "Locked out...");
            lv_obj_set_style_text_color(lbl_status, COL_RED, 0);
        } else if (s->mode == LOCKMODE_SET_NEW) {
            lv_label_set_text(lbl_status, "Enter new combination");
            lv_obj_set_style_text_color(lbl_status, COL_GRAY, 0);
        } else if (s->mode == LOCKMODE_SET_CONFIRM) {
            lv_label_set_text(lbl_status, "Enter again to confirm");
            lv_obj_set_style_text_color(lbl_status, COL_GRAY, 0);
        } else {
            lv_label_set_text(lbl_status, "Enter combination");
            lv_obj_set_style_text_color(lbl_status, COL_GRAY, 0);
        }
    }

    lv_refr_now(display_get_disp());
}

static void create() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);

    // Title — 20pt at top
    lbl_title = lv_label_create(scr_root);
    lv_label_set_text(lbl_title, "LOCKED");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_title, COL_RED, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 55);

    // Progress (1/3, 2/3, 3/3) — above the number
    lbl_progress = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_progress, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_progress, COL_CYAN_DIM, 0);
    lv_obj_align(lbl_progress, LV_ALIGN_CENTER, 0, -55);

    // Current position — BIG centered number (28pt is max available)
    lbl_position = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_position, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_position, COL_CYAN, 0);
    lv_obj_align(lbl_position, LV_ALIGN_CENTER, 0, -15);
    lv_label_set_text(lbl_position, "0");

    // Direction arrow — 20pt below the number
    lbl_direction = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_direction, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_direction, COL_CYAN, 0);
    lv_obj_align(lbl_direction, LV_ALIGN_CENTER, 0, 30);

    // Status message — 16pt near bottom
    lbl_status = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_status, COL_GRAY, 0);
    lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_label_set_text(lbl_status, "Enter combination");
}

static void show() {
    // Don't reset if already in set-code mode (called from settings)
    if (safe_lock_get_mode() != LOCKMODE_SET_NEW &&
        safe_lock_get_mode() != LOCKMODE_SET_CONFIRM) {
        safe_lock_reset();
    }
    tap_count = 0;
    lv_screen_load(scr_root);
    update_display();
}

static void hide() {}

static void destroy() {
    if (scr_root) {
        lv_obj_del(scr_root);
        scr_root = NULL;
        lbl_title = NULL;
        lbl_position = NULL;
        lbl_direction = NULL;
        lbl_progress = NULL;
        lbl_status = NULL;
    }
}

static void update() {
    safe_lock_update();  // Handle stop detection

    LockPhase phase = safe_lock_get_phase();
    LockMode mode = safe_lock_get_mode();

    if (phase == LOCK_SUCCESS) {
        haptic_double_click();
        update_display();
        delay(500);
        if (mode == LOCKMODE_SET_NEW || mode == LOCKMODE_SET_CONFIRM) {
            // Code was set — go back to settings
            navigation_goto(SCREEN_SETTINGS);
        } else {
            // Unlocked — go to main menu
            navigation_goto(SCREEN_MAIN_MENU);
        }
        return;
    }

    if (phase == LOCK_FAILED) {
        haptic_play(14);  // Long buzz
        update_display();
        delay(1000);
        if (mode == LOCKMODE_SET_CONFIRM) {
            // Mismatch — restart set-code flow
            safe_lock_start_set_code();
        } else {
            safe_lock_reset();
        }
        update_display();
        return;
    }

    update_display();
}

void scr_safe_lock_on_encoder(int8_t delta) {
    safe_lock_on_encoder(delta);
    haptic_click();
    update_display();
}

void scr_safe_lock_on_tap() {
    // Debug bypass: 5 taps within 2 seconds
    uint32_t now = millis();
    if (now - first_tap_ms > BYPASS_WINDOW_MS) {
        tap_count = 0;
    }
    if (tap_count == 0) first_tap_ms = now;
    tap_count++;

    if (tap_count >= BYPASS_TAPS) {
        Serial.println("[LOCK] DEBUG BYPASS");
        haptic_double_click();
        navigation_goto(SCREEN_MAIN_MENU);
        tap_count = 0;
    }
}

const ScreenDef scr_safe_lock_def = {
    .name = "Safe Lock",
    .group = GROUP_SYSTEM,
    .id = SCREEN_SAFE_LOCK,
    .create = create,
    .show = show,
    .hide = hide,
    .destroy = destroy,
    .update = update,
    .enc_mode = ENC_SAFE_LOCK
};
