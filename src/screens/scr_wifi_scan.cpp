/* scr_wifi_scan.cpp — WiFi Scanner screen module
 *
 * Extracted from display.cpp (Section C WiFi rendering).
 * Implements ScreenDef lifecycle: create/show/hide/update.
 * All LVGL objects are children of scr_root (created once via lv_obj_create(NULL)).
 */

#include "scr_wifi_scan.h"
#include "display.h"
#include "wifi_scanner.h"
#include "haptic.h"
#include <Arduino.h>
#include <lvgl.h>
#include <string.h>

static lv_obj_t* scr_root = NULL;

// --- Scan view LVGL objects ---
#define VISIBLE_AP_ROWS 4
static lv_obj_t *lbl_channel   = NULL;
static lv_obj_t *lbl_freq      = NULL;
static lv_obj_t *lbl_ap_count  = NULL;
static lv_obj_t *arc_activity  = NULL;
static lv_obj_t *lbl_scanning  = NULL;
static lv_obj_t *lbl_no_aps    = NULL;
static lv_obj_t *lbl_footer    = NULL;
static lv_obj_t *lbl_ap_ssid[VISIBLE_AP_ROWS]  = {};
static lv_obj_t *lbl_ap_rssi[VISIBLE_AP_ROWS]  = {};
static bool scan_view_built = false;

// --- Detail view LVGL objects ---
static lv_obj_t *lbl_detail_ssid   = NULL;
static lv_obj_t *lbl_detail_mac    = NULL;
static lv_obj_t *lbl_detail_rssi   = NULL;
static lv_obj_t *lbl_detail_ch     = NULL;
static lv_obj_t *lbl_detail_enc    = NULL;
static lv_obj_t *lbl_detail_vendor = NULL;
static lv_obj_t *lbl_detail_hint   = NULL;
static lv_obj_t *arc_detail        = NULL;
static bool detail_view_built = false;

// --- Arc animation state ---
static int16_t arc_base_value = 20;
static int16_t arc_current = 20;
static int8_t arc_pulse_dir = 1;
static uint32_t arc_pulse_last = 0;

// Dirty flag for this screen
static bool dirty = false;
static uint8_t prev_ap_count = 0xFF;
static uint32_t last_live_update = 0;

// --- Helper ---
static const char* enc_short(uint8_t enc) {
    switch (enc) {
        case 0: return "OPEN";
        case 1: return "WEP";
        case 2: return "WPA";
        case 3: return "WPA2";
        case 4: return "WPA3";
        default: return "?";
    }
}

static void mark_dirty() { dirty = true; }

// ============================================================================
// Build scan screen (creates all LVGL objects once on scr_root)
// ============================================================================

