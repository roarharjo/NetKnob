# Probe Sniffer Rewrite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Phase 3 event-log probe sniffer with a device-centric UI — real-MAC device table, randomized-MAC aggregate, drill-down detail views, and pause-while-running.

**Architecture:** The ISR ring buffer and callback signature are preserved. A new aggregation layer in `wifi_probe_sniffer.cpp` maintains a device table (32 real-MAC entries) and a randomized-MAC aggregate, both with per-entry SSID lists. The screen is rewritten with three views (list, device detail, randomized detail). Capture persists across navigation; only shake stops it.

**Tech Stack:** ESP32-S3, Arduino framework, LVGL 8.x, PlatformIO

**Spec:** `docs/PROBE-SNIFFER-REWRITE-FSD.md`

**Note:** This is an ESP32 firmware project with no host-side test runner. "Verify" steps mean compile (`pio run`) and flash (`pio run -t upload`). All git operations are handled manually by the user.

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `src/wifi_probe_sniffer.h` | Rewrite | New data structures: DeviceEntry, DeviceSSID, RandomizedAggregate, new ProbeSnifferState |
| `src/wifi_probe_sniffer.cpp` | Rewrite | Device aggregation engine, SSID tracking, eviction, session lifecycle |
| `src/screens/scr_probe_sniff.cpp` | Rewrite | List view, device detail view, randomized detail view, pause |
| `src/screens/scr_probe_sniff.h` | No change | API surface unchanged (on_encoder, on_tap, on_hold) |
| `src/navigation.cpp` | Modify (1 line + 1 include) | Add `probe_sniffer_stop()` to emergency stop |
| `src/navigation.h` | No change | — |

---

## Task 1: Rewrite Data Structures (`wifi_probe_sniffer.h`)

**Files:**
- Rewrite: `src/wifi_probe_sniffer.h`

- [ ] **Step 1: Replace the header file**

Write the complete new header. `ProbeRequest` struct is unchanged. `ProbeSnifferState` is replaced. New types: `DeviceSSID`, `DeviceEntry`, `RandomizedAggregate`.

```cpp
#pragma once

#include <stdint.h>

// Tunable constants (FSD Appendix A)
#define MAX_DEVICES            32
#define MAX_SSIDS_PER_DEV      16
#define RAW_BUFFER_SIZE        32
#define RANDOMIZED_MAC_WINDOW   8
#define PROBE_MAC_RANDOMIZED_BIT 0x02

// Unchanged from Phase 3
struct ProbeRequest {
    uint8_t  src_mac[6];
    char     ssid_probed[33];
    int8_t   rssi;
    uint32_t timestamp_ms;
    uint8_t  channel;
    bool     mac_randomized;
};

struct DeviceSSID {
    char     ssid[33];
    uint16_t probe_count;
    uint32_t last_seen_ms;
};

struct DeviceEntry {
    uint8_t      mac[6];
    char         vendor[20];
    int8_t       rssi_last;
    int8_t       rssi_min;
    int8_t       rssi_max;
    uint16_t     probe_count;
    uint8_t      ssid_count;
    DeviceSSID   ssids[MAX_SSIDS_PER_DEV];
    uint32_t     first_seen_ms;
    uint32_t     last_seen_ms;
    uint8_t      last_channel;
};

struct RandomizedAggregate {
    uint16_t    unique_macs;
    uint16_t    total_probes;
    uint8_t     ssid_count;
    DeviceSSID  ssids[MAX_SSIDS_PER_DEV];
    int8_t      rssi_strongest;
    uint32_t    last_seen_ms;
    uint8_t     recent_macs[RANDOMIZED_MAC_WINDOW][6];
    uint8_t     recent_macs_count;
};

struct ProbeSnifferState {
    DeviceEntry          devices[MAX_DEVICES];
    uint8_t              device_count;
    RandomizedAggregate  randomized;
    ProbeRequest         raw[RAW_BUFFER_SIZE];
    uint16_t             raw_write_index;
    uint32_t             total_probes;
    uint32_t             session_start_ms;
    bool                 running;
    bool                 paused;
};

void probe_sniffer_init();
void probe_sniffer_start();
void probe_sniffer_stop();
void probe_sniffer_update();
ProbeSnifferState* probe_sniffer_get_state();
void probe_sniffer_on_frame(const uint8_t* frame, uint16_t len, int8_t rssi, uint8_t channel);
```

---

## Task 2: Rewrite Backend Engine (`wifi_probe_sniffer.cpp`)

**Files:**
- Rewrite: `src/wifi_probe_sniffer.cpp`

**Context:** The ISR ring buffer (`probe_ring[]`, `probe_ring_head`, `probe_ring_tail`, `probe_ring_mux`) and the `IRAM_ATTR probe_sniffer_on_frame()` function are preserved exactly as-is. The `parse_probe_request()` function body is replaced to route frames through the new aggregation layer. `recount_uniques()` is deleted.

- [ ] **Step 1: Write the complete new backend**

