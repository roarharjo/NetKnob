/* scr_probe_sniff.cpp — Probe Sniffer screen
 *
 * Displays captured 802.11 probe requests in a scrollable list.
 * Encoder hops channels; tap cycles selection; hold opens detail view.
 */

#include "scr_probe_sniff.h"
#include "display.h"
#include "haptic.h"
#include "wifi_probe_sniffer.h"
#include "wifi_scanner.h"
#include <Arduino.h>
#include <lvgl.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// LVGL root + state
// ---------------------------------------------------------------------------
static lv_obj_t* scr_root = NULL;

// List view objects
static lv_obj_t* lbl_status   = NULL;
static lv_obj_t* lbl_stats    = NULL;
static lv_obj_t* lbl_probes[7] = {};
static lv_obj_t* lbl_footer   = NULL;

// Detail view objects
static lv_obj_t* lbl_det_mac    = NULL;
static lv_obj_t* lbl_det_type   = NULL;
static lv_obj_t* lbl_det_vendor = NULL;
static lv_obj_t* lbl_det_ssids  = NULL;
static lv_obj_t* lbl_det_rssi   = NULL;
static lv_obj_t* lbl_det_count  = NULL;
static lv_obj_t* lbl_det_hint   = NULL;

// Screen state
static int16_t   scroll_offset  = 0;
static int16_t   selected_idx   = -1;   // -1 = none
static bool      showing_detail = false;
static bool      list_built     = false;
static bool      detail_built   = false;
static uint32_t  last_ui_update = 0;

// ---------------------------------------------------------------------------
// Build list view (once)
// ---------------------------------------------------------------------------
static void build_list_view() {
    if (list_built) return;

    // Status header — channel + title
    lbl_status = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_status, COL_CYAN, 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 30);

    // Stats line — probe / device / ssid counts
    lbl_stats = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_stats, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_stats, COL_GRAY, 0);
    lv_obj_align(lbl_stats, LV_ALIGN_TOP_MID, 0, 52);

    // 7 probe rows
    for (int i = 0; i < 7; i++) {
        lbl_probes[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_font(lbl_probes[i], &lv_font_montserrat_14, 0);
        lv_obj_set_pos(lbl_probes[i], 20, 75 + i * 28);
        lv_obj_set_width(lbl_probes[i], 300);
        lv_label_set_long_mode(lbl_probes[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(lbl_probes[i], COL_GRAY, 0);
    }

    // Footer
    lbl_footer = lv_label_create(scr_root);
    lv_label_set_text(lbl_footer, "dial=CH  tap=select");
    lv_obj_set_style_text_font(lbl_footer, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_footer, COL_GRAY, 0);
    lv_obj_align(lbl_footer, LV_ALIGN_BOTTOM_MID, 0, -35);

    list_built = true;
}

// ---------------------------------------------------------------------------
// Build detail view (once)
// ---------------------------------------------------------------------------
static void build_detail_view() {
    if (detail_built) return;

    // MAC address — large cyan header
    lbl_det_mac = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_det_mac, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_det_mac, COL_CYAN, 0);
    lv_obj_align(lbl_det_mac, LV_ALIGN_TOP_MID, 0, 40);

    // Type (Randomized / Real)
    lbl_det_type = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_det_type, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_type, COL_WHITE, 0);
    lv_obj_set_pos(lbl_det_type, 30, 75);

    // Vendor
    lbl_det_vendor = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_det_vendor, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_vendor, COL_WHITE, 0);
    lv_obj_set_pos(lbl_det_vendor, 30, 100);

    // SSIDs probed (wrapping)
    lbl_det_ssids = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_det_ssids, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_ssids, COL_GREEN, 0);
    lv_obj_set_pos(lbl_det_ssids, 30, 135);
    lv_obj_set_width(lbl_det_ssids, 280);
    lv_label_set_long_mode(lbl_det_ssids, LV_LABEL_LONG_WRAP);

    // RSSI
    lbl_det_rssi = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_det_rssi, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_rssi, COL_WHITE, 0);
    lv_obj_set_pos(lbl_det_rssi, 30, 230);

    // Probe count for this MAC
    lbl_det_count = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_det_count, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_count, COL_WHITE, 0);
    lv_obj_set_pos(lbl_det_count, 30, 255);

    // Hint
    lbl_det_hint = lv_label_create(scr_root);
    lv_label_set_text(lbl_det_hint, "tap = back");
    lv_obj_set_style_text_font(lbl_det_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_det_hint, COL_GRAY, 0);
    lv_obj_align(lbl_det_hint, LV_ALIGN_BOTTOM_MID, 0, -35);

    detail_built = true;
}

// ---------------------------------------------------------------------------
// Hide / show helpers
// ---------------------------------------------------------------------------

static void hide_list_objects() {
    if (!list_built) return;
    lv_obj_add_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_stats,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_footer, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 7; i++) lv_obj_add_flag(lbl_probes[i], LV_OBJ_FLAG_HIDDEN);
}