static void build_scan_screen()
{
    lv_obj_t *scr = scr_root;
    lv_obj_set_style_bg_color(scr, COL_BG, 0);

    // Subtle center glow — concentric circles fading outward
    static const uint16_t glow_sizes[] = {60, 140, 240, 320};
    static const uint8_t  glow_opas[]  = {35, 20, 10, 5};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *g = lv_obj_create(scr);
        lv_obj_set_size(g, glow_sizes[i], glow_sizes[i]);
        lv_obj_center(g);
        lv_obj_set_style_radius(g, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(g, COL_CYAN_DIM, 0);
        lv_obj_set_style_bg_opa(g, glow_opas[i], 0);
        lv_obj_set_style_border_width(g, 0, 0);
        lv_obj_clear_flag(g, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    }

    // Outer decorative ring — thin, always visible
    lv_obj_t *ring_deco = lv_arc_create(scr);
    lv_obj_set_size(ring_deco, 356, 356);
    lv_obj_center(ring_deco);
    lv_arc_set_rotation(ring_deco, 0);
    lv_arc_set_bg_angles(ring_deco, 0, 360);
    lv_arc_set_range(ring_deco, 0, 100);
    lv_arc_set_value(ring_deco, 0);
    lv_obj_set_style_arc_color(ring_deco, lv_color_make(0x0C, 0x14, 0x1A), LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring_deco, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ring_deco, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ring_deco, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(ring_deco, LV_OBJ_FLAG_CLICKABLE);

    // Background texture — concentric dim rings for radar/scope feel
    static const uint16_t ring_sizes[] = {140, 220, 300};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *ring = lv_arc_create(scr);
        lv_obj_set_size(ring, ring_sizes[i], ring_sizes[i]);
        lv_obj_center(ring);
        lv_arc_set_rotation(ring, 0);
        lv_arc_set_bg_angles(ring, 0, 360);
        lv_arc_set_range(ring, 0, 100);
        lv_arc_set_value(ring, 0);
        lv_obj_set_style_arc_color(ring, lv_color_make(0x10, 0x14, 0x1A), LV_PART_MAIN);
        lv_obj_set_style_arc_width(ring, 1, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(ring, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    }

    // Crosshair lines — subtle horizontal and vertical
    lv_obj_t *h_line = lv_obj_create(scr);
    lv_obj_set_size(h_line, 320, 1);
    lv_obj_center(h_line);
    lv_obj_set_style_bg_color(h_line, lv_color_make(0x10, 0x14, 0x1A), 0);
    lv_obj_set_style_bg_opa(h_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(h_line, 0, 0);
    lv_obj_clear_flag(h_line, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    lv_obj_t *v_line = lv_obj_create(scr);
    lv_obj_set_size(v_line, 1, 320);
    lv_obj_center(v_line);
    lv_obj_set_style_bg_color(v_line, lv_color_make(0x10, 0x14, 0x1A), 0);
    lv_obj_set_style_bg_opa(v_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(v_line, 0, 0);
    lv_obj_clear_flag(v_line, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    // Activity arc — outer ring, fills with AP count. Neon cyan glow.
    arc_activity = lv_arc_create(scr);
    lv_obj_set_size(arc_activity, 350, 350);
    lv_obj_center(arc_activity);
    lv_arc_set_rotation(arc_activity, 135);
    lv_arc_set_bg_angles(arc_activity, 0, 270);
    lv_arc_set_range(arc_activity, 0, 1000);
    lv_arc_set_value(arc_activity, 20);
    lv_obj_set_style_arc_color(arc_activity, COL_DARK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_activity, COL_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_activity, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_activity, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_activity, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc_activity, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(arc_activity, LV_OBJ_FLAG_CLICKABLE);

    // Channel label — large, cyan, center-top
    lbl_channel = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_channel, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_channel, COL_CYAN, 0);
    lv_obj_align(lbl_channel, LV_ALIGN_TOP_MID, 0, 30);

    // Frequency — 16pt
    lbl_freq = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_freq, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_freq, COL_GRAY, 0);
    lv_obj_align(lbl_freq, LV_ALIGN_TOP_MID, 0, 58);

    // AP count — 16pt
    lbl_ap_count = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_ap_count, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_ap_count, COL_CYAN_DIM, 0);
    lv_obj_align(lbl_ap_count, LV_ALIGN_TOP_MID, 0, 78);

    // Scanning label — 16pt, initially hidden
    lbl_scanning = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_scanning, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_scanning, COL_CYAN_DIM, 0);
    lv_obj_align(lbl_scanning, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_add_flag(lbl_scanning, LV_OBJ_FLAG_HIDDEN);

    // No-APs label — 16pt
    lbl_no_aps = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_no_aps, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_no_aps, "No signals detected");
    lv_obj_set_style_text_color(lbl_no_aps, COL_GRAY, 0);
    lv_obj_align(lbl_no_aps, LV_ALIGN_CENTER, 0, 10);
    lv_obj_add_flag(lbl_no_aps, LV_OBJ_FLAG_HIDDEN);

    // AP list — 4 rows at 20pt, generous spacing
    for (int i = 0; i < VISIBLE_AP_ROWS; i++) {
        int y = 108 + i * 44;

        lbl_ap_ssid[i] = lv_label_create(scr);
        lv_obj_set_style_text_font(lbl_ap_ssid[i], &lv_font_montserrat_20, 0);
        lv_obj_set_width(lbl_ap_ssid[i], 190);
        lv_label_set_long_mode(lbl_ap_ssid[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(lbl_ap_ssid[i], COL_GRAY, 0);
        lv_obj_set_pos(lbl_ap_ssid[i], 50, y);
        lv_obj_add_flag(lbl_ap_ssid[i], LV_OBJ_FLAG_HIDDEN);

        lbl_ap_rssi[i] = lv_label_create(scr);
        lv_obj_set_style_text_font(lbl_ap_rssi[i], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl_ap_rssi[i], COL_GRAY, 0);
        lv_obj_set_pos(lbl_ap_rssi[i], 258, y + 2);
        lv_obj_add_flag(lbl_ap_rssi[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Footer — encryption summary
    lbl_footer = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_footer, COL_GRAY, 0);
    lv_obj_align(lbl_footer, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_flag(lbl_footer, LV_OBJ_FLAG_HIDDEN);

    scan_view_built = true;
    Serial.println("[scr_wifi] scan view built");
}

// ============================================================================
// Build detail screen (creates detail LVGL objects on scr_root)
// ============================================================================

static void build_detail_screen()
{
    lv_obj_t *scr = scr_root;

    // Decorative outer ring — always visible, dim
    lv_obj_t *ring_outer = lv_arc_create(scr);
    lv_obj_set_size(ring_outer, 350, 350);
    lv_obj_center(ring_outer);
    lv_arc_set_rotation(ring_outer, 0);
    lv_arc_set_bg_angles(ring_outer, 0, 360);
    lv_arc_set_range(ring_outer, 0, 100);
    lv_arc_set_value(ring_outer, 0);
    lv_obj_set_style_arc_color(ring_outer, COL_DARK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring_outer, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ring_outer, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ring_outer, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(ring_outer, LV_OBJ_FLAG_CLICKABLE);

    // RSSI signal arc — shows selected AP strength
    arc_detail = lv_arc_create(scr);
    lv_obj_set_size(arc_detail, 350, 350);
    lv_obj_center(arc_detail);
    lv_arc_set_rotation(arc_detail, 135);
    lv_arc_set_bg_angles(arc_detail, 0, 270);
    lv_arc_set_range(arc_detail, 0, 1000);
    lv_arc_set_value(arc_detail, 20);
    lv_obj_set_style_arc_color(arc_detail, lv_color_make(0x0C, 0x10, 0x16), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_detail, COL_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_detail, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_detail, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_detail, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc_detail, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(arc_detail, LV_OBJ_FLAG_CLICKABLE);

    // SSID — large, cyan, centered
    lbl_detail_ssid = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_detail_ssid, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_detail_ssid, COL_CYAN, 0);
    lv_obj_set_width(lbl_detail_ssid, 260);
    lv_label_set_long_mode(lbl_detail_ssid, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(lbl_detail_ssid, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_detail_ssid, LV_ALIGN_TOP_MID, 0, 45);

    // Thin separator line
    lv_obj_t *sep = lv_obj_create(scr);
    lv_obj_set_size(sep, 200, 1);
    lv_obj_align(sep, LV_ALIGN_TOP_MID, 0, 85);
    lv_obj_set_style_bg_color(sep, COL_CYAN_DIM, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_clear_flag(sep, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    // Info labels — centered, 16pt
    int y_start = 100;
    int y_step = 32;

    lbl_detail_mac = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_detail_mac, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_detail_mac, COL_GRAY, 0);
    lv_obj_align(lbl_detail_mac, LV_ALIGN_TOP_MID, 0, y_start);

    lbl_detail_rssi = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_detail_rssi, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_detail_rssi, LV_ALIGN_TOP_MID, 0, y_start + y_step);

    lbl_detail_ch = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_detail_ch, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_detail_ch, COL_GRAY, 0);
    lv_obj_align(lbl_detail_ch, LV_ALIGN_TOP_MID, 0, y_start + y_step * 2);

    lbl_detail_enc = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_detail_enc, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_detail_enc, COL_GRAY, 0);
    lv_obj_align(lbl_detail_enc, LV_ALIGN_TOP_MID, 0, y_start + y_step * 3);

    lbl_detail_vendor = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_detail_vendor, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_detail_vendor, COL_GRAY, 0);
    lv_obj_align(lbl_detail_vendor, LV_ALIGN_TOP_MID, 0, y_start + y_step * 4);

    // Hint
    lbl_detail_hint = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_detail_hint, COL_CYAN_DIM, 0);
    lv_label_set_text(lbl_detail_hint, "[ tap = back ]");
    lv_obj_align(lbl_detail_hint, LV_ALIGN_BOTTOM_MID, 0, -45);

    detail_view_built = true;
}

// ============================================================================
// Scan view visibility helpers
// ============================================================================

static void show_scan_objects() {
    // Scan view objects are managed by render functions via hidden flags
    // Just ensure the scan-view container objects exist and are not forcibly hidden
    // The render functions handle individual visibility
}

static void hide_scan_objects() {
    if (!scan_view_built) return;
    lv_obj_add_flag(lbl_channel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_freq, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_ap_count, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(arc_activity, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_scanning, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_no_aps, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_footer, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < VISIBLE_AP_ROWS; i++) {
        lv_obj_add_flag(lbl_ap_ssid[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_ap_rssi[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void unhide_scan_objects() {
    if (!scan_view_built) return;
    lv_obj_clear_flag(lbl_channel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_freq, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(arc_activity, LV_OBJ_FLAG_HIDDEN);
    // ap_count, scanning, no_aps, footer, rows — managed by render functions
}

static void hide_detail_objects() {
    if (!detail_view_built) return;
    lv_obj_add_flag(lbl_detail_ssid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_detail_mac, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_detail_rssi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_detail_ch, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_detail_enc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_detail_vendor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_detail_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(arc_detail, LV_OBJ_FLAG_HIDDEN);
}

static void unhide_detail_objects() {
    if (!detail_view_built) return;
    lv_obj_clear_flag(lbl_detail_ssid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_detail_mac, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_detail_rssi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_detail_ch, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_detail_enc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_detail_vendor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_detail_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(arc_detail, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================================
// Render: scanning animation (no APs yet)
// ============================================================================

static void render_scanning(uint8_t channel)
{
    unhide_scan_objects();
    hide_detail_objects();

    lv_label_set_text_fmt(lbl_channel, "CH %u", channel);
    lv_label_set_text_fmt(lbl_freq, "%u MHz", channel_to_freq(channel));

    lv_obj_add_flag(lbl_ap_count, LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_value(arc_activity, 20);

    // Animated scanning text
    static uint8_t dot_counter = 0;
    const char *dots[] = {"~ scanning ~", "~ scanning. ~", "~ scanning.. ~", "~ scanning... ~"};
    lv_label_set_text(lbl_scanning, dots[dot_counter & 3]);
    dot_counter++;
    lv_obj_clear_flag(lbl_scanning, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(lbl_no_aps, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_footer, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < VISIBLE_AP_ROWS; i++) {
        lv_obj_add_flag(lbl_ap_ssid[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_ap_rssi[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// ============================================================================
// Render: scan list (APs found)
// ============================================================================

static void render_scan_list(WifiScannerState *state)
{
    unhide_scan_objects();
    hide_detail_objects();

    // Header
    lv_label_set_text_fmt(lbl_channel, "CH %u", state->current_channel);
    lv_label_set_text_fmt(lbl_freq, "%u MHz", channel_to_freq(state->current_channel));

    // AP count label
    lv_label_set_text_fmt(lbl_ap_count, "%u signal%s", state->ap_count, state->ap_count == 1 ? "" : "s");
    lv_obj_clear_flag(lbl_ap_count, LV_OBJ_FLAG_HIDDEN);

    // Arc target follows selected AP — pulse function does the actual rendering
    if (state->ap_count > 0) {
        uint8_t idx = state->selected_index < state->ap_count ? state->selected_index : 0;
        int8_t sel_rssi = state->ap_list[idx].rssi;
        arc_base_value = rssi_to_arc(sel_rssi);
        lv_obj_set_style_arc_color(arc_activity, rssi_arc_color(sel_rssi), LV_PART_INDICATOR);
    } else {
        arc_base_value = 20;
        lv_obj_set_style_arc_color(arc_activity, COL_CYAN_DIM, LV_PART_INDICATOR);
    }

    lv_obj_add_flag(lbl_scanning, LV_OBJ_FLAG_HIDDEN);

    if (state->ap_count == 0) {
        lv_obj_clear_flag(lbl_no_aps, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_footer, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < VISIBLE_AP_ROWS; i++) {
            lv_obj_add_flag(lbl_ap_ssid[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_ap_rssi[i], LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(lbl_no_aps, LV_OBJ_FLAG_HIDDEN);
        uint8_t visible = state->ap_count < VISIBLE_AP_ROWS ? state->ap_count : VISIBLE_AP_ROWS;

        // Count encryption types for footer
        uint8_t n_open = 0, n_wep = 0, n_wpa = 0, n_wpa2 = 0, n_wpa3 = 0;
        for (uint8_t j = 0; j < state->ap_count; j++) {
            switch (state->ap_list[j].encryption) {
                case 0: n_open++; break;
                case 1: n_wep++; break;
                case 2: n_wpa++; break;
                case 3: n_wpa2++; break;
                case 4: n_wpa3++; break;
            }
        }

        for (int i = 0; i < visible; i++) {
            const AccessPoint *ap = &state->ap_list[i];
            bool selected = (i == state->selected_index);
            const char *name = (ap->hidden || ap->ssid[0] == '\0') ? "[hidden]" : ap->ssid;

            // SSID with encryption indicator
            if (selected) {
                if (ap->encryption == 0) {
                    lv_label_set_text_fmt(lbl_ap_ssid[i], "> %s [OPEN]", name);
                } else {
                    lv_label_set_text_fmt(lbl_ap_ssid[i], "> %s", name);
                }
                lv_obj_set_style_text_color(lbl_ap_ssid[i], COL_CYAN, 0);
            } else {
                if (ap->encryption == 0) {
                    lv_label_set_text_fmt(lbl_ap_ssid[i], "%s [OPEN]", name);
                    lv_obj_set_style_text_color(lbl_ap_ssid[i], COL_RED, 0);
                } else {
                    lv_label_set_text(lbl_ap_ssid[i], name);
                    lv_obj_set_style_text_color(lbl_ap_ssid[i], COL_GRAY, 0);
                }
            }

            // RSSI: signal bars + dBm, colored
            lv_label_set_text_fmt(lbl_ap_rssi[i], "%s %d dBm", rssi_bars(ap->rssi), ap->rssi);
            lv_obj_set_style_text_color(lbl_ap_rssi[i], rssi_color(ap->rssi), 0);

            lv_obj_clear_flag(lbl_ap_ssid[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_ap_rssi[i], LV_OBJ_FLAG_HIDDEN);
        }

        for (int i = visible; i < VISIBLE_AP_ROWS; i++) {
            lv_obj_add_flag(lbl_ap_ssid[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_ap_rssi[i], LV_OBJ_FLAG_HIDDEN);
        }

        // Footer — encryption summary
        char footer[64] = {};
        int pos = 0;
        if (n_wpa3) pos += snprintf(footer + pos, sizeof(footer) - pos, "%d WPA3  ", n_wpa3);
        if (n_wpa2) pos += snprintf(footer + pos, sizeof(footer) - pos, "%d WPA2  ", n_wpa2);
        if (n_wpa)  pos += snprintf(footer + pos, sizeof(footer) - pos, "%d WPA  ", n_wpa);
        if (n_wep)  pos += snprintf(footer + pos, sizeof(footer) - pos, "%d WEP  ", n_wep);
        if (n_open) pos += snprintf(footer + pos, sizeof(footer) - pos, "%d OPEN", n_open);
        lv_label_set_text(lbl_footer, footer);
        if (n_open > 0) {
            lv_obj_set_style_text_color(lbl_footer, COL_ORANGE, 0);
        } else {
            lv_obj_set_style_text_color(lbl_footer, COL_GRAY, 0);
        }
        lv_obj_clear_flag(lbl_footer, LV_OBJ_FLAG_HIDDEN);
    }
}

// ============================================================================
// Render: detail view
// ============================================================================

static void render_detail(AccessPoint *ap)
{
    hide_scan_objects();
    unhide_detail_objects();

    // Update SSID
    if (ap->hidden || ap->ssid[0] == '\0') {
        lv_label_set_text_fmt(lbl_detail_ssid, "[Hidden]\n%02X:%02X:%02X:%02X:%02X:%02X",
                              ap->bssid[0], ap->bssid[1], ap->bssid[2],
                              ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    } else {
        lv_label_set_text(lbl_detail_ssid, ap->ssid);
    }

    // Update MAC
    lv_label_set_text_fmt(lbl_detail_mac, "MAC  %02X:%02X:%02X:%02X:%02X:%02X",
                          ap->bssid[0], ap->bssid[1], ap->bssid[2],
                          ap->bssid[3], ap->bssid[4], ap->bssid[5]);

    // Update RSSI with color + arc
    lv_obj_set_style_text_color(lbl_detail_rssi, rssi_color(ap->rssi), 0);
    lv_label_set_text_fmt(lbl_detail_rssi, "%d dBm  %s", ap->rssi, rssi_bars(ap->rssi));
    if (arc_detail) {
        lv_arc_set_value(arc_detail, rssi_to_arc(ap->rssi));
        lv_obj_set_style_arc_color(arc_detail, rssi_color(ap->rssi), LV_PART_INDICATOR);
    }

    // Update channel
    lv_label_set_text_fmt(lbl_detail_ch, "CH    %d  (%d MHz)", ap->channel, channel_to_freq(ap->channel));

    // Update encryption
    lv_label_set_text_fmt(lbl_detail_enc, "Enc   %s", encryption_str(ap->encryption));

    // Update vendor
    lv_label_set_text_fmt(lbl_detail_vendor, "Vendor  %s", oui_lookup(ap->bssid));
}

// ============================================================================
// Live data update — only arc + RSSI numbers, no layout rebuild
// ============================================================================

static void update_live(WifiScannerState *state)
{
    if (!scan_view_built || !arc_activity) return;

    uint32_t now = millis();
    if (now - last_live_update < 250) return;  // 4Hz — fluid updates
    last_live_update = now;

    // Arc target follows selected AP — pulse function renders smoothly
    if (state->ap_count > 0) {
        uint8_t idx = state->selected_index < state->ap_count ? state->selected_index : 0;
        int8_t sel_rssi = state->ap_list[idx].rssi;
        arc_base_value = rssi_to_arc(sel_rssi);
        lv_obj_set_style_arc_color(arc_activity, rssi_arc_color(sel_rssi), LV_PART_INDICATOR);
    }

    // Update visible rows — both SSID and RSSI to stay in sync after sort
    uint8_t visible = state->ap_count < VISIBLE_AP_ROWS ? state->ap_count : VISIBLE_AP_ROWS;
    for (int i = 0; i < visible; i++) {
        const AccessPoint *ap = &state->ap_list[i];
        bool selected = (i == state->selected_index);
        const char *name = (ap->hidden || ap->ssid[0] == '\0') ? "[hidden]" : ap->ssid;

        // SSID — keep in sync with current sort order
        if (selected) {
            if (ap->encryption == 0) {
                lv_label_set_text_fmt(lbl_ap_ssid[i], "> %s [OPEN]", name);
            } else {
                lv_label_set_text_fmt(lbl_ap_ssid[i], "> %s", name);
            }
            lv_obj_set_style_text_color(lbl_ap_ssid[i], COL_CYAN, 0);
        } else {
            if (ap->encryption == 0) {
                lv_label_set_text_fmt(lbl_ap_ssid[i], "%s [OPEN]", name);
                lv_obj_set_style_text_color(lbl_ap_ssid[i], COL_RED, 0);
            } else {
                lv_label_set_text(lbl_ap_ssid[i], name);
                lv_obj_set_style_text_color(lbl_ap_ssid[i], COL_GRAY, 0);
            }
        }

        // RSSI text and color
        lv_label_set_text_fmt(lbl_ap_rssi[i], "%s %d dBm", rssi_bars(ap->rssi), ap->rssi);
        lv_obj_set_style_text_color(lbl_ap_rssi[i], rssi_color(ap->rssi), 0);
    }

    lv_refr_now(display_get_disp());
}

// ============================================================================
// Arc pulse — gentle breathing animation
// ============================================================================

static void update_arc_pulse()
{
    if (!arc_activity || !scan_view_built) return;

    uint32_t now = millis();
    if (now - arc_pulse_last < 33) return;  // 30fps, fluid
    arc_pulse_last = now;

    // Smooth interpolation — move 6% of remaining distance each frame
    int16_t diff = arc_base_value - arc_current;
    if (diff != 0) {
        int16_t step = diff / 16;  // ~6% per frame
        if (step == 0) step = (diff > 0) ? 1 : -1;  // Always make progress
        arc_current += step;
    }

    // Gentle breathing on top of interpolated value
    static uint16_t phase = 0;
    phase = (phase + 1) % 240;
    int16_t quarter = 60;
    int16_t wave;
    if (phase < quarter) wave = phase;
    else if (phase < 3 * quarter) wave = 120 - phase;
    else wave = phase - 240;
    int16_t offset = wave * 10 / 60;  // +/-10 in 1000-scale

    int16_t val = arc_current + offset;
    if (val < 20) val = 20;
    if (val > 980) val = 980;
    lv_arc_set_value(arc_activity, val);
    // No lv_refr_now here — update_live() handles rendering
}

// ============================================================================
// Lifecycle callbacks
// ============================================================================

static void create() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
    build_scan_screen();  // Build scan view objects on scr_root
}

static void show() {
    lv_screen_load(scr_root);
    dirty = true;
}

static void hide() {
    // State preserved in WifiScannerState — nothing to do
}

static void update() {
    WifiScannerState *s = scanner_get_state();

    scanner_update();

    // Dirty check on AP count change
    if (s->ap_count != prev_ap_count) {
        prev_ap_count = s->ap_count;
        dirty = true;
    }

    // Exit detail view if all APs aged out
    if (s->detail_view && s->ap_count == 0) {
        s->detail_view = false;
        hide_detail_objects();
        unhide_scan_objects();
        dirty = true;
    }

    // Render when dirty
    if (dirty) {
        if (s->detail_view) {
            render_detail(&s->ap_list[s->selected_index]);
        } else if (s->ap_count == 0 && s->scanning) {
            render_scanning(s->current_channel);
        } else {
            render_scan_list(s);
        }
        lv_refr_now(display_get_disp());
        dirty = false;
    }

    // Live data updates (throttled)
    if (!s->detail_view && s->ap_count > 0) {
        update_live(s);
        update_arc_pulse();
    }
}

// ============================================================================
// Input handlers
// ============================================================================

void scr_wifi_scan_on_encoder(int8_t delta) {
    WifiScannerState *s = scanner_get_state();
    if (s->detail_view) return;
    uint8_t ch = s->current_channel;
    ch = ((ch - 1 + delta + CHANNEL_MAX) % CHANNEL_MAX) + 1;
    scanner_set_channel(ch);
    haptic_click();
    prev_ap_count = 0xFF;  // Force dirty on channel change
    dirty = true;
}

void scr_wifi_scan_on_tap() {
    WifiScannerState *s = scanner_get_state();
    if (s->detail_view) {
        s->detail_view = false;
        hide_detail_objects();
        unhide_scan_objects();
        dirty = true;
    } else if (s->ap_count > 0) {
        s->selected_index = (s->selected_index + 1) % s->ap_count;
        memcpy(s->selected_bssid, s->ap_list[s->selected_index].bssid, 6);
        dirty = true;
    }
}

void scr_wifi_scan_on_hold() {
    WifiScannerState *s = scanner_get_state();
    if (!s->detail_view && s->ap_count > 0) {
        s->detail_view = true;
        haptic_double_click();
        // Build detail view if not yet built
        if (!detail_view_built) build_detail_screen();
        dirty = true;
    }
}

// ============================================================================
// ScreenDef
// ============================================================================

const ScreenDef scr_wifi_scan_def = {
    .name = "WiFi Scanner",
    .group = GROUP_WIFI,
    .id = SCREEN_WIFI_SCAN,
    .create = create,
    .show = show,
    .hide = hide,
    .destroy = NULL,
    .update = update,
    .enc_mode = ENC_CHANNEL_HOP
};