```cpp
#include "wifi_probe_sniffer.h"
#include "wifi_scanner.h"       // oui_lookup()
#include <Arduino.h>
#include <string.h>

// ---------------------------------------------------------------------------
// ISR ring buffer — unchanged from Phase 3
// ---------------------------------------------------------------------------
#define PROBE_RING_SLOTS    8
#define PROBE_RING_BUF_SIZE 128

struct ProbeRingSlot {
    uint8_t  data[PROBE_RING_BUF_SIZE];
    uint16_t len;
    int8_t   rssi;
    uint8_t  channel;
};

static ProbeRingSlot    probe_ring[PROBE_RING_SLOTS];
static volatile uint8_t probe_ring_head = 0;
static volatile uint8_t probe_ring_tail = 0;
static portMUX_TYPE     probe_ring_mux  = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static ProbeSnifferState state;

// ---------------------------------------------------------------------------
// ssid_list_update — add/increment SSID in a DeviceSSID array
// Shared by device_update and randomized_aggregate_update (DRY)
// ---------------------------------------------------------------------------
static void ssid_list_update(DeviceSSID ssids[], uint8_t* count, const char* raw_ssid, uint32_t now) {
    const char* ssid = (raw_ssid[0] == '\0') ? "(broadcast)" : raw_ssid;

    // Find existing
    for (uint8_t i = 0; i < *count; i++) {
        if (strcmp(ssids[i].ssid, ssid) == 0) {
            ssids[i].probe_count++;
            ssids[i].last_seen_ms = now;
            return;
        }
    }

    // Add new slot
    DeviceSSID* slot;
    if (*count < MAX_SSIDS_PER_DEV) {
        slot = &ssids[(*count)++];
    } else {
        // Evict oldest SSID
        uint8_t oldest = 0;
        for (uint8_t i = 1; i < *count; i++) {
            if (ssids[i].last_seen_ms < ssids[oldest].last_seen_ms)
                oldest = i;
        }
        slot = &ssids[oldest];
    }

    strncpy(slot->ssid, ssid, 32);
    slot->ssid[32] = '\0';
    slot->probe_count = 1;
    slot->last_seen_ms = now;
}

// ---------------------------------------------------------------------------
// find_or_create_device — lookup by MAC, create or LRU-evict if needed
// ---------------------------------------------------------------------------
static DeviceEntry* find_or_create_device(const uint8_t mac[6]) {
    // Search existing
    for (uint8_t i = 0; i < state.device_count; i++) {
        if (memcmp(state.devices[i].mac, mac, 6) == 0)
            return &state.devices[i];
    }

    // Pick a slot: append or evict
    DeviceEntry* dev;
    if (state.device_count < MAX_DEVICES) {
        dev = &state.devices[state.device_count++];
    } else {
        // Evict device with oldest last_seen_ms
        uint8_t oldest = 0;
        for (uint8_t i = 1; i < state.device_count; i++) {
            if (state.devices[i].last_seen_ms < state.devices[oldest].last_seen_ms)
                oldest = i;
        }
        dev = &state.devices[oldest];
    }

    memset(dev, 0, sizeof(DeviceEntry));
    memcpy(dev->mac, mac, 6);
    const char* vendor = oui_lookup(mac);
    strncpy(dev->vendor, vendor, sizeof(dev->vendor) - 1);
    dev->vendor[sizeof(dev->vendor) - 1] = '\0';
    dev->rssi_min = 0;   // sentinel: first probe will overwrite
    dev->rssi_max = -127;
    return dev;
}

// ---------------------------------------------------------------------------
// device_update — update a DeviceEntry with new probe data
// ---------------------------------------------------------------------------
static void device_update(DeviceEntry* dev, const char* ssid, int8_t rssi,
                          uint8_t channel, uint32_t now) {
    dev->probe_count++;
    dev->rssi_last = rssi;
    if (dev->rssi_min == 0 || rssi < dev->rssi_min) dev->rssi_min = rssi;
    if (rssi > dev->rssi_max) dev->rssi_max = rssi;
    if (dev->first_seen_ms == 0) dev->first_seen_ms = now;
    dev->last_seen_ms = now;
    dev->last_channel = channel;

    ssid_list_update(dev->ssids, &dev->ssid_count, ssid, now);
}

// ---------------------------------------------------------------------------
// randomized_aggregate_update — collapse randomized MACs into aggregate
// ---------------------------------------------------------------------------
static void randomized_aggregate_update(const uint8_t mac[6], const char* ssid,
                                        int8_t rssi, uint8_t channel, uint32_t now) {
    RandomizedAggregate& ra = state.randomized;
    ra.total_probes++;
    if (rssi > ra.rssi_strongest || ra.total_probes == 1) ra.rssi_strongest = rssi;
    ra.last_seen_ms = now;

    // Check sliding MAC window
    bool in_window = false;
    for (uint8_t i = 0; i < ra.recent_macs_count; i++) {
        if (memcmp(ra.recent_macs[i], mac, 6) == 0) {
            in_window = true;
            break;
        }
    }

    if (!in_window) {
        ra.unique_macs++;
        if (ra.recent_macs_count < RANDOMIZED_MAC_WINDOW) {
            memcpy(ra.recent_macs[ra.recent_macs_count++], mac, 6);
        } else {
            // FIFO shift: drop oldest, append new
            memmove(ra.recent_macs[0], ra.recent_macs[1],
                    (RANDOMIZED_MAC_WINDOW - 1) * 6);
            memcpy(ra.recent_macs[RANDOMIZED_MAC_WINDOW - 1], mac, 6);
        }
    }

    ssid_list_update(ra.ssids, &ra.ssid_count, ssid, now);
}

// ---------------------------------------------------------------------------
// parse_probe_request — called from main loop, routes to aggregation
// ---------------------------------------------------------------------------
static void parse_probe_request(const uint8_t* frame, uint16_t len,
                                int8_t rssi, uint8_t channel) {
    if (len < 24) return;

    // Source MAC at bytes 10-15
    uint8_t src_mac[6];
    memcpy(src_mac, &frame[10], 6);
    bool randomized = (src_mac[0] & PROBE_MAC_RANDOMIZED_BIT) != 0;

    // Parse SSID from tagged IEs (tag 0, always first in probe requests)
    char ssid[33] = {};
    uint16_t pos = 24;
    while (pos + 2 <= len) {
        uint8_t tag_id  = frame[pos];
        uint8_t tag_len = frame[pos + 1];
        if (pos + 2 + tag_len > len) break;
        if (tag_id == 0) {
            uint8_t copy_len = (tag_len > 32) ? 32 : tag_len;
            memcpy(ssid, &frame[pos + 2], copy_len);
            ssid[copy_len] = '\0';
            break;
        }
        pos += 2 + tag_len;
    }

    uint32_t now = millis();

    // Write to raw ring (debug/tail use only)
    ProbeRequest& raw = state.raw[state.raw_write_index % RAW_BUFFER_SIZE];
    memcpy(raw.src_mac, src_mac, 6);
    strncpy(raw.ssid_probed, ssid, 32);
    raw.ssid_probed[32] = '\0';
    raw.rssi           = rssi;
    raw.channel        = channel;
    raw.timestamp_ms   = now;
    raw.mac_randomized = randomized;
    state.raw_write_index++;

    state.total_probes++;

    // Route to aggregation
    if (randomized) {
        randomized_aggregate_update(src_mac, ssid, rssi, channel, now);
    } else {
        DeviceEntry* dev = find_or_create_device(src_mac);
        device_update(dev, ssid, rssi, channel, now);
    }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void probe_sniffer_init() {
    memset(&state, 0, sizeof(state));
}

void probe_sniffer_start() {
    if (state.running) return;  // Already running — keep accumulated data
    memset(&state, 0, sizeof(state));
    state.running          = true;
    state.session_start_ms = millis();
    // Flush ISR ring
    portENTER_CRITICAL(&probe_ring_mux);
    probe_ring_head = 0;
    probe_ring_tail = 0;
    portEXIT_CRITICAL(&probe_ring_mux);
}

void probe_sniffer_stop() {
    state.running = false;
    // State persists in memory; next start() will memset
}

void probe_sniffer_update() {
    // Drain ISR ring buffer into aggregation layer
    while (true) {
        portENTER_CRITICAL(&probe_ring_mux);
        bool has_data = (probe_ring_tail != probe_ring_head);
        ProbeRingSlot slot;
        if (has_data) {
            slot = probe_ring[probe_ring_tail];
            probe_ring_tail = (probe_ring_tail + 1) % PROBE_RING_SLOTS;
        }
        portEXIT_CRITICAL(&probe_ring_mux);

        if (!has_data) break;
        parse_probe_request(slot.data, slot.len, slot.rssi, slot.channel);
    }
}

ProbeSnifferState* probe_sniffer_get_state() {
    return &state;
}

void IRAM_ATTR probe_sniffer_on_frame(const uint8_t* frame, uint16_t len,
                                       int8_t rssi, uint8_t channel) {
    if (!state.running) return;

    portENTER_CRITICAL(&probe_ring_mux);
    uint8_t next_head = (probe_ring_head + 1) % PROBE_RING_SLOTS;
    if (next_head != probe_ring_tail) {
        uint16_t copy_len = (len > PROBE_RING_BUF_SIZE) ? PROBE_RING_BUF_SIZE : len;
        memcpy(probe_ring[probe_ring_head].data, frame, copy_len);
        probe_ring[probe_ring_head].len     = copy_len;
        probe_ring[probe_ring_head].rssi    = rssi;
        probe_ring[probe_ring_head].channel = channel;
        probe_ring_head = next_head;
    }
    portEXIT_CRITICAL(&probe_ring_mux);
}
```

