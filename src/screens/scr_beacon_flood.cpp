/* scr_beacon_flood.cpp — Beacon Flood attack screen
 *
 * Three visual states matching AttackPhase:
 *   CONFIG   — parameter selection and preview
 *   RUNNING  — live stats with magenta glow border
 *   COMPLETE — summary and dismiss/retry options
 */

#include "scr_beacon_flood.h"
#include "display.h"
#include "haptic.h"
#include "attack_common.h"
#include "wifi_attack.h"
#include "wifi_scanner.h"
#include <Arduino.h>
#include <lvgl.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// LVGL objects
// ============================================================================

static lv_obj_t* scr_root = NULL;

// --- Config view ---
static lv_obj_t* lbl_title      = NULL;
static lv_obj_t* lbl_channel    = NULL;
static lv_obj_t* lbl_params[4]  = {};   // ssid count, source, rate, duration
static lv_obj_t* lbl_total_rate = NULL;
static lv_obj_t* lbl_hint       = NULL;

// --- Running view ---
static lv_obj_t* lbl_run_title  = NULL;
static lv_obj_t* lbl_run_ssids  = NULL;
static lv_obj_t* lbl_run_rate   = NULL;
static lv_obj_t* lbl_run_sent   = NULL;
static lv_obj_t* lbl_run_time   = NULL;
static lv_obj_t* bar_progress   = NULL;
static lv_obj_t* lbl_run_hint   = NULL;
static lv_obj_t* border_glow    = NULL;

// --- Complete view ---
static lv_obj_t* lbl_comp_title = NULL;
static lv_obj_t* lbl_comp_dur   = NULL;
static lv_obj_t* lbl_comp_total = NULL;
static lv_obj_t* lbl_comp_rate  = NULL;
static lv_obj_t* lbl_comp_hint  = NULL;

// --- State tracking ---
static uint8_t  param_cursor    = 0;
static bool     config_built    = false;
static bool     running_built   = false;
static bool     complete_built  = false;
static uint32_t last_ui_update  = 0;
static AttackPhase last_shown_phase = ATTACK_IDLE;

static const char* source_names[] = {"Random", "Wordlist", "Clone"};

// ============================================================================
// hide_all_views — null-check each object before hiding
// ============================================================================

static void hide_all_views() {
    // Config view
    if (lbl_title)      lv_obj_add_flag(lbl_title,      LV_OBJ_FLAG_HIDDEN);
    if (lbl_channel)    lv_obj_add_flag(lbl_channel,    LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 4; i++) {
        if (lbl_params[i]) lv_obj_add_flag(lbl_params[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (lbl_total_rate) lv_obj_add_flag(lbl_total_rate, LV_OBJ_FLAG_HIDDEN);
    if (lbl_hint)       lv_obj_add_flag(lbl_hint,       LV_OBJ_FLAG_HIDDEN);

    // Running view
    if (border_glow)    lv_obj_add_flag(border_glow,    LV_OBJ_FLAG_HIDDEN);
    if (lbl_run_title)  lv_obj_add_flag(lbl_run_title,  LV_OBJ_FLAG_HIDDEN);
    if (lbl_run_ssids)  lv_obj_add_flag(lbl_run_ssids,  LV_OBJ_FLAG_HIDDEN);
    if (lbl_run_rate)   lv_obj_add_flag(lbl_run_rate,   LV_OBJ_FLAG_HIDDEN);
    if (lbl_run_sent)   lv_obj_add_flag(lbl_run_sent,   LV_OBJ_FLAG_HIDDEN);
    if (lbl_run_time)   lv_obj_add_flag(lbl_run_time,   LV_OBJ_FLAG_HIDDEN);
    if (bar_progress)   lv_obj_add_flag(bar_progress,   LV_OBJ_FLAG_HIDDEN);
    if (lbl_run_hint)   lv_obj_add_flag(lbl_run_hint,   LV_OBJ_FLAG_HIDDEN);

    // Complete view
    if (lbl_comp_title) lv_obj_add_flag(lbl_comp_title, LV_OBJ_FLAG_HIDDEN);
    if (lbl_comp_dur)   lv_obj_add_flag(lbl_comp_dur,   LV_OBJ_FLAG_HIDDEN);
    if (lbl_comp_total) lv_obj_add_flag(lbl_comp_total, LV_OBJ_FLAG_HIDDEN);
    if (lbl_comp_rate)  lv_obj_add_flag(lbl_comp_rate,  LV_OBJ_FLAG_HIDDEN);
    if (lbl_comp_hint)  lv_obj_add_flag(lbl_comp_hint,  LV_OBJ_FLAG_HIDDEN);
}

// ============================================================================
// build_config_view — creates config LVGL objects (once)
// ============================================================================

static void build_config_view() {
    if (config_built) return;

    lbl_title = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_title, COL_CYAN, 0);
    lv_label_set_text(lbl_title, "BEACON FLOOD");
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 35);

    lbl_channel = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_channel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_channel, COL_GRAY, 0);
    lv_obj_align(lbl_channel, LV_ALIGN_TOP_MID, 0, 62);

    for (int i = 0; i < 4; i++) {
        lbl_params[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_font(lbl_params[i], &lv_font_montserrat_16, 0);
        lv_obj_set_pos(lbl_params[i], 30, 90 + i * 32);
    }

    lbl_total_rate = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_total_rate, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_total_rate, COL_CYAN_DIM, 0);
    lv_obj_align(lbl_total_rate, LV_ALIGN_TOP_MID, 0, 225);

    lbl_hint = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_hint, COL_GRAY, 0);
    lv_label_set_text(lbl_hint, "hold = START");
    lv_obj_align(lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -40);

    config_built = true;
}