static void show_list_objects() {
    if (!list_built) return;
    lv_obj_clear_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_stats,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_footer, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 7; i++) lv_obj_clear_flag(lbl_probes[i], LV_OBJ_FLAG_HIDDEN);
}

static void hide_detail_objects() {
    if (!detail_built) return;
    lv_obj_add_flag(lbl_det_mac,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_type,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_vendor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_ssids,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_rssi,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_count,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_hint,   LV_OBJ_FLAG_HIDDEN);
}

static void show_detail_objects() {
    if (!detail_built) return;
    lv_obj_clear_flag(lbl_det_mac,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_type,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_vendor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_ssids,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_rssi,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_count,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_hint,   LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// update_list_display — refresh list view labels
// ---------------------------------------------------------------------------
static void update_list_display() {
    if (!list_built) return;
    ProbeSnifferState* s = probe_sniffer_get_state();
    WifiScannerState*  ws = scanner_get_state();

    // Status bar
    lv_label_set_text_fmt(lbl_status, "PROBE SNIFFER  CH %d", ws->current_channel);

    // Stats
    uint16_t filled = (s->total_count < PROBE_BUFFER_SIZE) ? s->total_count : PROBE_BUFFER_SIZE;
    lv_label_set_text_fmt(lbl_stats, "Probes: %u | Devs: %u | SSIDs: %u",
                          s->total_count, s->unique_macs, s->unique_ssids);

    // Probe rows — newest first
    for (int i = 0; i < 7; i++) {
        int list_pos = scroll_offset + i;    // position in newest-first virtual list
        if ((uint16_t)list_pos >= filled) {
            lv_label_set_text(lbl_probes[i], "");
            lv_obj_add_flag(lbl_probes[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        // Map newest-first list position to actual buffer index
        // write_index - 1 is newest, write_index - 2 is second newest, etc.
        uint16_t buf_idx = (s->write_index + PROBE_BUFFER_SIZE - 1 - list_pos) % PROBE_BUFFER_SIZE;
        const ProbeRequest& pr = s->probes[buf_idx];

        // Format: "  XX:XX:XX:* > SSID"  or  "> XX:XX:XX:* > SSID" if selected
        char row[64];
        const char* ssid_str = (pr.ssid_probed[0] != '\0') ? pr.ssid_probed : "(broadcast)";

        bool is_selected = (selected_idx == list_pos);
        if (is_selected) {
            snprintf(row, sizeof(row), "> %02X:%02X:%02X:* > %s",
                     pr.src_mac[0], pr.src_mac[1], pr.src_mac[2], ssid_str);
        } else {
            snprintf(row, sizeof(row), "  %02X:%02X:%02X:* > %s",
                     pr.src_mac[0], pr.src_mac[1], pr.src_mac[2], ssid_str);
        }

        lv_label_set_text(lbl_probes[i], row);
        lv_obj_clear_flag(lbl_probes[i], LV_OBJ_FLAG_HIDDEN);

        if (is_selected) {
            lv_obj_set_style_text_color(lbl_probes[i], COL_CYAN, 0);
        } else if (pr.mac_randomized) {
            lv_obj_set_style_text_color(lbl_probes[i], COL_GRAY, 0);
        } else {
            lv_obj_set_style_text_color(lbl_probes[i], COL_WHITE, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// update_detail_display — populate detail view for selected_idx
// ---------------------------------------------------------------------------
static void update_detail_display() {
    if (!detail_built || selected_idx < 0) return;

    ProbeSnifferState* s = probe_sniffer_get_state();
    uint16_t filled = (s->total_count < PROBE_BUFFER_SIZE) ? s->total_count : PROBE_BUFFER_SIZE;

    if ((uint16_t)selected_idx >= filled) return;

    uint16_t buf_idx = (s->write_index + PROBE_BUFFER_SIZE - 1 - selected_idx) % PROBE_BUFFER_SIZE;
    const ProbeRequest& pr = s->probes[buf_idx];

    // Full MAC
    lv_label_set_text_fmt(lbl_det_mac, "%02X:%02X:%02X:%02X:%02X:%02X",
                          pr.src_mac[0], pr.src_mac[1], pr.src_mac[2],
                          pr.src_mac[3], pr.src_mac[4], pr.src_mac[5]);

    // Type
    lv_label_set_text_fmt(lbl_det_type, "Type:   %s",
                          pr.mac_randomized ? "Randomized" : "Real");

    // Vendor (only meaningful for real MACs)
    lv_label_set_text_fmt(lbl_det_vendor, "Vendor: %s",
                          pr.mac_randomized ? "N/A" : oui_lookup(pr.src_mac));

    // Collect up to 5 unique SSIDs for this MAC
    char ssid_buf[160] = {};
    int  ssid_buf_pos  = 0;
    uint8_t ssid_lines = 0;
    char seen_ssids[5][33] = {};
    uint8_t seen_count = 0;

    for (uint16_t j = 0; j < filled && ssid_lines < 5; j++) {
        uint16_t jdx = (s->write_index + PROBE_BUFFER_SIZE - 1 - j) % PROBE_BUFFER_SIZE;
        const ProbeRequest& other = s->probes[jdx];

        if (memcmp(other.src_mac, pr.src_mac, 6) != 0) continue;
        if (other.ssid_probed[0] == '\0') continue;

        // Check if this SSID was already added
        bool already = false;
        for (uint8_t k = 0; k < seen_count; k++) {
            if (strcmp(seen_ssids[k], other.ssid_probed) == 0) { already = true; break; }
        }
        if (already) continue;

        strncpy(seen_ssids[seen_count], other.ssid_probed, 32);
        seen_ssids[seen_count][32] = '\0';
        seen_count++;

        ssid_buf_pos += snprintf(ssid_buf + ssid_buf_pos,
                                 sizeof(ssid_buf) - ssid_buf_pos,
                                 "%s\n", other.ssid_probed);
        ssid_lines++;
    }

    if (ssid_lines == 0) {
        lv_label_set_text(lbl_det_ssids, "SSIDs:  (broadcast only)");
    } else {
        // Remove trailing newline
        if (ssid_buf_pos > 0 && ssid_buf[ssid_buf_pos - 1] == '\n') {
            ssid_buf[ssid_buf_pos - 1] = '\0';
        }
        lv_label_set_text_fmt(lbl_det_ssids, "SSIDs:\n%s", ssid_buf);
    }

    // RSSI
    lv_label_set_text_fmt(lbl_det_rssi, "RSSI:   %d dBm", (int)pr.rssi);

    // Count probes from this MAC
    uint16_t mac_probe_count = 0;
    for (uint16_t j = 0; j < filled; j++) {
        uint16_t jdx = (s->write_index + PROBE_BUFFER_SIZE - 1 - j) % PROBE_BUFFER_SIZE;
        if (memcmp(s->probes[jdx].src_mac, pr.src_mac, 6) == 0) mac_probe_count++;
    }
    lv_label_set_text_fmt(lbl_det_count, "Count:  %u probe%s",
                          mac_probe_count, mac_probe_count == 1 ? "" : "s");
}

// ---------------------------------------------------------------------------
// show_list / show_detail
// ---------------------------------------------------------------------------
static void show_list() {
    build_list_view();
    show_list_objects();
    hide_detail_objects();
    showing_detail = false;
    update_list_display();
}

static void show_detail() {
    build_detail_view();
    hide_list_objects();
    show_detail_objects();
    showing_detail = true;
    update_detail_display();
}

// ---------------------------------------------------------------------------
// ScreenDef lifecycle
// ---------------------------------------------------------------------------

static void create() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
}

static void show_screen() {
    probe_sniffer_start();
    scroll_offset  = 0;
    selected_idx   = -1;
    show_list();
    lv_screen_load(scr_root);
}

static void hide() {
    probe_sniffer_stop();
}

static void update_screen() {
    probe_sniffer_update();

    if (!showing_detail) {
        uint32_t now = millis();
        if (now - last_ui_update >= 500) {
            last_ui_update = now;
            update_list_display();
            lv_refr_now(display_get_disp());
        }
    }
}

// ---------------------------------------------------------------------------
// Input handlers
// ---------------------------------------------------------------------------

void scr_probe_sniff_on_encoder(int8_t delta) {
    if (showing_detail) return;

    WifiScannerState* ws = scanner_get_state();
    int ch = (int)ws->current_channel + delta;
    // Wrap within CHANNEL_MIN..CHANNEL_MAX
    int range = CHANNEL_MAX - CHANNEL_MIN + 1;
    ch = ((ch - CHANNEL_MIN) % range + range) % range + CHANNEL_MIN;
    scanner_set_channel((uint8_t)ch);
    haptic_click();
    update_list_display();
    lv_refr_now(display_get_disp());
}

void scr_probe_sniff_on_tap() {
    haptic_click();

    if (showing_detail) {
        show_list();
        lv_refr_now(display_get_disp());
        return;
    }

    // List view: cycle selected_idx through entries
    ProbeSnifferState* s = probe_sniffer_get_state();
    uint16_t filled = (s->total_count < PROBE_BUFFER_SIZE) ? s->total_count : PROBE_BUFFER_SIZE;

    if (filled == 0) return;

    if (selected_idx < 0) {
        // Start at scroll_offset
        selected_idx = scroll_offset;
    } else {
        selected_idx++;
        if ((uint16_t)selected_idx >= filled) {
            selected_idx = 0;
        }
    }

    update_list_display();
    lv_refr_now(display_get_disp());
}

void scr_probe_sniff_on_hold() {
    if (showing_detail) return;
    if (selected_idx < 0) return;

    haptic_double_click();
    show_detail();
    lv_refr_now(display_get_disp());
}

// ---------------------------------------------------------------------------
// ScreenDef
// ---------------------------------------------------------------------------

const ScreenDef scr_probe_sniff_def = {
    .name    = "Probe Sniffer",
    .group   = GROUP_WIFI,
    .id      = SCREEN_PROBE_SNIFF,
    .create  = create,
    .show    = show_screen,
    .hide    = hide,
    .destroy = NULL,
    .update  = update_screen,
    .enc_mode = ENC_CHANNEL_HOP
};