- [ ] **Step 2: Compile check**

Run: `pio run`

This will fail because `scr_probe_sniff.cpp` still references the old `ProbeSnifferState`. That's expected — proceed to Task 3.

---

## Task 3: Rewrite Screen (`scr_probe_sniff.cpp`)

**Files:**
- Rewrite: `src/screens/scr_probe_sniff.cpp`

**Context:** The header (`scr_probe_sniff.h`) is unchanged — same exported functions. The screen has three views: list (primary), device detail, and randomized detail. The encoder moves the cursor (not channel hop). Touch tap opens detail or goes back. Touch hold toggles pause.

Layout coordinates are for the 360x360 circular display. The usable text area is roughly 300px wide, y=30 to y=325.

- [ ] **Step 1: Write the complete new screen file**

```cpp
/* scr_probe_sniff.cpp — Probe Sniffer screen (device-centric rewrite)
 *
 * Three views:
 *   LIST              — device rows sorted by last_seen, sticky randomized row
 *   DEVICE_DETAIL     — identity + scrollable SSID list for one real-MAC device
 *   RANDOMIZED_DETAIL — aggregate stats + SSID list + recent MACs
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
// Constants
// ---------------------------------------------------------------------------
#define VISIBLE_ROWS     3      // Real-device rows visible at once
#define PAUSE_HOLD_MS    1000   // Touch-hold threshold (FR-14)
#define MAX_DETAIL_SSIDS 5      // SSID rows visible in detail view
#define COL_AMBER        lv_color_make(0xFF, 0xBF, 0x00)

// ---------------------------------------------------------------------------
// View state
// ---------------------------------------------------------------------------
enum ProbeView { VIEW_LIST, VIEW_DEVICE_DETAIL, VIEW_RANDOMIZED_DETAIL };

static lv_obj_t*  scr_root     = NULL;
static ProbeView  current_view = VIEW_LIST;

// List view cursor — tracks by MAC, not by index
static uint8_t  cursor_mac[6]   = {};
static uint8_t  cursor_idx      = 0;   // Index in sorted order (sorted_count = rnd row)
static uint8_t  scroll_offset   = 0;   // First visible device row index
static bool     cursor_on_rnd   = false;

// Sort index
static uint8_t  sorted_idx[MAX_DEVICES];
static uint8_t  sorted_count = 0;

// Detail view scroll
static int8_t   ssid_scroll = 0;

// UI throttle
static uint32_t last_ui_update = 0;

// Snapshot for pause: we freeze the display by skipping update_list_display
// while state.paused is true. Capture keeps running underneath (FR-05).

// ---------------------------------------------------------------------------
// LVGL objects — List view
// ---------------------------------------------------------------------------
static lv_obj_t* lbl_status1    = NULL;   // "HOPPING · 0:42"
static lv_obj_t* lbl_status2    = NULL;   // "3 devs · 105p · 14 SSIDs"
static lv_obj_t* lbl_dev_l1[VISIBLE_ROWS] = {};   // Device line 1: MAC + vendor
static lv_obj_t* lbl_dev_l2[VISIBLE_ROWS] = {};   // Device line 2: stats
static lv_obj_t* lbl_rnd        = NULL;   // "[rnd] 23 MACs · 47p · 8 SSIDs"
static lv_obj_t* lbl_list_hint  = NULL;   // "tap=open  hold=pause"
static lv_obj_t* lbl_empty      = NULL;   // "Listening..."

static bool list_built = false;

// LVGL objects — Device detail
static lv_obj_t* lbl_det_mac    = NULL;
static lv_obj_t* lbl_det_info   = NULL;   // Multiline: vendor/count/rssi, range, timing
static lv_obj_t* lbl_det_ssids  = NULL;   // Multiline: "Probing for:\n  SSID (Nx)\n..."
static lv_obj_t* lbl_det_hint   = NULL;

static bool detail_built = false;

// LVGL objects — Randomized detail
static lv_obj_t* lbl_rdet_info  = NULL;   // Multiline: MAC count, probes, rssi, last seen
static lv_obj_t* lbl_rdet_ssids = NULL;   // Multiline SSID list
static lv_obj_t* lbl_rdet_macs  = NULL;   // "Last 8 MACs:\nDA:23:1F:* ..."
static lv_obj_t* lbl_rdet_hint  = NULL;

static bool rdet_built = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void format_elapsed(uint32_t elapsed_ms, char* buf, size_t len) {
    uint32_t s = elapsed_ms / 1000;
    if (s < 3600) {
        snprintf(buf, len, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    } else {
        snprintf(buf, len, "%02lu:%02lu", (unsigned long)(s / 3600),
                 (unsigned long)((s % 3600) / 60));
    }
}

static void sort_devices() {
    ProbeSnifferState* s = probe_sniffer_get_state();
    sorted_count = s->device_count;
    for (uint8_t i = 0; i < sorted_count; i++) sorted_idx[i] = i;

    // Insertion sort by last_seen_ms descending (max 32 items)
    for (uint8_t i = 1; i < sorted_count; i++) {
        uint8_t key = sorted_idx[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 &&
               s->devices[sorted_idx[j]].last_seen_ms < s->devices[key].last_seen_ms) {
            sorted_idx[j + 1] = sorted_idx[j];
            j--;
        }
        sorted_idx[j + 1] = key;
    }
}

// Re-find cursor's MAC after sort; jump to top if evicted (FR-10)
static void anchor_cursor() {
    if (cursor_on_rnd) {
        cursor_idx = sorted_count;
        return;
    }
    ProbeSnifferState* s = probe_sniffer_get_state();
    for (uint8_t i = 0; i < sorted_count; i++) {
        if (memcmp(s->devices[sorted_idx[i]].mac, cursor_mac, 6) == 0) {
            cursor_idx = i;
            return;
        }
    }
    // MAC evicted — jump to top
    cursor_idx = 0;
    cursor_on_rnd = false;
    if (sorted_count > 0)
        memcpy(cursor_mac, s->devices[sorted_idx[0]].mac, 6);
}

static uint16_t count_total_unique_ssids() {
    ProbeSnifferState* s = probe_sniffer_get_state();
    // Deduplicate SSIDs across all devices + randomized.
    // Use a static buffer to avoid stack pressure.
    static char seen[64][33];
    uint16_t n = 0;

    auto check = [&](const char* ssid) {
        for (uint16_t i = 0; i < n; i++) {
            if (strcmp(seen[i], ssid) == 0) return;
        }
        if (n < 64) {
            strncpy(seen[n], ssid, 32);
            seen[n][32] = '\0';
            n++;
        }
    };

    for (uint8_t d = 0; d < s->device_count; d++)
        for (uint8_t i = 0; i < s->devices[d].ssid_count; i++)
            check(s->devices[d].ssids[i].ssid);
    for (uint8_t i = 0; i < s->randomized.ssid_count; i++)
        check(s->randomized.ssids[i].ssid);

    return n;
}

// ---------------------------------------------------------------------------
// Build list view (lazy, once)
// ---------------------------------------------------------------------------
static void build_list_view() {
    if (list_built) return;

    lbl_status1 = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_status1, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_status1, COL_CYAN, 0);
    lv_obj_align(lbl_status1, LV_ALIGN_TOP_MID, 0, 30);

    lbl_status2 = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_status2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_status2, COL_GRAY, 0);
    lv_obj_align(lbl_status2, LV_ALIGN_TOP_MID, 0, 50);

    for (int i = 0; i < VISIBLE_ROWS; i++) {
        int y = 74 + i * 44;
        lbl_dev_l1[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_font(lbl_dev_l1[i], &lv_font_montserrat_14, 0);
        lv_obj_set_pos(lbl_dev_l1[i], 20, y);
        lv_obj_set_width(lbl_dev_l1[i], 300);
        lv_label_set_long_mode(lbl_dev_l1[i], LV_LABEL_LONG_CLIP);

        lbl_dev_l2[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_font(lbl_dev_l2[i], &lv_font_montserrat_12, 0);
        lv_obj_set_pos(lbl_dev_l2[i], 20, y + 20);
        lv_obj_set_width(lbl_dev_l2[i], 300);
        lv_label_set_long_mode(lbl_dev_l2[i], LV_LABEL_LONG_CLIP);
    }

    lbl_rnd = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_rnd, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_rnd, COL_GRAY, 0);
    lv_obj_set_pos(lbl_rnd, 20, 210);
    lv_obj_set_width(lbl_rnd, 300);
    lv_label_set_long_mode(lbl_rnd, LV_LABEL_LONG_CLIP);

    lbl_list_hint = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_list_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_list_hint, COL_GRAY, 0);
    lv_obj_align(lbl_list_hint, LV_ALIGN_BOTTOM_MID, 0, -35);

    lbl_empty = lv_label_create(scr_root);
    lv_label_set_text(lbl_empty, "Listening...");
    lv_obj_set_style_text_font(lbl_empty, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_empty, COL_GRAY, 0);
    lv_obj_align(lbl_empty, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);

    list_built = true;
}

// ---------------------------------------------------------------------------
// Build device detail view (lazy, once)
// ---------------------------------------------------------------------------
static void build_detail_view() {
    if (detail_built) return;

    lbl_det_mac = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_det_mac, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_det_mac, COL_CYAN, 0);
    lv_obj_align(lbl_det_mac, LV_ALIGN_TOP_MID, 0, 35);

    lbl_det_info = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_det_info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_det_info, COL_WHITE, 0);
    lv_obj_set_pos(lbl_det_info, 20, 60);
    lv_obj_set_width(lbl_det_info, 300);
    lv_label_set_long_mode(lbl_det_info, LV_LABEL_LONG_WRAP);

    lbl_det_ssids = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_det_ssids, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_ssids, COL_GREEN, 0);
    lv_obj_set_pos(lbl_det_ssids, 20, 135);
    lv_obj_set_width(lbl_det_ssids, 300);
    lv_label_set_long_mode(lbl_det_ssids, LV_LABEL_LONG_WRAP);

    lbl_det_hint = lv_label_create(scr_root);
    lv_label_set_text(lbl_det_hint, "tap = back");
    lv_obj_set_style_text_font(lbl_det_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_det_hint, COL_GRAY, 0);
    lv_obj_align(lbl_det_hint, LV_ALIGN_BOTTOM_MID, 0, -35);

    detail_built = true;
}

// ---------------------------------------------------------------------------
// Build randomized detail view (lazy, once)
// ---------------------------------------------------------------------------
static void build_rdet_view() {
    if (rdet_built) return;

    lbl_rdet_info = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_rdet_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_rdet_info, COL_WHITE, 0);
    lv_obj_set_pos(lbl_rdet_info, 20, 40);
    lv_obj_set_width(lbl_rdet_info, 300);
    lv_label_set_long_mode(lbl_rdet_info, LV_LABEL_LONG_WRAP);

    lbl_rdet_ssids = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_rdet_ssids, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_rdet_ssids, COL_GREEN, 0);
    lv_obj_set_pos(lbl_rdet_ssids, 20, 120);
    lv_obj_set_width(lbl_rdet_ssids, 300);
    lv_label_set_long_mode(lbl_rdet_ssids, LV_LABEL_LONG_WRAP);

    lbl_rdet_macs = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_rdet_macs, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_rdet_macs, COL_GRAY, 0);
    lv_obj_set_pos(lbl_rdet_macs, 20, 240);
    lv_obj_set_width(lbl_rdet_macs, 300);
    lv_label_set_long_mode(lbl_rdet_macs, LV_LABEL_LONG_WRAP);

    lbl_rdet_hint = lv_label_create(scr_root);
    lv_label_set_text(lbl_rdet_hint, "tap = back");
    lv_obj_set_style_text_font(lbl_rdet_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_rdet_hint, COL_GRAY, 0);
    lv_obj_align(lbl_rdet_hint, LV_ALIGN_BOTTOM_MID, 0, -35);

    rdet_built = true;
}

// ---------------------------------------------------------------------------
// Hide helpers
// ---------------------------------------------------------------------------
static void hide_list_objects() {
    if (!list_built) return;
    lv_obj_add_flag(lbl_status1,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_status2,   LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < VISIBLE_ROWS; i++) {
        lv_obj_add_flag(lbl_dev_l1[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_dev_l2[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(lbl_rnd,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_list_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_empty,     LV_OBJ_FLAG_HIDDEN);
}

static void show_list_objects() {
    if (!list_built) return;
    lv_obj_clear_flag(lbl_status1,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_status2,   LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < VISIBLE_ROWS; i++) {
        lv_obj_clear_flag(lbl_dev_l1[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_dev_l2[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(lbl_rnd,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_list_hint, LV_OBJ_FLAG_HIDDEN);
}

static void hide_detail_objects() {
    if (!detail_built) return;
    lv_obj_add_flag(lbl_det_mac,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_info,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_ssids, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_det_hint,  LV_OBJ_FLAG_HIDDEN);
}

static void show_detail_objects() {
    if (!detail_built) return;
    lv_obj_clear_flag(lbl_det_mac,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_info,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_ssids, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_hint,  LV_OBJ_FLAG_HIDDEN);
}

static void hide_rdet_objects() {
    if (!rdet_built) return;
    lv_obj_add_flag(lbl_rdet_info,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_rdet_ssids, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_rdet_macs,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_rdet_hint,  LV_OBJ_FLAG_HIDDEN);
}

static void show_rdet_objects() {
    if (!rdet_built) return;
    lv_obj_clear_flag(lbl_rdet_info,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_rdet_ssids, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_rdet_macs,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_rdet_hint,  LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// update_list_display — refresh all list view labels
// ---------------------------------------------------------------------------
static void update_list_display() {
    if (!list_built) return;

    ProbeSnifferState* s = probe_sniffer_get_state();

    sort_devices();
    anchor_cursor();

    // Auto-scroll viewport to keep cursor visible
    if (!cursor_on_rnd) {
        if (cursor_idx < scroll_offset) scroll_offset = cursor_idx;
        if (cursor_idx >= scroll_offset + VISIBLE_ROWS)
            scroll_offset = cursor_idx - VISIBLE_ROWS + 1;
    }

    // --- Empty state (FR-13) ---
    bool empty = (s->device_count == 0 && s->randomized.unique_macs == 0);
    if (empty) {
        for (int i = 0; i < VISIBLE_ROWS; i++) {
            lv_obj_add_flag(lbl_dev_l1[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_dev_l2[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(lbl_rnd, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    }

    // --- Status bar (FR-07) ---
    char elapsed[16];
    format_elapsed(millis() - s->session_start_ms, elapsed, sizeof(elapsed));

    if (s->paused) {
        lv_label_set_text_fmt(lbl_status1, "PAUSED \xc2\xb7 %s", elapsed);
        lv_obj_set_style_text_color(lbl_status1, COL_AMBER, 0);
    } else {
        lv_label_set_text_fmt(lbl_status1, "HOPPING \xc2\xb7 %s", elapsed);
        lv_obj_set_style_text_color(lbl_status1, COL_CYAN, 0);
    }

    uint16_t unique_ssids = count_total_unique_ssids();
    lv_label_set_text_fmt(lbl_status2, "%u devs \xc2\xb7 %lup \xc2\xb7 %u SSIDs",
                          s->device_count, (unsigned long)s->total_probes, unique_ssids);

    if (empty) {
        lv_label_set_text(lbl_list_hint, "");
        return;
    }

    // --- Device rows (FR-08) ---
    for (int i = 0; i < VISIBLE_ROWS; i++) {
        uint8_t dev_pos = scroll_offset + i;
        if (dev_pos >= sorted_count) {
            lv_label_set_text(lbl_dev_l1[i], "");
            lv_label_set_text(lbl_dev_l2[i], "");
            lv_obj_add_flag(lbl_dev_l1[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_dev_l2[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        lv_obj_clear_flag(lbl_dev_l1[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_dev_l2[i], LV_OBJ_FLAG_HIDDEN);

        const DeviceEntry& dev = s->devices[sorted_idx[dev_pos]];
        bool selected = (!cursor_on_rnd && cursor_idx == dev_pos);

        // Line 1: MAC + vendor
        char line1[48];
        if (dev.vendor[0] != '\0' && strcmp(dev.vendor, "Unknown") != 0) {
            snprintf(line1, sizeof(line1), "%s%02X:%02X:%02X:%02X:%02X:%02X  %s",
                     selected ? "\xe2\x96\xb6 " : "  ",
                     dev.mac[0], dev.mac[1], dev.mac[2],
                     dev.mac[3], dev.mac[4], dev.mac[5],
                     dev.vendor);
        } else {
            snprintf(line1, sizeof(line1), "%s%02X:%02X:%02X:%02X:%02X:%02X",
                     selected ? "\xe2\x96\xb6 " : "  ",
                     dev.mac[0], dev.mac[1], dev.mac[2],
                     dev.mac[3], dev.mac[4], dev.mac[5]);
        }
        lv_label_set_text(lbl_dev_l1[i], line1);

        // Line 2: probe count, SSID count, RSSI
        char line2[48];
        snprintf(line2, sizeof(line2), "  %up \xc2\xb7 %u %s \xc2\xb7 %d dBm",
                 dev.probe_count, dev.ssid_count,
                 (dev.ssid_count == 1) ? "SSID" : "SSIDs",
                 (int)dev.rssi_last);
        lv_label_set_text(lbl_dev_l2[i], line2);

        // Colors
        lv_color_t col = selected ? COL_CYAN : COL_WHITE;
        lv_obj_set_style_text_color(lbl_dev_l1[i], col, 0);
        lv_obj_set_style_text_color(lbl_dev_l2[i], selected ? COL_CYAN : COL_GRAY, 0);
    }

    // --- Randomized aggregate row (FR-12) ---
    const RandomizedAggregate& ra = s->randomized;
    if (ra.unique_macs > 0 || ra.total_probes > 0) {
        lv_obj_clear_flag(lbl_rnd, LV_OBJ_FLAG_HIDDEN);
        bool rnd_sel = cursor_on_rnd;
        char rnd_buf[64];
        snprintf(rnd_buf, sizeof(rnd_buf), "%s[rnd] %u MACs \xc2\xb7 %up \xc2\xb7 %u SSIDs",
                 rnd_sel ? "\xe2\x96\xb6 " : "  ",
                 ra.unique_macs, ra.total_probes, ra.ssid_count);
        lv_label_set_text(lbl_rnd, rnd_buf);
        lv_obj_set_style_text_color(lbl_rnd, rnd_sel ? COL_CYAN : COL_GRAY, 0);
    } else {
        lv_obj_add_flag(lbl_rnd, LV_OBJ_FLAG_HIDDEN);
    }

    // --- Footer hint ---
    lv_label_set_text(lbl_list_hint, s->paused ? "tap=open  hold=resume" : "tap=open  hold=pause");
}

// ---------------------------------------------------------------------------
// Sort SSIDs by probe_count descending for detail views (FR-17)
// ---------------------------------------------------------------------------
static void sort_ssid_indices(const DeviceSSID ssids[], uint8_t count,
                              uint8_t out_idx[], uint8_t* out_count) {
    *out_count = count;
    for (uint8_t i = 0; i < count; i++) out_idx[i] = i;

    for (uint8_t i = 1; i < count; i++) {
        uint8_t key = out_idx[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && ssids[out_idx[j]].probe_count < ssids[key].probe_count) {
            out_idx[j + 1] = out_idx[j];
            j--;
        }
        out_idx[j + 1] = key;
    }
}

// ---------------------------------------------------------------------------
// update_detail_display — device detail view (FR-15 .. FR-17)
// ---------------------------------------------------------------------------
static void update_detail_display() {
    if (!detail_built || cursor_on_rnd) return;

    ProbeSnifferState* s = probe_sniffer_get_state();
    if (cursor_idx >= sorted_count) return;

    const DeviceEntry& dev = s->devices[sorted_idx[cursor_idx]];

    // MAC header
    lv_label_set_text_fmt(lbl_det_mac, "%02X:%02X:%02X:%02X:%02X:%02X",
                          dev.mac[0], dev.mac[1], dev.mac[2],
                          dev.mac[3], dev.mac[4], dev.mac[5]);

    // Info block (FR-16): 4 lines
    char info[160];
    const char* v = (dev.vendor[0] != '\0' && strcmp(dev.vendor, "Unknown") != 0)
                    ? dev.vendor : "Real (no OUI)";

    char last_ago[16], first_ago[16];
    format_elapsed(millis() - dev.last_seen_ms, last_ago, sizeof(last_ago));
    format_elapsed(millis() - dev.first_seen_ms, first_ago, sizeof(first_ago));

    snprintf(info, sizeof(info),
             "%s \xc2\xb7 %up \xc2\xb7 %d dBm\n"
             "%d .. %d \xc2\xb7 seen %s\n"
             "first %s \xc2\xb7 CH %u last",
             v, dev.probe_count, (int)dev.rssi_last,
             (int)dev.rssi_min, (int)dev.rssi_max, last_ago,
             first_ago, dev.last_channel);
    lv_label_set_text(lbl_det_info, info);

    // SSID list (FR-17): sorted by probe_count desc, scrollable
    uint8_t ssid_order[MAX_SSIDS_PER_DEV];
    uint8_t ssid_total = 0;
    sort_ssid_indices(dev.ssids, dev.ssid_count, ssid_order, &ssid_total);

    // Clamp scroll
    int max_scroll = (int)ssid_total - MAX_DETAIL_SSIDS;
    if (max_scroll < 0) max_scroll = 0;
    if (ssid_scroll > max_scroll) ssid_scroll = max_scroll;
    if (ssid_scroll < 0) ssid_scroll = 0;

    char ssid_buf[320] = {};
    int pos = 0;
    pos += snprintf(ssid_buf + pos, sizeof(ssid_buf) - pos,
                    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                    "\nProbing for:\n");

    uint8_t visible = 0;
    for (uint8_t vi = ssid_scroll; vi < ssid_total && visible < MAX_DETAIL_SSIDS; vi++, visible++) {
        const DeviceSSID& ss = dev.ssids[ssid_order[vi]];
        // Truncate SSID name at ~17 chars
        char name[20];
        strncpy(name, ss.ssid, 17);
        name[17] = '\0';
        if (strlen(ss.ssid) > 17) {
            name[16] = '\0';
            strcat(name, "\xe2\x80\xa6");  // ellipsis
        }
        pos += snprintf(ssid_buf + pos, sizeof(ssid_buf) - pos,
                        "  %-18s (%u\xc3\x97)\n", name, ss.probe_count);
    }

    if (ssid_total > MAX_DETAIL_SSIDS && ssid_scroll < max_scroll) {
        pos += snprintf(ssid_buf + pos, sizeof(ssid_buf) - pos,
                        "            \xe2\x86\xbb more");
    }

    lv_label_set_text(lbl_det_ssids, ssid_buf);
}

// ---------------------------------------------------------------------------
// update_rdet_display — randomized aggregate detail view (FR-19 .. FR-20)
// ---------------------------------------------------------------------------
static void update_rdet_display() {
    if (!rdet_built) return;

    ProbeSnifferState* s = probe_sniffer_get_state();
    const RandomizedAggregate& ra = s->randomized;

    // Info block
    char last_ago[16];
    format_elapsed(millis() - ra.last_seen_ms, last_ago, sizeof(last_ago));

    char info[128];
    snprintf(info, sizeof(info),
             "RANDOMIZED DEVICES\n"
             "%u MACs \xc2\xb7 %u probes\n"
             "%d dBm strongest\n"
             "last seen %s ago",
             ra.unique_macs, ra.total_probes,
             (int)ra.rssi_strongest, last_ago);
    lv_label_set_text(lbl_rdet_info, info);

    // SSID list
    uint8_t ssid_order[MAX_SSIDS_PER_DEV];
    uint8_t ssid_total = 0;
    sort_ssid_indices(ra.ssids, ra.ssid_count, ssid_order, &ssid_total);

    int max_scroll = (int)ssid_total - MAX_DETAIL_SSIDS;
    if (max_scroll < 0) max_scroll = 0;
    if (ssid_scroll > max_scroll) ssid_scroll = max_scroll;
    if (ssid_scroll < 0) ssid_scroll = 0;

    char ssid_buf[320] = {};
    int pos = 0;
    pos += snprintf(ssid_buf + pos, sizeof(ssid_buf) - pos,
                    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                    "\nAll SSIDs probed:\n");

    uint8_t visible = 0;
    for (uint8_t vi = ssid_scroll; vi < ssid_total && visible < MAX_DETAIL_SSIDS; vi++, visible++) {
        const DeviceSSID& ss = ra.ssids[ssid_order[vi]];
        char name[20];
        strncpy(name, ss.ssid, 17);
        name[17] = '\0';
        if (strlen(ss.ssid) > 17) {
            name[16] = '\0';
            strcat(name, "\xe2\x80\xa6");
        }
        pos += snprintf(ssid_buf + pos, sizeof(ssid_buf) - pos,
                        "  %-18s (%u\xc3\x97)\n", name, ss.probe_count);
    }

    if (ssid_total > MAX_DETAIL_SSIDS && ssid_scroll < max_scroll) {
        pos += snprintf(ssid_buf + pos, sizeof(ssid_buf) - pos,
                        "            \xe2\x86\xbb more");
    }

    lv_label_set_text(lbl_rdet_ssids, ssid_buf);

    // Recent MACs block
    char mac_buf[128] = {};
    int mpos = 0;
    mpos += snprintf(mac_buf + mpos, sizeof(mac_buf) - mpos,
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                     "\nLast %u MACs:\n", ra.recent_macs_count);

    for (uint8_t i = 0; i < ra.recent_macs_count; i++) {
        mpos += snprintf(mac_buf + mpos, sizeof(mac_buf) - mpos,
                         "%02X:%02X:%02X:*%s",
                         ra.recent_macs[i][0], ra.recent_macs[i][1],
                         ra.recent_macs[i][2],
                         ((i + 1) % 2 == 0 || i == ra.recent_macs_count - 1) ? "\n" : " ");
    }

    lv_label_set_text(lbl_rdet_macs, mac_buf);
}

// ---------------------------------------------------------------------------
// show_view — switch between views
// ---------------------------------------------------------------------------
static void show_list_view() {
    build_list_view();
    hide_detail_objects();
    hide_rdet_objects();
    show_list_objects();
    current_view = VIEW_LIST;
    update_list_display();
}

static void show_device_detail() {
    build_detail_view();
    hide_list_objects();
    hide_rdet_objects();
    show_detail_objects();
    current_view = VIEW_DEVICE_DETAIL;
    ssid_scroll = 0;
    update_detail_display();
}

static void show_randomized_detail() {
    build_rdet_view();
    hide_list_objects();
    hide_detail_objects();
    show_rdet_objects();
    current_view = VIEW_RANDOMIZED_DETAIL;
    ssid_scroll = 0;
    update_rdet_display();
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
    probe_sniffer_start();     // No-op if already running (session persists)
    scroll_offset  = 0;
    cursor_idx     = 0;
    cursor_on_rnd  = false;
    memset(cursor_mac, 0, 6);

    // If there are devices, anchor cursor to the top one
    ProbeSnifferState* s = probe_sniffer_get_state();
    if (s->device_count > 0) {
        sort_devices();
        memcpy(cursor_mac, s->devices[sorted_idx[0]].mac, 6);
    }

    show_list_view();
    lv_screen_load(scr_root);
}

static void hide() {
    // Capture persists — do NOT call probe_sniffer_stop() (FR-04)
}

static void update_screen() {
    probe_sniffer_update();   // Drain ISR ring (always, even when paused)

    uint32_t now = millis();
    if (now - last_ui_update < 500) return;
    last_ui_update = now;

    ProbeSnifferState* s = probe_sniffer_get_state();

    switch (current_view) {
        case VIEW_LIST:
            if (!s->paused) {
                update_list_display();
                lv_refr_now(display_get_disp());
            } else {
                // Only update the elapsed timer while paused
                char elapsed[16];
                format_elapsed(now - s->session_start_ms, elapsed, sizeof(elapsed));
                lv_label_set_text_fmt(lbl_status1, "PAUSED \xc2\xb7 %s", elapsed);
                lv_refr_now(display_get_disp());
            }
            break;
        case VIEW_DEVICE_DETAIL:
            update_detail_display();
            lv_refr_now(display_get_disp());
            break;
        case VIEW_RANDOMIZED_DETAIL:
            update_rdet_display();
            lv_refr_now(display_get_disp());
            break;
    }
}

// ---------------------------------------------------------------------------
// Input handlers
// ---------------------------------------------------------------------------

void scr_probe_sniff_on_encoder(int8_t delta) {
    ProbeSnifferState* s = probe_sniffer_get_state();

    if (current_view == VIEW_LIST) {
        // Cursor movement (FR-14)
        bool has_rnd = (s->randomized.unique_macs > 0 || s->randomized.total_probes > 0);
        uint8_t max_pos = sorted_count;  // sorted_count = rnd position
        if (!has_rnd && sorted_count > 0) max_pos = sorted_count - 1;
        if (sorted_count == 0 && !has_rnd) return;  // Nothing to navigate

        int new_pos = (int)cursor_idx + delta;
        if (new_pos < 0) new_pos = 0;
        if ((uint8_t)new_pos > max_pos) new_pos = max_pos;

        cursor_idx = (uint8_t)new_pos;
        cursor_on_rnd = (cursor_idx == sorted_count && has_rnd);

        if (!cursor_on_rnd && sorted_count > 0) {
            memcpy(cursor_mac, s->devices[sorted_idx[cursor_idx]].mac, 6);
        }

        haptic_click();
        update_list_display();
        lv_refr_now(display_get_disp());
    } else {
        // Detail view: scroll SSID list (FR-18)
        ssid_scroll += delta;
        if (ssid_scroll < 0) ssid_scroll = 0;

        haptic_click();
        if (current_view == VIEW_DEVICE_DETAIL)
            update_detail_display();
        else
            update_rdet_display();
        lv_refr_now(display_get_disp());
    }
}

void scr_probe_sniff_on_tap() {
    haptic_click();

    if (current_view != VIEW_LIST) {
        // Back to list (FR-18)
        show_list_view();
        lv_refr_now(display_get_disp());
        return;
    }

    // List view: open detail for cursor's item (FR-14)
    ProbeSnifferState* s = probe_sniffer_get_state();

    if (cursor_on_rnd && s->randomized.unique_macs > 0) {
        show_randomized_detail();
    } else if (!cursor_on_rnd && sorted_count > 0) {
        show_device_detail();
    }
    lv_refr_now(display_get_disp());
}

void scr_probe_sniff_on_hold() {
    if (current_view != VIEW_LIST) return;  // FR-18: reserved (no-op)

    // Toggle pause (FR-05, FR-14)
    ProbeSnifferState* s = probe_sniffer_get_state();
    s->paused = !s->paused;

    haptic_double_click();

    if (!s->paused) {
        // Resume: merge accumulated changes
        update_list_display();
    }
    // If pausing: display freezes (update_screen skips list refresh)

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
    .enc_mode = ENC_MENU              // Was ENC_CHANNEL_HOP; encoder now scrolls list
};
```