// ============================================================================
// build_running_view — creates running LVGL objects (once)
// ============================================================================

static void build_running_view() {
    if (running_built) return;

    // Border glow: 360x360 circular object, magenta border
    border_glow = lv_obj_create(scr_root);
    lv_obj_set_size(border_glow, 360, 360);
    lv_obj_center(border_glow);
    lv_obj_set_style_radius(border_glow, 180, 0);
    lv_obj_set_style_bg_opa(border_glow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(border_glow, 4, 0);
    lv_obj_set_style_border_color(border_glow, lv_color_make(0xFF, 0x00, 0xFF), 0);
    lv_obj_set_style_border_opa(border_glow, LV_OPA_70, 0);
    lv_obj_clear_flag(border_glow, LV_OBJ_FLAG_CLICKABLE);

    lbl_run_title = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_run_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_run_title, lv_color_make(0xFF, 0x00, 0xFF), 0);
    lv_label_set_text(lbl_run_title, "BEACON FLOOD");
    lv_obj_align(lbl_run_title, LV_ALIGN_TOP_MID, 0, 35);

    lbl_run_ssids = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_run_ssids, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_run_ssids, COL_WHITE, 0);
    lv_obj_set_pos(lbl_run_ssids, 30, 80);

    lbl_run_rate = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_run_rate, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_run_rate, COL_WHITE, 0);
    lv_obj_set_pos(lbl_run_rate, 30, 110);

    lbl_run_sent = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_run_sent, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_run_sent, COL_GREEN, 0);
    lv_obj_set_pos(lbl_run_sent, 30, 140);

    lbl_run_time = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_run_time, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_run_time, COL_WHITE, 0);
    lv_obj_set_pos(lbl_run_time, 30, 170);

    bar_progress = lv_bar_create(scr_root);
    lv_obj_set_size(bar_progress, 240, 12);
    lv_obj_align(bar_progress, LV_ALIGN_TOP_MID, 0, 210);
    lv_obj_set_style_bg_color(bar_progress, COL_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_progress, lv_color_make(0xFF, 0x00, 0xFF), LV_PART_INDICATOR);
    lv_bar_set_range(bar_progress, 0, 100);
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);

    lbl_run_hint = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_run_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_run_hint, COL_GRAY, 0);
    lv_label_set_text(lbl_run_hint, "hold = STOP");
    lv_obj_align(lbl_run_hint, LV_ALIGN_BOTTOM_MID, 0, -40);

    running_built = true;
}

// ============================================================================
// build_complete_view — creates complete LVGL objects (once)
// ============================================================================

