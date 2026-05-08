/* scr_ble_scan.cpp — BLE Scanner screen module
 *
 * Implements ScreenDef lifecycle: create/show/hide/update.
 * All LVGL objects are children of scr_root (created once via lv_obj_create(NULL)).
 * Follows same patterns as scr_wifi_scan.cpp.
 */

#include "scr_ble_scan.h"
#include "display.h"
#include "ble_scanner.h"
#include "haptic.h"
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

static lv_obj_t* scr_root = NULL;

// --- List view LVGL objects ---
#define VISIBLE_ROWS 6
static lv_obj_t* lbl_title      = NULL;
static lv_obj_t* lbl_status     = NULL;
static lv_obj_t* lbl_rows[VISIBLE_ROWS]  = {};
static lv_obj_t* lbl_rssi[VISIBLE_ROWS]  = {};
static bool list_view_built = false;

// --- Detail view LVGL objects ---
static lv_obj_t* lbl_det_name   = NULL;
static lv_obj_t* lbl_det_mac    = NULL;
static lv_obj_t* lbl_det_type   = NULL;
static lv_obj_t* lbl_det_rssi   = NULL;
static lv_obj_t* lbl_det_addr   = NULL;
static lv_obj_t* lbl_det_mfr    = NULL;
static lv_obj_t* lbl_det_svc    = NULL;
static lv_obj_t* lbl_det_tx     = NULL;
static lv_obj_t* lbl_det_hint   = NULL;
static bool detail_built = false;

// Dirty flag + throttle
static bool dirty = true;
static uint32_t last_update = 0;

// ============================================================================
// Helpers
// ============================================================================

static const char* type_prefix(uint8_t t) {
    switch (t) {
        case BLE_TYPE_PHONE:      return "[P]";
        case BLE_TYPE_COMPUTER:   return "[C]";
        case BLE_TYPE_WATCH:      return "[W]";
        case BLE_TYPE_HEADPHONES: return "[H]";
        case BLE_TYPE_SPEAKER:    return "[S]";
        case BLE_TYPE_BEACON:     return "[B]";
        case BLE_TYPE_IOT:        return "[I]";
        case BLE_TYPE_TRACKER:    return "[T]";
        case BLE_TYPE_TV:         return "[V]";
        case BLE_TYPE_PERIPHERAL: return "[K]";
        default:                  return "[?]";
    }
}

static const char* addr_type_str(uint8_t addr_type) {
    switch (addr_type) {
        case 0:  return "Public";
        case 1:  return "Random";
        case 2:  return "RPA";
        default: return "?";
    }
}

// ============================================================================
// Build list view (creates all list LVGL objects once on scr_root)
// ============================================================================