---

## Task 4: Integration Changes

**Files:**
- Modify: `src/navigation.cpp:1-6` (add include)
- Modify: `src/navigation.cpp:55-58` (add probe_sniffer_stop to emergency stop)

- [ ] **Step 1: Add `#include "wifi_probe_sniffer.h"` to navigation.cpp**

After the existing `#include "attack_common.h"` (line 5), add:

```cpp
#include "wifi_probe_sniffer.h"
```

- [ ] **Step 2: Add `probe_sniffer_stop()` to emergency stop handler**

In `navigation_emergency_stop()` (line 55-58), add the call after `attack_emergency_stop()`:

Current:
```cpp
void navigation_emergency_stop() {
    attack_emergency_stop();  // Halt any active attack
    haptic_play(10);          // Strong double-pulse
    navigation_goto(SCREEN_MAIN_MENU);
}
```

New:
```cpp
void navigation_emergency_stop() {
    attack_emergency_stop();  // Halt any active attack
    probe_sniffer_stop();     // Halt any active probe capture
    haptic_play(10);          // Strong double-pulse
    navigation_goto(SCREEN_MAIN_MENU);
}
```

---

## Task 5: Compile, Flash, and Verify

- [ ] **Step 1: Compile**

Run: `pio run`

Expected: Clean compile with 0 errors. Warnings about unused variables are acceptable on first pass.