static void build_complete_view() {
    if (complete_built) return;

    lbl_comp_title = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_comp_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_comp_title, COL_GREEN, 0);
    lv_label_set_text(lbl_comp_title, "FLOOD COMPLETE");
    lv_obj_align(lbl_comp_title, LV_ALIGN_TOP_MID, 0, 50);

    lbl_comp_dur = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_comp_dur, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_comp_dur, COL_WHITE, 0);
    lv_obj_set_pos(lbl_comp_dur, 40, 110);

    lbl_comp_total = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_comp_total, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_comp_total, COL_WHITE, 0);
    lv_obj_set_pos(lbl_comp_total, 40, 145);

    lbl_comp_rate = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_comp_rate, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_comp_rate, COL_WHITE, 0);
    lv_obj_set_pos(lbl_comp_rate, 40, 180);

    lbl_comp_hint = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_comp_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_comp_hint, COL_GRAY, 0);
    lv_label_set_text(lbl_comp_hint, "tap = dismiss  hold = again");
    lv_obj_align(lbl_comp_hint, LV_ALIGN_BOTTOM_MID, 0, -40);

    complete_built = true;
}

// ============================================================================
// update_config_display — format each param with arrow indicator
// ============================================================================

static void update_config_display() {
    if (!config_built) return;
    AttackState* st = attack_get_state();

    // Channel label
    lv_label_set_text_fmt(lbl_channel, "CH %u", st->channel);

    // Param labels — each gets a ▶ prefix when selected
    static char buf[64];

    // Param 0: SSID count
    snprintf(buf, sizeof(buf), "%s SSIDs: %u",
             (param_cursor == 0) ? ">" : " ", st->ssid_count);
    lv_label_set_text(lbl_params[0], buf);
    lv_obj_set_style_text_color(lbl_params[0],
        (param_cursor == 0) ? COL_CYAN : COL_WHITE, 0);

    // Param 1: source
    const char* src_name = (st->ssid_source < 3) ? source_names[st->ssid_source] : "?";
    snprintf(buf, sizeof(buf), "%s Source: %s",
             (param_cursor == 1) ? ">" : " ", src_name);
    lv_label_set_text(lbl_params[1], buf);
    lv_obj_set_style_text_color(lbl_params[1],
        (param_cursor == 1) ? COL_CYAN : COL_WHITE, 0);

    // Param 2: tx rate
    snprintf(buf, sizeof(buf), "%s Rate: %u/s/AP",
             (param_cursor == 2) ? ">" : " ", st->tx_rate);
    lv_label_set_text(lbl_params[2], buf);
    lv_obj_set_style_text_color(lbl_params[2],
        (param_cursor == 2) ? COL_CYAN : COL_WHITE, 0);

    // Param 3: duration
    if (st->duration_sec == 0) {
        snprintf(buf, sizeof(buf), "%s Duration: infinite",
                 (param_cursor == 3) ? ">" : " ");
    } else {
        snprintf(buf, sizeof(buf), "%s Duration: %us",
                 (param_cursor == 3) ? ">" : " ", st->duration_sec);
    }
    lv_label_set_text(lbl_params[3], buf);
    lv_obj_set_style_text_color(lbl_params[3],
        (param_cursor == 3) ? COL_CYAN : COL_WHITE, 0);

    // Total rate preview
    uint32_t total = (uint32_t)st->ssid_count * st->tx_rate;
    lv_label_set_text_fmt(lbl_total_rate, "~%lu pkt/s total", (unsigned long)total);
}

// ============================================================================
// update_running_display — live stats
// ============================================================================

static void update_running_display() {
    if (!running_built) return;
    AttackState* st = attack_get_state();

    lv_label_set_text_fmt(lbl_run_ssids, "SSIDs: %u active", st->ssid_count);

    lv_label_set_text_fmt(lbl_run_rate, "Rate:  %.1f pkt/s", (double)st->stats.avg_tx_rate);

    lv_label_set_text_fmt(lbl_run_sent, "Sent:  %lu pkts",
                          (unsigned long)st->stats.packets_sent);

    uint32_t now = millis();
    uint32_t elapsed = (now - st->running_start_ms) / 1000;
    if (st->duration_sec == 0) {
        lv_label_set_text_fmt(lbl_run_time, "Time:  %lus / inf", (unsigned long)elapsed);
    } else {
        lv_label_set_text_fmt(lbl_run_time, "Time:  %lus / %us",
                              (unsigned long)elapsed, st->duration_sec);
    }

    // Progress bar
    int pct = 0;
    if (st->duration_sec > 0) {
        pct = (int)((elapsed * 100) / st->duration_sec);
        if (pct > 100) pct = 100;
    }
    lv_bar_set_value(bar_progress, pct, LV_ANIM_OFF);
}