static void build_list_view() {
    lv_obj_t* scr = scr_root;
    lv_obj_set_style_bg_color(scr, COL_BG, 0);

    // Outer decorative ring
    lv_obj_t* ring = lv_arc_create(scr);
    lv_obj_set_size(ring, 356, 356);
    lv_obj_center(ring);
    lv_arc_set_rotation(ring, 0);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_range(ring, 0, 100);
    lv_arc_set_value(ring, 0);
    lv_obj_set_style_arc_color(ring, COL_DARK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);

    // Title — "BLE Scanner", top-center, cyan
    lbl_title = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_title, COL_CYAN, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 18);
    lv_label_set_text(lbl_title, "BLE Scanner");

    // Status line — "Scanning... N devices"
    lbl_status = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_status, COL_CYAN_DIM, 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 40);
    lv_label_set_text(lbl_status, "Scanning...");

    // Separator line under header
    lv_obj_t* sep = lv_obj_create(scr);
    lv_obj_set_size(sep, 300, 1);
    lv_obj_align(sep, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_bg_color(sep, COL_CYAN_DIM, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_clear_flag(sep, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    // Device rows — 6 rows, 14pt font, 44px spacing starting at y=70
    for (int i = 0; i < VISIBLE_ROWS; i++) {
        int y = 70 + i * 44;

        lbl_rows[i] = lv_label_create(scr);
        lv_obj_set_style_text_font(lbl_rows[i], &lv_font_montserrat_14, 0);
        lv_obj_set_width(lbl_rows[i], 210);
        lv_label_set_long_mode(lbl_rows[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(lbl_rows[i], COL_GRAY, 0);
        lv_obj_set_pos(lbl_rows[i], 28, y);
        lv_obj_add_flag(lbl_rows[i], LV_OBJ_FLAG_HIDDEN);

        lbl_rssi[i] = lv_label_create(scr);
        lv_obj_set_style_text_font(lbl_rssi[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl_rssi[i], COL_GRAY, 0);
        lv_obj_set_pos(lbl_rssi[i], 248, y);
        lv_obj_add_flag(lbl_rssi[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Scroll hint at bottom
    lv_obj_t* lbl_hint = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_hint, COL_CYAN_DIM, 0);
    lv_label_set_text(lbl_hint, "hold = detail");
    lv_obj_align(lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_clear_flag(lbl_hint, LV_OBJ_FLAG_CLICKABLE);

    list_view_built = true;
    Serial.println("[scr_ble] list view built");
}

// ============================================================================
// Build detail view (creates detail LVGL objects on scr_root)
// ============================================================================

static void build_detail_view() {
    lv_obj_t* scr = scr_root;

    // Device name — large, cyan, top-center
    lbl_det_name = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_det_name, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_det_name, COL_CYAN, 0);
    lv_obj_set_width(lbl_det_name, 280);
    lv_label_set_long_mode(lbl_det_name, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(lbl_det_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_det_name, LV_ALIGN_TOP_MID, 0, 30);

    // Separator
    lv_obj_t* sep = lv_obj_create(scr);
    lv_obj_set_size(sep, 240, 1);
    lv_obj_align(sep, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_bg_color(sep, COL_CYAN_DIM, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_clear_flag(sep, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    // Info labels — 14pt, centered
    int y = 74;
    const int step = 28;

    lbl_det_mac = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_det_mac, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_mac, COL_GRAY, 0);
    lv_obj_align(lbl_det_mac, LV_ALIGN_TOP_MID, 0, y);

    lbl_det_addr = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_det_addr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_addr, COL_GRAY, 0);
    lv_obj_align(lbl_det_addr, LV_ALIGN_TOP_MID, 0, y + step);

    lbl_det_rssi = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_det_rssi, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_det_rssi, LV_ALIGN_TOP_MID, 0, y + step * 2);

    lbl_det_type = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_det_type, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_type, COL_GRAY, 0);
    lv_obj_align(lbl_det_type, LV_ALIGN_TOP_MID, 0, y + step * 3);

    lbl_det_mfr = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_det_mfr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_mfr, COL_GRAY, 0);
    lv_obj_align(lbl_det_mfr, LV_ALIGN_TOP_MID, 0, y + step * 4);

    lbl_det_svc = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_det_svc, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_svc, COL_GRAY, 0);
    lv_obj_set_width(lbl_det_svc, 300);
    lv_label_set_long_mode(lbl_det_svc, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl_det_svc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_det_svc, LV_ALIGN_TOP_MID, 0, y + step * 5);

    lbl_det_tx = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_det_tx, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_tx, COL_GRAY, 0);
    lv_obj_align(lbl_det_tx, LV_ALIGN_TOP_MID, 0, y + step * 6);

    // Hint
    lbl_det_hint = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_det_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_hint, COL_CYAN_DIM, 0);
    lv_label_set_text(lbl_det_hint, "[ tap = back ]");
    lv_obj_align(lbl_det_hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    detail_built = true;
    Serial.println("[scr_ble] detail view built");
}

// ============================================================================
// Visibility helpers
// ============================================================================

static void hide_list() {
    if (!list_view_built) return;
    lv_obj_add_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < VISIBLE_ROWS; i++) {
        lv_obj_add_flag(lbl_rows[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_rssi[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_list() {
    if (!list_view_built) return;
    lv_obj_clear_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
    // Row visibility is managed by render_list()
}

static void hide_detail() {
    if (!detail_built) return;
    lv_obj_add_flag(lbl_det_name, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_mac,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_type, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_rssi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_addr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_mfr,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_svc,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_tx,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_hint, LV_OBJ_FLAG_HIDDEN);
}

static void show_detail() {
    if (!detail_built) return;
    lv_obj_clear_flag(lbl_det_name, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_mac,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_type, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_rssi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_addr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_mfr,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_svc,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_tx,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_hint, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================================
// Render: list view
// ============================================================================

static void render_list(BleScannerState* s) {
    show_list();
    hide_detail();

    // Status line
    if (s->scanning) {
        lv_label_set_text_fmt(lbl_status, "Scanning... %u device%s",
                              s->device_count, s->device_count == 1 ? "" : "s");
    } else {
        lv_label_set_text_fmt(lbl_status, "Paused — %u device%s",
                              s->device_count, s->device_count == 1 ? "" : "s");
    }

    if (s->device_count == 0) {
        for (int i = 0; i < VISIBLE_ROWS; i++) {
            lv_obj_add_flag(lbl_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_rssi[i], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    uint8_t visible = (s->device_count - s->scroll_offset < VISIBLE_ROWS)
                    ? (s->device_count - s->scroll_offset)
                    : VISIBLE_ROWS;

    for (int i = 0; i < visible; i++) {
        uint8_t dev_idx = s->scroll_offset + i;
        const BleDevice* dev = &s->devices[dev_idx];
        bool selected = (dev_idx == s->selected_index);

        // Build device name string: prefix + name or [Unknown] + truncated MAC
        char name_buf[40];
        const char* prefix = type_prefix(dev->device_type);
        if (dev->name[0] != '\0') {
            snprintf(name_buf, sizeof(name_buf), "%s %s", prefix, dev->name);
        } else {
            // No name: show [Unknown] + truncated MAC (last 3 bytes)
            snprintf(name_buf, sizeof(name_buf), "%s [Unknown] %02X:%02X:%02X",
                     prefix,
                     dev->mac[3], dev->mac[4], dev->mac[5]);
        }

        // Color: selected = cyan, stale = dark/dim, else RSSI color
        if (selected) {
            lv_label_set_text_fmt(lbl_rows[i], "> %s", name_buf);
            lv_obj_set_style_text_color(lbl_rows[i], COL_CYAN, 0);
        } else if (dev->stale) {
            lv_label_set_text(lbl_rows[i], name_buf);
            lv_obj_set_style_text_color(lbl_rows[i], COL_DARK, 0);
        } else {
            lv_label_set_text(lbl_rows[i], name_buf);
            lv_obj_set_style_text_color(lbl_rows[i], COL_GRAY, 0);
        }

        // RSSI bar + dBm — stale devices use dim color
        if (dev->stale) {
            lv_label_set_text_fmt(lbl_rssi[i], "%s %d", rssi_bars(dev->rssi_avg), dev->rssi_avg);
            lv_obj_set_style_text_color(lbl_rssi[i], COL_DARK, 0);
        } else {
            lv_label_set_text_fmt(lbl_rssi[i], "%s %d", rssi_bars(dev->rssi_avg), dev->rssi_avg);
            lv_obj_set_style_text_color(lbl_rssi[i], rssi_color(dev->rssi_avg), 0);
        }

        lv_obj_clear_flag(lbl_rows[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_rssi[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Hide unused rows
    for (int i = visible; i < VISIBLE_ROWS; i++) {
        lv_obj_add_flag(lbl_rows[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_rssi[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// ============================================================================
// Render: detail view
// ============================================================================

static void render_detail(BleScannerState* s) {
    hide_list();
    show_detail();

    if (s->selected_index >= s->device_count) return;
    const BleDevice* dev = &s->devices[s->selected_index];

    // Device name (large)
    if (dev->name[0] != '\0') {
        lv_label_set_text(lbl_det_name, dev->name);
    } else {
        lv_label_set_text(lbl_det_name, "[Unknown]");
    }

    // MAC address
    lv_label_set_text_fmt(lbl_det_mac, "%02X:%02X:%02X:%02X:%02X:%02X",
                          dev->mac[0], dev->mac[1], dev->mac[2],
                          dev->mac[3], dev->mac[4], dev->mac[5]);

    // Address type
    lv_label_set_text_fmt(lbl_det_addr, "Addr  %s", addr_type_str(dev->addr_type));

    // RSSI with live color
    lv_obj_set_style_text_color(lbl_det_rssi, rssi_color(dev->rssi), 0);
    lv_label_set_text_fmt(lbl_det_rssi, "%d dBm  %s", dev->rssi, rssi_bars(dev->rssi));

    // Device type
    lv_label_set_text_fmt(lbl_det_type, "Type  %s", ble_device_type_str(dev->device_type));

    // Manufacturer — prefer guessed name, fall back to company_id
    if (dev->mfr_name[0]) {
        lv_label_set_text_fmt(lbl_det_mfr, "Mfr   %s", dev->mfr_name);
    } else if (dev->company_id != 0) {
        const char* mfr = ble_manufacturer_lookup(dev->company_id);
        if (mfr) {
            lv_label_set_text_fmt(lbl_det_mfr, "Ad    %s", mfr);  // "Ad" = advertisement company
        } else {
            lv_label_set_text_fmt(lbl_det_mfr, "Ad    0x%04X", dev->company_id);
        }
    } else {
        lv_label_set_text(lbl_det_mfr, "Mfr   --");
    }

    // Service UUIDs (up to 8, as hex)
    if (dev->service_count > 0) {
        char svc_buf[80];
        int pos = 0;
        uint8_t n = dev->service_count < 8 ? dev->service_count : 8;
        for (uint8_t j = 0; j < n; j++) {
            if (j > 0) pos += snprintf(svc_buf + pos, sizeof(svc_buf) - pos, " ");
            pos += snprintf(svc_buf + pos, sizeof(svc_buf) - pos, "%04X", dev->service_uuids[j]);
        }
        lv_label_set_text_fmt(lbl_det_svc, "SVC  %s", svc_buf);
    } else {
        lv_label_set_text(lbl_det_svc, "SVC  --");
    }

    // TX power (INT8_MIN = not available, set by ble_scanner when haveTXPower() is false)
    if (dev->tx_power != INT8_MIN) {
        lv_label_set_text_fmt(lbl_det_tx, "TX   %d dBm", dev->tx_power);
    } else {
        lv_label_set_text(lbl_det_tx, "TX   --");
    }
}

// ============================================================================
// Lifecycle callbacks
// ============================================================================

static void create() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
    build_list_view();
}

static void show() {
    ble_scanner_start();
    lv_screen_load(scr_root);
    dirty = true;
}

static void hide() {
    ble_scanner_stop();
}

static void update() {
    ble_scanner_update();

    // Snapshot state under mutex for consistent rendering
    static BleScannerState snapshot;
    BleScannerState* real = ble_scanner_get_state();

    if (!ble_scanner_lock(10)) return;  // Skip frame if mutex busy
    memcpy(&snapshot, real, sizeof(BleScannerState));
    ble_scanner_unlock();

    BleScannerState* s = &snapshot;

    // Exit detail view if device list became empty
    if (s->detail_view && s->device_count == 0) {
        s->detail_view = false;
        real->detail_view = false;
        hide_detail();
        show_list();
        dirty = true;
    }

    // 2Hz refresh throttle for display updates
    uint32_t now = millis();
    if (!dirty && (now - last_update < 500)) return;
    last_update = now;

    if (s->detail_view) {
        render_detail(s);
    } else {
        render_list(s);
    }

    lv_refr_now(display_get_disp());
    dirty = false;
}

// ============================================================================
// Input handlers
// ============================================================================

void scr_ble_scan_on_encoder(int8_t delta) {
    BleScannerState* s = ble_scanner_get_state();
    if (s->detail_view) return;
    if (s->device_count == 0) return;

    // Move selected_index, clamped to [0, device_count-1]
    int new_idx = (int)s->selected_index + delta;
    if (new_idx < 0) new_idx = 0;
    if (new_idx >= (int)s->device_count) new_idx = s->device_count - 1;
    s->selected_index = (uint8_t)new_idx;

    // Adjust scroll_offset so selected_index stays in visible window
    if (s->selected_index < s->scroll_offset) {
        s->scroll_offset = s->selected_index;
    } else if (s->selected_index >= s->scroll_offset + VISIBLE_ROWS) {
        s->scroll_offset = s->selected_index - VISIBLE_ROWS + 1;
    }

    haptic_click();
    dirty = true;
}

void scr_ble_scan_on_tap() {
    BleScannerState* s = ble_scanner_get_state();
    if (s->detail_view) {
        // Return from detail to list
        s->detail_view = false;
        hide_detail();
        show_list();
        haptic_click();
        dirty = true;
    } else if (s->device_count > 0) {
        // Advance selection — wrap around
        s->selected_index = (s->selected_index + 1) % s->device_count;

        // Keep scroll window in sync
        if (s->selected_index < s->scroll_offset) {
            s->scroll_offset = s->selected_index;
        } else if (s->selected_index >= s->scroll_offset + VISIBLE_ROWS) {
            s->scroll_offset = s->selected_index - VISIBLE_ROWS + 1;
        }

        haptic_click();
        dirty = true;
    }
}

void scr_ble_scan_on_hold() {
    BleScannerState* s = ble_scanner_get_state();
    if (!s->detail_view && s->device_count > 0) {
        s->detail_view = true;
        haptic_double_click();
        // Build detail view objects lazily on first hold
        if (!detail_built) build_detail_view();
        dirty = true;
    }
}

// ============================================================================
// ScreenDef
// ============================================================================

const ScreenDef scr_ble_scan_def = {
    .name    = "BLE Scanner",
    .group   = GROUP_BLE,
    .id      = SCREEN_BLE_SCAN,
    .create  = create,
    .show    = show,
    .hide    = hide,
    .destroy = NULL,
    .update  = update,
    .enc_mode = ENC_BLE_LIST
};