- [ ] **Step 2: Flash and run**

Run: `pio run -t upload --upload-port COM9`

- [ ] **Step 3: Verify acceptance criteria**

Manual hardware verification checklist (from FSD Section 7):

| AC | Test | Pass? |
|----|------|-------|
| AC-01 | Enter Probe Sniffer for first time. See "Listening..." and elapsed timer. | |
| AC-02 | Wait for probes. Device rows appear sorted by most recent. | |
| AC-03 | Status bar shows elapsed, probe count, device count, SSID count. | |
| AC-04 | Turn encoder — cursor moves through devices, viewport scrolls at edge. | |
| AC-05 | Tap on a device row — device detail opens with SSID list. | |
| AC-06 | Detail shows MAC, vendor, RSSI range, first/last seen, channel, SSIDs. | |
| AC-07 | Tap or backspin in detail — returns to list with cursor on same MAC. | |
| AC-08 | Hold in list — status shows PAUSED, rows freeze. Hold again — resumes. | |
| AC-09 | Randomized row sticky at bottom. Tap it — opens randomized detail. | |
| AC-10 | Randomized detail shows MAC count, probes, RSSI, SSIDs, last 8 MACs. | |
| AC-13 | New probe for non-top device promotes it to top. Cursor stays anchored. | |
| AC-14 | Backspin from list → WiFi menu. Re-enter → same data shown. | |
| AC-15 | Shake on either view → main menu. Re-enter → fresh session. | |
| AC-17 | Broadcast probes show as "(broadcast)" in detail SSIDs. | |

- [ ] **Step 4: Fix any issues found during verification**

Iterate on layout positioning, color, or logic bugs discovered during hardware testing. Re-compile and re-flash after each fix.