// ============================================================================
// update_complete_display — final summary
// ============================================================================

static void update_complete_display() {
    if (!complete_built) return;
    AttackState* st = attack_get_state();

    uint32_t dur_ms = st->stats.end_time_ms - st->stats.start_time_ms;
    uint32_t dur_s  = dur_ms / 1000;
    lv_label_set_text_fmt(lbl_comp_dur,   "Duration: %lus", (unsigned long)dur_s);
    lv_label_set_text_fmt(lbl_comp_total, "Total TX: %lu pkts",
                          (unsigned long)st->stats.packets_sent);
    lv_label_set_text_fmt(lbl_comp_rate,  "Avg rate: %.1f pkt/s",
                          (double)st->stats.avg_tx_rate);
}

// ============================================================================
// show_phase_view — switch between config/running/complete
// ============================================================================

static void show_phase_view() {
    AttackState* st = attack_get_state();
    AttackPhase phase = st->phase;

    hide_all_views();

    switch (phase) {
        case ATTACK_IDLE:
        case ATTACK_CONFIG:
        case ATTACK_ARMED:
            build_config_view();
            update_config_display();
            if (lbl_title)      lv_obj_clear_flag(lbl_title,      LV_OBJ_FLAG_HIDDEN);
            if (lbl_channel)    lv_obj_clear_flag(lbl_channel,    LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < 4; i++) {
                if (lbl_params[i]) lv_obj_clear_flag(lbl_params[i], LV_OBJ_FLAG_HIDDEN);
            }
            if (lbl_total_rate) lv_obj_clear_flag(lbl_total_rate, LV_OBJ_FLAG_HIDDEN);
            if (lbl_hint)       lv_obj_clear_flag(lbl_hint,       LV_OBJ_FLAG_HIDDEN);
            break;

        case ATTACK_RUNNING:
            build_running_view();
            update_running_display();
            if (border_glow)    lv_obj_clear_flag(border_glow,    LV_OBJ_FLAG_HIDDEN);
            if (lbl_run_title)  lv_obj_clear_flag(lbl_run_title,  LV_OBJ_FLAG_HIDDEN);
            if (lbl_run_ssids)  lv_obj_clear_flag(lbl_run_ssids,  LV_OBJ_FLAG_HIDDEN);
            if (lbl_run_rate)   lv_obj_clear_flag(lbl_run_rate,   LV_OBJ_FLAG_HIDDEN);
            if (lbl_run_sent)   lv_obj_clear_flag(lbl_run_sent,   LV_OBJ_FLAG_HIDDEN);
            if (lbl_run_time)   lv_obj_clear_flag(lbl_run_time,   LV_OBJ_FLAG_HIDDEN);
            if (bar_progress)   lv_obj_clear_flag(bar_progress,   LV_OBJ_FLAG_HIDDEN);
            if (lbl_run_hint)   lv_obj_clear_flag(lbl_run_hint,   LV_OBJ_FLAG_HIDDEN);
            break;

        case ATTACK_COMPLETE:
            build_complete_view();
            update_complete_display();
            if (lbl_comp_title) lv_obj_clear_flag(lbl_comp_title, LV_OBJ_FLAG_HIDDEN);
            if (lbl_comp_dur)   lv_obj_clear_flag(lbl_comp_dur,   LV_OBJ_FLAG_HIDDEN);
            if (lbl_comp_total) lv_obj_clear_flag(lbl_comp_total, LV_OBJ_FLAG_HIDDEN);
            if (lbl_comp_rate)  lv_obj_clear_flag(lbl_comp_rate,  LV_OBJ_FLAG_HIDDEN);
            if (lbl_comp_hint)  lv_obj_clear_flag(lbl_comp_hint,  LV_OBJ_FLAG_HIDDEN);
            break;
    }

    last_shown_phase = phase;
}

// ============================================================================
// ScreenDef lifecycle
// ============================================================================

static void create() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
}

static void show() {
    AttackState* st = attack_get_state();
    if (st->phase == ATTACK_IDLE || st->type != ATTACK_BEACON_FLOOD) {
        attack_start(ATTACK_BEACON_FLOOD);
        param_cursor = 0;
    }
    show_phase_view();
    lv_screen_load(scr_root);
}

static void hide() {
    // Nothing to do — state persists in AttackState
}

static void update() {
    AttackState* st = attack_get_state();
    AttackPhase phase = st->phase;

    // Detect phase transitions
    if (phase != last_shown_phase) {
        if (phase == ATTACK_RUNNING) {
            haptic_play(16);
        } else if (phase == ATTACK_COMPLETE) {
            wifi_attack_stop();
            haptic_play(14);
        }
        show_phase_view();
        lv_refr_now(display_get_disp());
    }

    // Live updates during RUNNING
    if (phase == ATTACK_RUNNING) {
        uint32_t now = millis();
        if (now - last_ui_update >= 500) {
            last_ui_update = now;
            update_running_display();
            lv_refr_now(display_get_disp());
        }
    }
}

// ============================================================================
// Input handlers
// ============================================================================

void scr_beacon_flood_on_encoder(int8_t delta) {
    AttackState* st = attack_get_state();
    if (st->phase != ATTACK_CONFIG && st->phase != ATTACK_IDLE) return;

    switch (param_cursor) {
        case 0: {
            int v = (int)st->ssid_count + delta;
            if (v < 1)  v = 1;
            if (v > 50) v = 50;
            st->ssid_count = (uint8_t)v;
            break;
        }
        case 1: {
            int v = (int)st->ssid_source + delta;
            if (v < 0) v = 2;
            if (v > 2) v = 0;
            st->ssid_source = (uint8_t)v;
            break;
        }
        case 2: {
            int v = (int)st->tx_rate + delta * 5;
            if (v < 1)   v = 1;
            if (v > 100) v = 100;
            st->tx_rate = (uint16_t)v;
            break;
        }
        case 3: {
            int v = (int)st->duration_sec + delta * 5;
            if (v < 0)   v = 0;
            if (v > 300) v = 300;
            st->duration_sec = (uint16_t)v;
            break;
        }
        default: break;
    }

    update_config_display();
    haptic_click();
    lv_refr_now(display_get_disp());
}

void scr_beacon_flood_on_tap() {
    AttackState* st = attack_get_state();
    switch (st->phase) {
        case ATTACK_IDLE:
        case ATTACK_CONFIG:
        case ATTACK_ARMED:
            param_cursor = (param_cursor + 1) % 4;
            update_config_display();
            lv_refr_now(display_get_disp());
            break;
        case ATTACK_COMPLETE:
            // Dismiss — restart to CONFIG
            attack_start(ATTACK_BEACON_FLOOD);
            param_cursor = 0;
            show_phase_view();
            lv_refr_now(display_get_disp());
            break;
        default: break;
    }
}

void scr_beacon_flood_on_hold() {
    AttackState* st = attack_get_state();
    switch (st->phase) {
        case ATTACK_IDLE:
        case ATTACK_CONFIG:
        case ATTACK_ARMED:
            wifi_attack_start_beacon_flood();
            attack_confirm();
            break;
        case ATTACK_RUNNING:
            attack_stop();
            wifi_attack_stop();
            break;
        case ATTACK_COMPLETE:
            // Run again
            wifi_attack_start_beacon_flood();
            attack_confirm();
            break;
        default: break;
    }
}

// ============================================================================
// ScreenDef
// ============================================================================

const ScreenDef scr_beacon_flood_def = {
    .name    = "Beacon Flood",
    .group   = GROUP_WIFI,
    .id      = SCREEN_BEACON_FLOOD,
    .create  = create,
    .show    = show,
    .hide    = hide,
    .destroy = NULL,
    .update  = update,
    .enc_mode = ENC_ATTACK_PARAM
};
