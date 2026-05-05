# Phase 2 — Navigation System, BLE Scanner, Safe-Lock, Settings

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Phase 1's single-screen WiFi scanner with a multi-screen navigation system supporting BLE scanning, device lock, settings persistence, and heap monitoring.

**Architecture:** Gesture-driven navigation (backspin = menu, shake = emergency stop) with retain-and-hide screen lifecycle. Each screen module implements a `ScreenDef` interface. Navigation state machine routes encoder events to the active screen. NVS provides persistent settings. NimBLE adds BLE passive scanning.

**Tech Stack:** ESP32-S3, PlatformIO (espressif32@6.6.0), Arduino + ESP-IDF, LVGL 9.2, NimBLE-Arduino, ESP-IDF NVS, SHA-256 (mbedtls)

**Important:** User handles all git manually. No git commands in this plan. Testing is build+flash+verify on device (COM9).

---

## File Structure

### New Files

| File | Responsibility |
|------|---------------|
| `src/heap_monitor.h/.cpp` | Periodic heap logging, threshold alerts, baseline measurement |
| `src/gesture.h/.cpp` | Encoder velocity calculation, backspin detection, shake detection |
| `src/navigation.h/.cpp` | Screen registry, lifecycle management, state machine, encoder routing |
| `src/ble_scanner.h/.cpp` | NimBLE passive scanning, device parsing, dedup, aging |
| `src/safe_lock.h/.cpp` | Combination lock logic, SHA-256 hash verification, NVS storage |
| `src/settings.h/.cpp` | NVS read/write for all settings, defaults, validation |
| `src/screens/scr_main_menu.h/.cpp` | Main menu screen (WiFi / BLE / System groups) |
| `src/screens/scr_group_menu.h/.cpp` | Group menu screen (screens within a group) |
| `src/screens/scr_wifi_scan.h/.cpp` | WiFi scanner screen (extracted from display.cpp) |
| `src/screens/scr_ble_scan.h/.cpp` | BLE device list + detail view |
| `src/screens/scr_safe_lock.h/.cpp` | Safe-lock dial visualization |
| `src/screens/scr_settings.h/.cpp` | Settings list + inline editors |
| `src/screens/scr_debug.h/.cpp` | Real-time heap/system info |

### Modified Files

| File | Changes |
|------|---------|
| `src/main.cpp` | Complete rewrite: gesture → navigation → screen dispatch loop |
| `src/display.h/.cpp` | Extract WiFi rendering to screen module; expose shared colors/helpers; keep HW driver + LVGL setup |
| `src/encoder.h/.cpp` | Add event ring buffer with timestamps for gesture velocity |
| `src/haptic.h/.cpp` | Add `haptic_buzz()` and `haptic_alert()` helpers |
| `platformio.ini` | Add `nimble-arduino` lib dep, NimBLE PSRAM config flags |

### Unchanged Files

| File | Reason |
|------|--------|
| `src/wifi_scanner.h/.cpp` | Scanning logic unchanged; screen rendering moves out |
| `src/touch.h/.cpp` | Works as-is |
| `include/pins.h` | No new GPIO |
| `src/interchip.h` | Reserved for Phase 4 |

---

## Task 1: Heap Monitor

**Files:**
- Create: `src/heap_monitor.h`
- Create: `src/heap_monitor.cpp`

This is standalone with zero dependencies on other Phase 2 work. Start here for immediate baseline visibility.

- [ ] **Step 1: Create `src/heap_monitor.h`**

```cpp
#pragma once

#include <stdint.h>

#define HEAP_LOG_INTERVAL_MS   10000
#define HEAP_WARN_THRESHOLD    81920   // 80 KB
#define HEAP_CRITICAL_THRESHOLD 30720  // 30 KB

void heap_monitor_init();
void heap_monitor_update();  // Call from main loop
void heap_monitor_log_baseline(const char* label);
```

- [ ] **Step 2: Create `src/heap_monitor.cpp`**

```cpp
#include "heap_monitor.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

static uint32_t last_log_ms = 0;
static bool warned = false;
static bool critical = false;

void heap_monitor_init() {
    heap_monitor_log_baseline("boot");
}

void heap_monitor_update() {
    uint32_t now = millis();
    if (now - last_log_ms < HEAP_LOG_INTERVAL_MS) return;
    last_log_ms = now;

    uint32_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t min_ever = esp_get_minimum_free_heap_size();
    uint32_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    uint32_t psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    Serial.printf("[HEAP] free=%lu min=%lu largest=%lu psram=%lu\n",
                  free_int, min_ever, largest, psram);

    if (free_int < HEAP_CRITICAL_THRESHOLD && !critical) {
        Serial.printf("[HEAP CRITICAL] %lu bytes remaining\n", free_int);
        critical = true;
    } else if (free_int < HEAP_WARN_THRESHOLD && !warned) {
        Serial.printf("[HEAP WARNING] %lu bytes remaining\n", free_int);
        warned = true;
    }

    if (free_int >= HEAP_WARN_THRESHOLD) { warned = false; critical = false; }
}

void heap_monitor_log_baseline(const char* label) {
    uint32_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    Serial.printf("[HEAP BASELINE] after=%s free=%lu psram=%lu\n", label, free_int, psram);
}
```

- [ ] **Step 3: Integrate into main.cpp**

Add `#include "heap_monitor.h"` and call `heap_monitor_init()` at end of `setup()`, `heap_monitor_update()` in `loop()`.

- [ ] **Step 4: Build and verify**

Run: `pio run -e knob`
Flash and check serial for `[HEAP BASELINE]` and periodic `[HEAP]` lines every 10s.

---

## Task 2: Encoder Event Stream

**Files:**
- Modify: `src/encoder.h`
- Modify: `src/encoder.cpp`

The gesture module needs per-pulse timestamps and direction. Add a small event ring buffer to the encoder timer callback.

- [ ] **Step 1: Update `src/encoder.h`**

```cpp
#pragma once

#include <stdint.h>

struct EncoderEvent {
    int8_t   direction;     // +1 CW, -1 CCW
    uint32_t timestamp_us;  // micros() at detection
};

#define ENCODER_EVENT_SLOTS 16

void encoder_init(uint8_t pin_a, uint8_t pin_b);
int8_t encoder_get_delta();

// New: drain event buffer for gesture processing
uint8_t encoder_get_events(EncoderEvent* out, uint8_t max_count);
```

- [ ] **Step 2: Update `src/encoder.cpp`**

Add event ring buffer alongside existing delta accumulator. In `process_channel()`, when a valid edge is detected, push an event with `esp_timer_get_time()` timestamp.

```cpp
// Add to existing file:
static EncoderEvent event_ring[ENCODER_EVENT_SLOTS];
static volatile uint8_t evt_head = 0;
static volatile uint8_t evt_tail = 0;

// In process_channel(), where delta is incremented, also push event:
static void push_event(int8_t dir) {
    uint8_t next = (evt_head + 1) % ENCODER_EVENT_SLOTS;
    if (next != evt_tail) {
        event_ring[evt_head].direction = dir;
        event_ring[evt_head].timestamp_us = (uint32_t)esp_timer_get_time();
        evt_head = next;
    }
}

// New public function:
uint8_t encoder_get_events(EncoderEvent* out, uint8_t max_count) {
    portENTER_CRITICAL(&delta_mux);
    uint8_t count = 0;
    while (evt_tail != evt_head && count < max_count) {
        out[count] = event_ring[evt_tail];
        evt_tail = (evt_tail + 1) % ENCODER_EVENT_SLOTS;
        count++;
    }
    portEXIT_CRITICAL(&delta_mux);
    return count;
}
```

Modify `process_channel()` to call `push_event(step)` where it currently does `delta += step`.

- [ ] **Step 3: Build and verify**

Run: `pio run -e knob`
Existing `encoder_get_delta()` behavior must be unchanged. Flash and confirm WiFi scanner still responds to encoder.

---

## Task 3: Gesture Module

**Files:**
- Create: `src/gesture.h`
- Create: `src/gesture.cpp`

Processes encoder event stream to detect velocity, backspin (fast CCW flick), and shake (3+ reversals in 500ms). Knows nothing about UI.

- [ ] **Step 1: Create `src/gesture.h`**

```cpp
#pragma once

#include <stdint.h>

// Tunable constants
#define BACKSPIN_MIN_VELOCITY    20    // steps/sec CCW threshold
#define BACKSPIN_QUIET_MS        100   // silence after burst confirms intent
#define BACKSPIN_MIN_STEPS       3     // minimum CCW steps in burst
#define SHAKE_REVERSALS          3     // direction changes for emergency stop
#define SHAKE_WINDOW_MS          500   // time window for counting reversals
#define VELOCITY_RING_SIZE       4     // samples for velocity smoothing

enum GestureEvent {
    GESTURE_NONE,
    GESTURE_BACKSPIN,     // Fast CCW flick confirmed
    GESTURE_SHAKE         // 3+ rapid reversals confirmed
};

struct GestureState {
    float     velocity;             // Current velocity (steps/sec), signed (neg=CCW)
    float     peak_velocity;        // Peak in current burst
    uint8_t   ccw_burst_count;      // Consecutive fast CCW steps
    bool      backspin_armed;       // Fast CCW detected, waiting for quiet
    uint32_t  backspin_quiet_start; // millis() when burst ended
    int8_t    last_direction;       // +1 CW, -1 CCW
    uint8_t   reversal_count;       // Reversals in window
    uint32_t  reversal_timestamps[SHAKE_REVERSALS + 1];
};

void gesture_init();
GestureEvent gesture_update();  // Call from main loop; returns detected gesture
const GestureState* gesture_get_state();
int8_t gesture_get_delta();     // Accumulated delta (consumed by caller)
```

- [ ] **Step 2: Create `src/gesture.cpp`**

```cpp
#include "gesture.h"
#include "encoder.h"
#include <Arduino.h>

static GestureState state;
static int8_t accumulated_delta = 0;

// Ring buffer for inter-pulse timing
static uint32_t pulse_times[VELOCITY_RING_SIZE];
static uint8_t pulse_idx = 0;
static uint8_t pulse_count = 0;

void gesture_init() {
    memset(&state, 0, sizeof(state));
    memset(pulse_times, 0, sizeof(pulse_times));
    pulse_idx = 0;
    pulse_count = 0;
}

GestureEvent gesture_update() {
    // Drain encoder events
    EncoderEvent events[ENCODER_EVENT_SLOTS];
    uint8_t n = encoder_get_events(events, ENCODER_EVENT_SLOTS);

    for (uint8_t i = 0; i < n; i++) {
        accumulated_delta += events[i].direction;

        // Velocity calculation from timestamps
        pulse_times[pulse_idx] = events[i].timestamp_us;
        pulse_idx = (pulse_idx + 1) % VELOCITY_RING_SIZE;
        if (pulse_count < VELOCITY_RING_SIZE) pulse_count++;

        if (pulse_count >= 2) {
            uint8_t oldest = (pulse_idx + VELOCITY_RING_SIZE - pulse_count) % VELOCITY_RING_SIZE;
            uint8_t newest = (pulse_idx + VELOCITY_RING_SIZE - 1) % VELOCITY_RING_SIZE;
            uint32_t dt = pulse_times[newest] - pulse_times[oldest];
            if (dt > 0) {
                state.velocity = (float)(pulse_count - 1) * 1000000.0f / (float)dt;
                if (events[i].direction < 0) state.velocity = -state.velocity;
            }
        }

        // --- Shake detection: track direction reversals ---
        if (state.last_direction != 0 && events[i].direction != state.last_direction) {
            // Direction reversal
            uint32_t now_ms = events[i].timestamp_us / 1000;

            // Shift timestamps, add new
            if (state.reversal_count < SHAKE_REVERSALS + 1) {
                state.reversal_timestamps[state.reversal_count] = now_ms;
                state.reversal_count++;
            } else {
                for (uint8_t j = 0; j < SHAKE_REVERSALS; j++)
                    state.reversal_timestamps[j] = state.reversal_timestamps[j + 1];
                state.reversal_timestamps[SHAKE_REVERSALS] = now_ms;
            }

            // Check if N reversals happened within window
            if (state.reversal_count >= SHAKE_REVERSALS) {
                uint32_t first = state.reversal_timestamps[state.reversal_count - SHAKE_REVERSALS];
                if (now_ms - first <= SHAKE_WINDOW_MS) {
                    state.reversal_count = 0;
                    state.backspin_armed = false;
                    accumulated_delta = 0;
                    return GESTURE_SHAKE;
                }
            }
        }
        state.last_direction = events[i].direction;

        // --- Backspin detection: fast CCW burst followed by quiet ---
        if (events[i].direction == -1 && state.velocity < -BACKSPIN_MIN_VELOCITY) {
            state.ccw_burst_count++;
            state.backspin_armed = false;  // Still in burst
            if (-state.velocity > state.peak_velocity)
                state.peak_velocity = -state.velocity;
        } else if (events[i].direction == +1) {
            // CW breaks any CCW burst
            state.ccw_burst_count = 0;
            state.backspin_armed = false;
            state.peak_velocity = 0;
        }
    }

    // --- Backspin quiet period check (runs every loop, not per-event) ---
    uint32_t now = millis();

    if (!state.backspin_armed && state.ccw_burst_count >= BACKSPIN_MIN_STEPS) {
        // Burst ended (no new events this loop), start quiet timer
        if (n == 0) {
            state.backspin_armed = true;
            state.backspin_quiet_start = now;
        }
    }

    if (state.backspin_armed) {
        if (now - state.backspin_quiet_start >= BACKSPIN_QUIET_MS) {
            // Quiet period confirmed — backspin!
            state.backspin_armed = false;
            state.ccw_burst_count = 0;
            state.peak_velocity = 0;
            accumulated_delta = 0;
            return GESTURE_BACKSPIN;
        }
    }

    // Decay velocity when no events for a while
    if (n == 0 && pulse_count > 0) {
        // If last pulse was >200ms ago, velocity is effectively 0
        uint32_t last = pulse_times[(pulse_idx + VELOCITY_RING_SIZE - 1) % VELOCITY_RING_SIZE];
        if ((uint32_t)esp_timer_get_time() - last > 200000) {
            state.velocity = 0;
            pulse_count = 0;
        }
    }

    return GESTURE_NONE;
}

int8_t gesture_get_delta() {
    int8_t d = accumulated_delta;
    accumulated_delta = 0;
    return d;
}

const GestureState* gesture_get_state() {
    return &state;
}
```

- [ ] **Step 3: Build and verify**

Run: `pio run -e knob`
Temporarily add serial logging in main loop to print gesture events. Flash and test:
- Fast CCW flick → serial shows "BACKSPIN"
- Rapid CW-CCW-CW-CCW → serial shows "SHAKE"
- Normal slow turning → NONE (no false triggers)

---

## Task 4: Display Refactor — Expose Shared Utilities

**Files:**
- Modify: `src/display.h`
- Modify: `src/display.cpp`

Screen modules need access to the color palette, RSSI helpers, and LVGL display pointer. Export these while keeping the hardware driver private.

- [ ] **Step 1: Update `src/display.h`**

```cpp
#pragma once

#include <lvgl.h>
#include "wifi_scanner.h"

// --- Hardware + LVGL lifecycle ---
void display_init();
void display_splash();
void display_animate_splash(uint16_t duration_ms);
void display_flush();
void display_mark_dirty();
bool display_is_dirty();
void display_clear();

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

// --- Legacy WiFi screen API (will be removed after Task 6 refactor) ---
void display_wifi_scan(WifiScannerState *state);
void display_wifi_detail(AccessPoint *ap);
void display_scanning(uint8_t channel);
void display_update_live(WifiScannerState *state);
void display_update_arc_pulse();
```

- [ ] **Step 2: Update `src/display.cpp`**

- Move color `#define`s out (now in header)
- Remove `static` from `rssi_color()`, `rssi_bars()`, `rssi_to_arc()`, `rssi_arc_color()` — make them public
- Add `display_get_disp()`:

```cpp
lv_display_t* display_get_disp() {
    return lvgl_disp;
}
```

- [ ] **Step 3: Build and verify**

Run: `pio run -e knob`
No behavioral changes — just API exposure. Existing WiFi scanner must still work.

---

## Task 5: Navigation Framework

**Files:**
- Create: `src/navigation.h`
- Create: `src/navigation.cpp`

The core screen registry and lifecycle management. Defines `ScreenDef`, manages create/show/hide/destroy transitions, routes encoder events.

- [ ] **Step 1: Create `src/navigation.h`**

```cpp
#pragma once

#include <stdint.h>

enum ScreenGroup {
    GROUP_WIFI,
    GROUP_BLE,
    GROUP_SYSTEM,
    GROUP_COUNT
};

enum ScreenId {
    SCREEN_MAIN_MENU,
    SCREEN_GROUP_MENU,
    SCREEN_WIFI_SCAN,
    SCREEN_BLE_SCAN,
    SCREEN_SETTINGS,
    SCREEN_DEBUG,
    SCREEN_SAFE_LOCK,
    SCREEN_COUNT
};

enum EncoderMode {
    ENC_MENU,           // Menu navigation
    ENC_CHANNEL_HOP,    // WiFi scanner channel control
    ENC_BLE_LIST,       // BLE device list scroll
    ENC_SAFE_LOCK,      // Safe-lock dial
    ENC_SETTINGS,       // Settings scroll/adjust
    ENC_LOCKED          // Encoder ignored
};

struct ScreenDef {
    const char*   name;
    ScreenGroup   group;
    ScreenId      id;
    void          (*create)();    // First-time LVGL object creation
    void          (*show)();      // Called when screen becomes active
    void          (*hide)();      // Called when screen is deactivated
    void          (*destroy)();   // Free memory (NULL = retain forever)
    void          (*update)();    // Called each loop when active (live data)
    EncoderMode   enc_mode;
};

struct NavigationState {
    ScreenId      active_screen;
    ScreenGroup   active_group;
    uint8_t       group_selection;                   // Main menu cursor
    uint8_t       screen_selection[GROUP_COUNT];     // Per-group cursor
    bool          stealth_mode;
    uint32_t      last_activity_ms;
};

void navigation_init();
void navigation_register_screen(const ScreenDef* def);
void navigation_goto(ScreenId id);
void navigation_open_menu();       // Backspin handler
void navigation_emergency_stop();  // Shake handler
void navigation_update();          // Call from main loop (runs active screen's update)
void navigation_mark_activity();   // Reset auto-lock timer

ScreenId navigation_get_active();
EncoderMode navigation_get_encoder_mode();
const NavigationState* navigation_get_state();
```

- [ ] **Step 2: Create `src/navigation.cpp`**

```cpp
#include "navigation.h"
#include "display.h"
#include "haptic.h"
#include "heap_monitor.h"
#include <Arduino.h>
#include <lvgl.h>

static NavigationState nav_state;
static const ScreenDef* screen_registry[SCREEN_COUNT] = {};
static bool screen_created[SCREEN_COUNT] = {};

void navigation_init() {
    memset(&nav_state, 0, sizeof(nav_state));
    nav_state.active_screen = SCREEN_MAIN_MENU;
    nav_state.active_group = GROUP_WIFI;
    nav_state.last_activity_ms = millis();
}

void navigation_register_screen(const ScreenDef* def) {
    screen_registry[def->id] = def;
}

void navigation_goto(ScreenId id) {
    const ScreenDef* current = screen_registry[nav_state.active_screen];
    const ScreenDef* target  = screen_registry[id];
    if (!target) return;

    // Hide current
    if (current && current->hide) current->hide();

    // Create target if first visit
    if (!screen_created[id]) {
        if (target->create) target->create();
        screen_created[id] = true;
        heap_monitor_log_baseline(target->name);
    }

    // Show target
    if (target->show) target->show();

    nav_state.active_screen = id;
    nav_state.last_activity_ms = millis();

    Serial.printf("[nav] goto %s\n", target->name);
}

void navigation_open_menu() {
    haptic_play(14);  // Medium buzz
    navigation_goto(SCREEN_MAIN_MENU);
}

void navigation_emergency_stop() {
    haptic_play(10);  // Strong double-pulse
    // Stop active operations (WiFi/BLE) handled by screen hide callbacks
    navigation_goto(SCREEN_MAIN_MENU);
    Serial.println("[nav] EMERGENCY STOP");
}

void navigation_update() {
    const ScreenDef* active = screen_registry[nav_state.active_screen];
    if (active && active->update) active->update();
}

void navigation_mark_activity() {
    nav_state.last_activity_ms = millis();
}

ScreenId navigation_get_active() {
    return nav_state.active_screen;
}

EncoderMode navigation_get_encoder_mode() {
    const ScreenDef* active = screen_registry[nav_state.active_screen];
    if (active) return active->enc_mode;
    return ENC_MENU;
}

const NavigationState* navigation_get_state() {
    return &nav_state;
}
```

- [ ] **Step 3: Build and verify**

Run: `pio run -e knob`
Navigation compiles but isn't wired into main loop yet. That comes in Task 9.

---

## Task 6: WiFi Scanner Screen Module

**Files:**
- Create: `src/screens/scr_wifi_scan.h`
- Create: `src/screens/scr_wifi_scan.cpp`

Extract WiFi scan/detail rendering from `display.cpp` into a screen module implementing the `ScreenDef` lifecycle.

- [ ] **Step 1: Create `src/screens/scr_wifi_scan.h`**

```cpp
#pragma once

#include "navigation.h"

extern const ScreenDef scr_wifi_scan_def;

// Encoder handler for channel hopping (called by main loop when this screen is active)
void scr_wifi_scan_on_encoder(int8_t delta);
void scr_wifi_scan_on_tap();
void scr_wifi_scan_on_hold();
```

- [ ] **Step 2: Create `src/screens/scr_wifi_scan.cpp`**

Move all WiFi rendering code from `display.cpp` (the `build_scan_screen()`, `display_wifi_scan()`, `display_wifi_detail()`, `display_scanning()`, `display_update_live()`, `display_update_arc_pulse()` functions and their static LVGL pointers) into this file.

Implement lifecycle callbacks:
- `create()`: calls `build_scan_screen()` to create LVGL objects on a dedicated screen (`lv_obj_create(NULL)`)
- `show()`: `lv_screen_load(scr_root)` — screen retained between visits
- `hide()`: no-op (LVGL screen just becomes inactive)
- `destroy()`: NULL (never destroyed — retained)
- `update()`: calls `scanner_update()`, handles dirty check, renders

```cpp
#include "scr_wifi_scan.h"
#include "display.h"
#include "wifi_scanner.h"
#include "haptic.h"
#include <lvgl.h>

static lv_obj_t* scr_root = NULL;
// ... all the static LVGL object pointers from display.cpp Section C (scan screen) ...
// ... all the rendering functions moved from display.cpp ...

static void create() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
    // Build all LVGL objects on scr_root (same code as build_scan_screen but parented to scr_root)
    build_scan_screen();
}

static void show() {
    lv_screen_load(scr_root);
    // Trigger fresh scan display
    display_mark_dirty();
}

static void hide() {
    // State preserved in WifiScannerState — nothing to do
}

static void update() {
    scanner_update();
    WifiScannerState *s = scanner_get_state();

    // Same logic as current main loop steps 5-7
    // ... dirty check, render, live update, arc pulse ...
}

void scr_wifi_scan_on_encoder(int8_t delta) {
    WifiScannerState *s = scanner_get_state();
    if (s->detail_view) return;
    uint8_t ch = s->current_channel;
    ch = ((ch - 1 + delta + CHANNEL_MAX) % CHANNEL_MAX) + 1;
    scanner_set_channel(ch);
    haptic_click();
    display_mark_dirty();
}

void scr_wifi_scan_on_tap() {
    WifiScannerState *s = scanner_get_state();
    if (s->detail_view) {
        s->detail_view = false;
        display_mark_dirty();
    } else if (s->ap_count > 0) {
        s->selected_index = (s->selected_index + 1) % s->ap_count;
        memcpy(s->selected_bssid, s->ap_list[s->selected_index].bssid, 6);
        display_mark_dirty();
    }
}

void scr_wifi_scan_on_hold() {
    WifiScannerState *s = scanner_get_state();
    if (!s->detail_view && s->ap_count > 0) {
        s->detail_view = true;
        haptic_double_click();
        display_mark_dirty();
    }
}

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
```

- [ ] **Step 3: Remove WiFi rendering from `display.cpp`**

Delete Section C WiFi-specific code from `display.cpp` (everything after the `display_flush()` function). Keep:
- Section A (QSPI driver)
- Section B (LVGL setup)
- `display_init()`, `display_splash()`, `display_animate_splash()`, `display_clear()`, `display_mark_dirty()`, `display_is_dirty()`, `display_flush()`, `display_get_disp()`
- The shared helpers now declared in `display.h`

Remove legacy WiFi screen declarations from `display.h`.

- [ ] **Step 4: Build and verify**

Run: `pio run -e knob`
WiFi scanner should still work identically — just hosted in the new screen module.

---

## Task 7: Main Menu Screen

**Files:**
- Create: `src/screens/scr_main_menu.h`
- Create: `src/screens/scr_main_menu.cpp`

Three groups (WiFi / BLE / System) displayed vertically. Encoder scrolls, tap selects.

- [ ] **Step 1: Create `src/screens/scr_main_menu.h`**

```cpp
#pragma once

#include "navigation.h"

extern const ScreenDef scr_main_menu_def;

void scr_main_menu_on_encoder(int8_t delta);
void scr_main_menu_on_tap();
```

- [ ] **Step 2: Create `src/screens/scr_main_menu.cpp`**

```cpp
#include "scr_main_menu.h"
#include "display.h"
#include "haptic.h"
#include <lvgl.h>

static lv_obj_t* scr_root = NULL;
static lv_obj_t* lbl_groups[GROUP_COUNT] = {};
static lv_obj_t* lbl_title = NULL;
static uint8_t cursor = 0;

static const char* group_names[] = { "WiFi", "BLE", "System" };

static void update_highlight() {
    for (int i = 0; i < GROUP_COUNT; i++) {
        if (i == cursor) {
            lv_obj_set_style_text_color(lbl_groups[i], COL_CYAN, 0);
            lv_obj_set_style_text_font(lbl_groups[i], &lv_font_montserrat_28, 0);
        } else {
            lv_obj_set_style_text_color(lbl_groups[i], COL_GRAY, 0);
            lv_obj_set_style_text_font(lbl_groups[i], &lv_font_montserrat_20, 0);
        }
    }
}

static void create() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);

    lbl_title = lv_label_create(scr_root);
    lv_label_set_text(lbl_title, "NetKnob");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_title, COL_CYAN_DIM, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 40);

    int y_start = 130;
    int y_step = 60;
    for (int i = 0; i < GROUP_COUNT; i++) {
        lbl_groups[i] = lv_label_create(scr_root);
        lv_label_set_text(lbl_groups[i], group_names[i]);
        lv_obj_set_style_text_align(lbl_groups[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl_groups[i], LV_ALIGN_TOP_MID, 0, y_start + i * y_step);
    }

    update_highlight();
}

static void show() {
    // Restore cursor from navigation state
    const NavigationState* nav = navigation_get_state();
    cursor = nav->group_selection;
    update_highlight();
    lv_screen_load(scr_root);
}

static void hide() {
    // Save cursor to navigation state (done via navigation module)
}

static void update() {
    // Static screen — no periodic updates needed
}

void scr_main_menu_on_encoder(int8_t delta) {
    cursor = (cursor + delta + GROUP_COUNT) % GROUP_COUNT;
    update_highlight();
    haptic_click();
    lv_refr_now(display_get_disp());
}

void scr_main_menu_on_tap() {
    haptic_double_click();
    // Navigate to group menu for selected group
    // Navigation module handles: set active_group, goto GROUP_MENU
    navigation_goto(SCREEN_GROUP_MENU);
}

const ScreenDef scr_main_menu_def = {
    .name = "Main Menu",
    .group = GROUP_SYSTEM,  // doesn't belong to a group per se
    .id = SCREEN_MAIN_MENU,
    .create = create,
    .show = show,
    .hide = hide,
    .destroy = NULL,
    .update = update,
    .enc_mode = ENC_MENU
};
```

- [ ] **Step 3: Build and verify**

Run: `pio run -e knob`

---

## Task 8: Group Menu Screen

**Files:**
- Create: `src/screens/scr_group_menu.h`
- Create: `src/screens/scr_group_menu.cpp`

Shows screens within the selected group. Encoder scrolls, tap activates, backspin returns to main menu.

- [ ] **Step 1: Create `src/screens/scr_group_menu.h`**

```cpp
#pragma once

#include "navigation.h"

extern const ScreenDef scr_group_menu_def;

void scr_group_menu_on_encoder(int8_t delta);
void scr_group_menu_on_tap();
void scr_group_menu_set_group(ScreenGroup group);
```

- [ ] **Step 2: Create `src/screens/scr_group_menu.cpp`**

Similar pattern to main menu. Shows screens belonging to the active group. Uses the screen registry to list available screens per group.

Key implementation:
- Maintains a list of `ScreenId`s belonging to current group
- Encoder scrolls through them
- Tap activates the selected screen via `navigation_goto()`
- Title shows group name

```cpp
#include "scr_group_menu.h"
#include "display.h"
#include "haptic.h"
#include <lvgl.h>

#define MAX_SCREENS_PER_GROUP 8

static lv_obj_t* scr_root = NULL;
static lv_obj_t* lbl_title = NULL;
static lv_obj_t* lbl_items[MAX_SCREENS_PER_GROUP] = {};

static ScreenGroup current_group = GROUP_WIFI;
static ScreenId group_screens[MAX_SCREENS_PER_GROUP];
static const char* group_screen_names[MAX_SCREENS_PER_GROUP];
static uint8_t group_screen_count = 0;
static uint8_t cursor = 0;

// Group-to-screen mapping (hardcoded for Phase 2)
static void populate_group(ScreenGroup g) {
    group_screen_count = 0;
    current_group = g;
    switch (g) {
        case GROUP_WIFI:
            group_screens[0] = SCREEN_WIFI_SCAN;
            group_screen_names[0] = "Scanner";
            group_screen_count = 1;
            break;
        case GROUP_BLE:
            group_screens[0] = SCREEN_BLE_SCAN;
            group_screen_names[0] = "Scanner";
            group_screen_count = 1;
            break;
        case GROUP_SYSTEM:
            group_screens[0] = SCREEN_SETTINGS;
            group_screen_names[0] = "Settings";
            group_screens[1] = SCREEN_DEBUG;
            group_screen_names[1] = "Debug";
            group_screen_count = 2;
            break;
        default: break;
    }
}

static void update_display();  // Forward decl

static void create() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);

    lbl_title = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_title, COL_CYAN_DIM, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 40);

    for (int i = 0; i < MAX_SCREENS_PER_GROUP; i++) {
        lbl_items[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_align(lbl_items[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl_items[i], LV_ALIGN_TOP_MID, 0, 130 + i * 50);
        lv_obj_add_flag(lbl_items[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void show() {
    const NavigationState* nav = navigation_get_state();
    populate_group(nav->active_group);
    cursor = nav->screen_selection[current_group];
    if (cursor >= group_screen_count) cursor = 0;
    update_display();
    lv_screen_load(scr_root);
}

static void hide() {}

static void update() {}

static void update_display() {
    static const char* group_titles[] = { "WiFi", "BLE", "System" };
    lv_label_set_text(lbl_title, group_titles[current_group]);

    for (int i = 0; i < MAX_SCREENS_PER_GROUP; i++) {
        if (i < group_screen_count) {
            lv_label_set_text(lbl_items[i], group_screen_names[i]);
            lv_obj_clear_flag(lbl_items[i], LV_OBJ_FLAG_HIDDEN);
            if (i == cursor) {
                lv_obj_set_style_text_color(lbl_items[i], COL_CYAN, 0);
                lv_obj_set_style_text_font(lbl_items[i], &lv_font_montserrat_28, 0);
            } else {
                lv_obj_set_style_text_color(lbl_items[i], COL_GRAY, 0);
                lv_obj_set_style_text_font(lbl_items[i], &lv_font_montserrat_20, 0);
            }
        } else {
            lv_obj_add_flag(lbl_items[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void scr_group_menu_on_encoder(int8_t delta) {
    if (group_screen_count == 0) return;
    cursor = (cursor + delta + group_screen_count) % group_screen_count;
    update_display();
    haptic_click();
    lv_refr_now(display_get_disp());
}

void scr_group_menu_on_tap() {
    if (group_screen_count == 0) return;
    haptic_double_click();
    navigation_goto(group_screens[cursor]);
}

void scr_group_menu_set_group(ScreenGroup group) {
    current_group = group;
}

const ScreenDef scr_group_menu_def = {
    .name = "Group Menu",
    .group = GROUP_SYSTEM,
    .id = SCREEN_GROUP_MENU,
    .create = create,
    .show = show,
    .hide = hide,
    .destroy = NULL,
    .update = update,
    .enc_mode = ENC_MENU
};
```

- [ ] **Step 3: Build and verify**

Run: `pio run -e knob`

---

## Task 9: Main Loop Rewrite

**Files:**
- Modify: `src/main.cpp`

Rewire main.cpp to use gesture → navigation → screen dispatch. This is where everything comes together.

- [ ] **Step 1: Rewrite `src/main.cpp`**

```cpp
#include <Arduino.h>
#include <lvgl.h>
#include "display.h"
#include "encoder.h"
#include "touch.h"
#include "haptic.h"
#include "wifi_scanner.h"
#include "gesture.h"
#include "navigation.h"
#include "heap_monitor.h"
#include "pins.h"

// Screen definitions
#include "screens/scr_main_menu.h"
#include "screens/scr_group_menu.h"
#include "screens/scr_wifi_scan.h"
// #include "screens/scr_ble_scan.h"    // Task 15
// #include "screens/scr_settings.h"    // Task 11
// #include "screens/scr_debug.h"       // Task 16
// #include "screens/scr_safe_lock.h"   // Task 13

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("NetKnob Phase 2 — booting...");

    display_init();
    display_splash();

    encoder_init(PIN_ENC_A, PIN_ENC_B);
    touch_init();
    haptic_init();
    gesture_init();
    scanner_init();

    display_animate_splash(1500);

    // Register all screens
    navigation_init();
    navigation_register_screen(&scr_main_menu_def);
    navigation_register_screen(&scr_group_menu_def);
    navigation_register_screen(&scr_wifi_scan_def);
    // navigation_register_screen(&scr_ble_scan_def);
    // navigation_register_screen(&scr_settings_def);
    // navigation_register_screen(&scr_debug_def);

    // Boot to main menu (or safe-lock if enabled — Task 13)
    navigation_goto(SCREEN_MAIN_MENU);

    heap_monitor_init();
    Serial.println("[main] setup complete");
}

void loop() {
    // 1. Input: touch
    touch_read();
    touch_update();

    // 2. Input: gesture processing (consumes encoder events)
    GestureEvent gesture = gesture_update();

    // 3. Gesture-level actions (highest priority)
    if (gesture == GESTURE_SHAKE) {
        navigation_emergency_stop();
    } else if (gesture == GESTURE_BACKSPIN) {
        navigation_open_menu();
    } else {
        // 4. Route encoder delta to active screen
        int8_t delta = gesture_get_delta();
        if (delta != 0) {
            navigation_mark_activity();
            ScreenId active = navigation_get_active();
            switch (active) {
                case SCREEN_MAIN_MENU:
                    scr_main_menu_on_encoder(delta);
                    break;
                case SCREEN_GROUP_MENU:
                    scr_group_menu_on_encoder(delta);
                    break;
                case SCREEN_WIFI_SCAN:
                    scr_wifi_scan_on_encoder(delta);
                    break;
                // case SCREEN_BLE_SCAN:
                //     scr_ble_scan_on_encoder(delta);
                //     break;
                // case SCREEN_SETTINGS:
                //     scr_settings_on_encoder(delta);
                //     break;
                // case SCREEN_SAFE_LOCK:
                //     scr_safe_lock_on_encoder(delta);
                //     break;
                default: break;
            }
        }

        // 5. Route touch to active screen
        if (touch_tapped()) {
            navigation_mark_activity();
            ScreenId active = navigation_get_active();
            switch (active) {
                case SCREEN_MAIN_MENU:
                    scr_main_menu_on_tap();
                    break;
                case SCREEN_GROUP_MENU:
                    scr_group_menu_on_tap();
                    break;
                case SCREEN_WIFI_SCAN:
                    scr_wifi_scan_on_tap();
                    break;
                default: break;
            }
        }
        if (touch_held()) {
            navigation_mark_activity();
            ScreenId active = navigation_get_active();
            switch (active) {
                case SCREEN_WIFI_SCAN:
                    scr_wifi_scan_on_hold();
                    break;
                default: break;
            }
        }
    }

    // 6. Active screen update (live data, rendering)
    navigation_update();

    // 7. LVGL tick
    lv_timer_handler();

    // 8. Heap monitoring
    heap_monitor_update();
}
```

- [ ] **Step 2: Build and verify**

Run: `pio run -e knob`
Flash and test:
- Device boots to main menu
- Encoder scrolls between WiFi/BLE/System
- Tap on WiFi → group menu → Scanner → WiFi scanner works as before
- Fast CCW flick (backspin) from WiFi scanner → returns to main menu
- Rapid shake → returns to main menu

---

## Task 10: Settings Module + NVS

**Files:**
- Create: `src/settings.h`
- Create: `src/settings.cpp`
- Modify: `platformio.ini` (add NVS partition if needed — usually default on ESP32-S3)

- [ ] **Step 1: Create `src/settings.h`**

```cpp
#pragma once

#include <stdint.h>

struct Settings {
    bool     lock_enabled;
    uint8_t  lock_timeout_min;     // 0 = never
    uint8_t  wifi_region;          // 0=ETSI(1-13), 1=FCC(1-11), 2=Japan(1-14)
    uint8_t  display_brightness;   // 10-100
    bool     haptic_enabled;
    uint8_t  haptic_strength;      // 0=Weak, 1=Medium, 2=Strong
    bool     auto_scan_on_boot;
    uint8_t  splash_duration_sec;  // 0-5
};

void settings_init();              // Load from NVS or use defaults
void settings_save();              // Write all to NVS
const Settings* settings_get();
Settings* settings_get_mut();      // For editing

// Individual setters (save immediately)
void settings_set_lock_enabled(bool val);
void settings_set_lock_timeout(uint8_t min);
void settings_set_wifi_region(uint8_t region);
void settings_set_brightness(uint8_t pct);
void settings_set_haptic_enabled(bool val);
void settings_set_haptic_strength(uint8_t str);
void settings_set_auto_scan(bool val);
void settings_set_splash_duration(uint8_t sec);

// Lock code management
bool settings_verify_lock_code(uint8_t d1, uint8_t d2, uint8_t d3);
void settings_set_lock_code(uint8_t d1, uint8_t d2, uint8_t d3);
bool settings_has_lock_code();

// WiFi region helpers
uint8_t settings_max_channel();    // Returns 11, 13, or 14 based on region
```

- [ ] **Step 2: Create `src/settings.cpp`**

```cpp
#include "settings.h"
#include <Arduino.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <mbedtls/sha256.h>
#include <esp_mac.h>
#include <string.h>

static Settings cfg;
static nvs_handle_t nvs;
static uint8_t lock_hash[32] = {};  // Stored hash
static bool lock_hash_valid = false;

static const Settings defaults = {
    .lock_enabled = false,
    .lock_timeout_min = 5,
    .wifi_region = 0,       // ETSI
    .display_brightness = 80,
    .haptic_enabled = true,
    .haptic_strength = 1,   // Medium
    .auto_scan_on_boot = true,
    .splash_duration_sec = 2
};

static uint8_t clamp(uint8_t val, uint8_t lo, uint8_t hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

void settings_init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_open("settings", NVS_READWRITE, &nvs);

    // Load each setting, default if not present
    uint8_t u8;
    cfg = defaults;

    if (nvs_get_u8(nvs, "lock_en", &u8) == ESP_OK) cfg.lock_enabled = u8;
    if (nvs_get_u8(nvs, "lock_tout", &u8) == ESP_OK) cfg.lock_timeout_min = clamp(u8, 0, 60);
    if (nvs_get_u8(nvs, "wifi_rgn", &u8) == ESP_OK) cfg.wifi_region = clamp(u8, 0, 2);
    if (nvs_get_u8(nvs, "disp_brt", &u8) == ESP_OK) cfg.display_brightness = clamp(u8, 10, 100);
    if (nvs_get_u8(nvs, "haptic_en", &u8) == ESP_OK) cfg.haptic_enabled = u8;
    if (nvs_get_u8(nvs, "haptic_str", &u8) == ESP_OK) cfg.haptic_strength = clamp(u8, 0, 2);
    if (nvs_get_u8(nvs, "auto_scan", &u8) == ESP_OK) cfg.auto_scan_on_boot = u8;
    if (nvs_get_u8(nvs, "splash_dur", &u8) == ESP_OK) cfg.splash_duration_sec = clamp(u8, 0, 5);

    // Load lock hash
    size_t len = 32;
    if (nvs_get_blob(nvs, "lock_hash", lock_hash, &len) == ESP_OK && len == 32) {
        lock_hash_valid = true;
    }

    Serial.printf("[settings] loaded (lock=%d, region=%d, brightness=%d)\n",
                  cfg.lock_enabled, cfg.wifi_region, cfg.display_brightness);
}

void settings_save() {
    nvs_set_u8(nvs, "lock_en", cfg.lock_enabled);
    nvs_set_u8(nvs, "lock_tout", cfg.lock_timeout_min);
    nvs_set_u8(nvs, "wifi_rgn", cfg.wifi_region);
    nvs_set_u8(nvs, "disp_brt", cfg.display_brightness);
    nvs_set_u8(nvs, "haptic_en", cfg.haptic_enabled);
    nvs_set_u8(nvs, "haptic_str", cfg.haptic_strength);
    nvs_set_u8(nvs, "auto_scan", cfg.auto_scan_on_boot);
    nvs_set_u8(nvs, "splash_dur", cfg.splash_duration_sec);
    nvs_commit(nvs);
}

const Settings* settings_get() { return &cfg; }
Settings* settings_get_mut() { return &cfg; }

void settings_set_lock_enabled(bool val) { cfg.lock_enabled = val; settings_save(); }
void settings_set_lock_timeout(uint8_t min) { cfg.lock_timeout_min = clamp(min, 0, 60); settings_save(); }
void settings_set_wifi_region(uint8_t r) { cfg.wifi_region = clamp(r, 0, 2); settings_save(); }
void settings_set_brightness(uint8_t pct) { cfg.display_brightness = clamp(pct, 10, 100); settings_save(); }
void settings_set_haptic_enabled(bool val) { cfg.haptic_enabled = val; settings_save(); }
void settings_set_haptic_strength(uint8_t str) { cfg.haptic_strength = clamp(str, 0, 2); settings_save(); }
void settings_set_auto_scan(bool val) { cfg.auto_scan_on_boot = val; settings_save(); }
void settings_set_splash_duration(uint8_t sec) { cfg.splash_duration_sec = clamp(sec, 0, 5); settings_save(); }

// --- Lock code hashing ---

static void compute_hash(uint8_t d1, uint8_t d2, uint8_t d3, uint8_t out[32]) {
    // Salt from device MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    uint8_t input[9]; // 3 digits + 6 MAC bytes
    input[0] = d1;
    input[1] = d2;
    input[2] = d3;
    memcpy(&input[3], mac, 6);

    mbedtls_sha256(input, 9, out, 0);  // 0 = SHA-256 (not SHA-224)
}

void settings_set_lock_code(uint8_t d1, uint8_t d2, uint8_t d3) {
    compute_hash(d1, d2, d3, lock_hash);
    nvs_set_blob(nvs, "lock_hash", lock_hash, 32);
    nvs_commit(nvs);
    lock_hash_valid = true;
}

bool settings_verify_lock_code(uint8_t d1, uint8_t d2, uint8_t d3) {
    if (!lock_hash_valid) return true;  // No code set = always pass

    uint8_t attempt[32];
    compute_hash(d1, d2, d3, attempt);

    // Constant-time comparison
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= attempt[i] ^ lock_hash[i];
    return diff == 0;
}

bool settings_has_lock_code() { return lock_hash_valid; }

uint8_t settings_max_channel() {
    switch (cfg.wifi_region) {
        case 1: return 11;  // FCC
        case 2: return 14;  // Japan
        default: return 13; // ETSI
    }
}
```

- [ ] **Step 3: Wire into setup**

In `main.cpp`, add `#include "settings.h"` and call `settings_init()` early in `setup()` (before display splash so brightness can be applied).

- [ ] **Step 4: Build and verify**

Run: `pio run -e knob`
Check serial for `[settings] loaded` message. Verify NVS partition exists (default on ESP32-S3).

---

## Task 11: Settings Screen

**Files:**
- Create: `src/screens/scr_settings.h`
- Create: `src/screens/scr_settings.cpp`

Vertical list of settings with inline editing. Encoder scrolls list or adjusts values. Tap enters/confirms edit.

- [ ] **Step 1: Create `src/screens/scr_settings.h`**

```cpp
#pragma once

#include "navigation.h"

extern const ScreenDef scr_settings_def;

void scr_settings_on_encoder(int8_t delta);
void scr_settings_on_tap();
```

- [ ] **Step 2: Create `src/screens/scr_settings.cpp`**

Implements a list with 9 settings rows. Two modes:
- **Browse mode**: encoder scrolls cursor, tap enters edit
- **Edit mode**: encoder adjusts value, tap confirms and saves

```cpp
#include "scr_settings.h"
#include "display.h"
#include "settings.h"
#include "haptic.h"
#include <lvgl.h>

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

static void get_value_str(uint8_t idx, char* buf, size_t len);
static void update_display();

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
        lv_obj_set_style_text_font(lbl_name[i], &lv_font_montserrat_16, 0);
        lv_obj_set_pos(lbl_name[i], 40, y);

        lbl_value[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_font(lbl_value[i], &lv_font_montserrat_16, 0);
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

void scr_settings_on_encoder(int8_t delta) {
    Settings* s = settings_get_mut();
    if (!editing) {
        // Scroll cursor
        int8_t new_cursor = (int8_t)cursor + delta;
        if (new_cursor < 0) new_cursor = 0;
        if (new_cursor >= SETTING_COUNT) new_cursor = SETTING_COUNT - 1;
        cursor = new_cursor;
        // Adjust scroll
        if (cursor < scroll_offset) scroll_offset = cursor;
        if (cursor >= scroll_offset + VISIBLE_ROWS) scroll_offset = cursor - VISIBLE_ROWS + 1;
    } else {
        // Adjust value
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
        // Confirm edit
        editing = false;
        haptic_double_click();
    } else {
        // Enter edit or toggle
        switch (setting_defs[cursor].type) {
            case ST_TOGGLE:
                switch (cursor) {
                    case 0: settings_set_lock_enabled(!s->lock_enabled); break;
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
                // Change code flow — TODO: Task 12
                haptic_double_click();
                break;
        }
    }
    update_display();
}

// Helper
static uint8_t clamp_add(uint8_t val, int8_t delta, uint8_t lo, uint8_t hi) {
    int16_t result = (int16_t)val + delta;
    if (result < lo) return lo;
    if (result > hi) return hi;
    return (uint8_t)result;
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
```

- [ ] **Step 3: Register in main.cpp and add routing**

Uncomment registration and add encoder/tap routing for `SCREEN_SETTINGS`.

- [ ] **Step 4: Build and verify**

Run: `pio run -e knob`
Navigate: Main Menu → System → Settings. Toggle haptic, change brightness, verify persistence across reboot.

---

## Task 12: Safe-Lock Logic + Screen

**Files:**
- Create: `src/safe_lock.h`
- Create: `src/safe_lock.cpp`
- Create: `src/screens/scr_safe_lock.h`
- Create: `src/screens/scr_safe_lock.cpp`

3-digit combination lock (0-39) with alternating CW/CCW/CW directions.

- [ ] **Step 1: Create `src/safe_lock.h`**

```cpp
#pragma once

#include <stdint.h>

#define LOCK_POSITIONS 40
#define LOCK_DIGITS 3

enum LockPhase {
    LOCK_DIGIT_1_CW,
    LOCK_DIGIT_2_CCW,
    LOCK_DIGIT_3_CW,
    LOCK_OPEN_CCW,
    LOCK_SUCCESS,
    LOCK_FAILED
};

struct SafeLockState {
    uint8_t   current_position;    // 0-39
    uint8_t   entered_digits[3];
    LockPhase phase;
    uint8_t   attempt_count;
    uint32_t  lockout_until_ms;
};

void safe_lock_init();
void safe_lock_reset();
void safe_lock_on_encoder(int8_t delta);  // Rotate dial
LockPhase safe_lock_get_phase();
const SafeLockState* safe_lock_get_state();
bool safe_lock_is_locked_out();
```

- [ ] **Step 2: Create `src/safe_lock.cpp`**

Implements the combination mechanism:
- Phase 1: CW to first digit, stop confirms
- Phase 2: CCW past first to second digit, stop confirms  
- Phase 3: CW past second to third digit, stop confirms
- Phase 4: Short CCW to open (any CCW step triggers verification)

"Stop" detection: 500ms of no encoder movement while in a digit phase transitions to next phase.

- [ ] **Step 3: Create `src/screens/scr_safe_lock.h` and `.cpp`**

Visual: circular number ring (0-39) around display edge, fixed marker at top, ring rotates with encoder, progress indicator (1/3, 2/3, 3/3), direction arrow.

Lifecycle: created on lock trigger, destroyed after successful unlock.

- [ ] **Step 4: Integrate with boot flow**

In `main.cpp` setup: after `settings_init()`, check `settings_get()->lock_enabled`. If true, goto `SCREEN_SAFE_LOCK` instead of `SCREEN_MAIN_MENU`.

- [ ] **Step 5: Auto-lock integration**

In main loop, check `navigation_get_state()->last_activity_ms`. If exceeded timeout and lock enabled, trigger lock screen.

- [ ] **Step 6: Build and verify**

Run: `pio run -e knob`
Enable lock in settings, set code, reboot → lock screen appears. Enter correct code → main menu. Wrong code → error + lockout escalation.

---

## Task 13: BLE Scanner Module

**Files:**
- Create: `src/ble_scanner.h`
- Create: `src/ble_scanner.cpp`
- Modify: `platformio.ini` (add NimBLE dependency)

- [ ] **Step 1: Update `platformio.ini`**

```ini
lib_deps =
    lvgl/lvgl@~9.2.0
    h2zero/NimBLE-Arduino@^1.4.0

build_flags =
    ... (existing flags) ...
    -DCONFIG_BT_NIMBLE_ROLE_CENTRAL=1
    -DCONFIG_BT_NIMBLE_ROLE_OBSERVER=1
    -DCONFIG_BT_NIMBLE_ROLE_PERIPHERAL=0
    -DCONFIG_BT_NIMBLE_ROLE_BROADCASTER=0
```

- [ ] **Step 2: Create `src/ble_scanner.h`**

```cpp
#pragma once

#include <stdint.h>

#define BLE_MAX_DEVICES     50
#define BLE_STALE_MS        60000
#define BLE_REMOVE_MS       120000
#define BLE_SCAN_INTERVAL   160   // 100ms in 0.625ms units
#define BLE_SCAN_WINDOW     128   // 80ms in 0.625ms units

enum BleDeviceType {
    BLE_TYPE_UNKNOWN,
    BLE_TYPE_PHONE,
    BLE_TYPE_COMPUTER,
    BLE_TYPE_WATCH,
    BLE_TYPE_HEADPHONES,
    BLE_TYPE_SPEAKER,
    BLE_TYPE_BEACON,
    BLE_TYPE_IOT
};

struct BleDevice {
    uint8_t   mac[6];
    uint8_t   addr_type;       // 0=Public, 1=Random, 2=RPA
    char      name[30];
    int8_t    rssi;
    int8_t    rssi_avg;
    int8_t    tx_power;
    uint8_t   device_type;     // BleDeviceType
    uint16_t  company_id;
    uint16_t  service_uuids[8];
    uint8_t   service_count;
    uint32_t  first_seen_ms;
    uint32_t  last_seen_ms;
    bool      stale;
};

struct BleScannerState {
    BleDevice devices[BLE_MAX_DEVICES];
    uint8_t   device_count;
    uint8_t   selected_index;
    uint8_t   scroll_offset;
    bool      scanning;
    bool      detail_view;
};

void ble_scanner_init();
void ble_scanner_start();
void ble_scanner_stop();
void ble_scanner_update();   // Call from main loop: age devices, mark stale
BleScannerState* ble_scanner_get_state();
const char* ble_manufacturer_lookup(uint16_t company_id);
const char* ble_device_type_str(uint8_t type);
```

- [ ] **Step 3: Create `src/ble_scanner.cpp`**

NimBLE passive scanning implementation:
- Scan callback parses advertisement data (name, RSSI, address type, manufacturer, services, TX power)
- Dedup by MAC address, update RSSI with running average
- Aging: mark stale at 60s, remove at 120s
- When list full, evict weakest RSSI that's also stale (or oldest)
- Sort by RSSI descending

Include manufacturer lookup table (25 entries from FSD section FR-21).

- [ ] **Step 4: Build and verify**

Run: `pio run -e knob`
Add temporary serial logging in scan callback. Flash and verify BLE devices appear in serial output.

---

## Task 14: BLE Scanner Screen

**Files:**
- Create: `src/screens/scr_ble_scan.h`
- Create: `src/screens/scr_ble_scan.cpp`

Same interaction pattern as WiFi scanner: list view with RSSI color coding, tap to select, hold for detail view.

- [ ] **Step 1: Create `src/screens/scr_ble_scan.h`**

```cpp
#pragma once

#include "navigation.h"

extern const ScreenDef scr_ble_scan_def;

void scr_ble_scan_on_encoder(int8_t delta);
void scr_ble_scan_on_tap();
void scr_ble_scan_on_hold();
```

- [ ] **Step 2: Create `src/screens/scr_ble_scan.cpp`**

Key differences from WiFi scanner screen:
- Shows up to 7 visible devices (smaller font)
- Encoder scrolls the list (not channel hop)
- Device type icon prefix (text-based: `[P]` phone, `[C]` computer, `[H]` headphones, etc.)
- Detail view shows: name, MAC, address type, RSSI, device type, services, manufacturer, TX power

Lifecycle:
- `create()`: Build list view and detail view LVGL objects on `scr_root`
- `show()`: Start BLE scan, load screen
- `hide()`: Pause BLE scan (preserve device list)
- `update()`: Call `ble_scanner_update()`, refresh display at 2Hz

- [ ] **Step 3: Register and route in main.cpp**

Add `scr_ble_scan_def` registration and encoder/tap/hold routing.

- [ ] **Step 4: Build and verify**

Run: `pio run -e knob`
Navigate: Main Menu → BLE → Scanner. Verify devices appear, scroll works, detail view shows correct data. Test WiFi+BLE coexistence (visit WiFi scanner, then BLE scanner — both should have data).

---

## Task 15: Debug Screen

**Files:**
- Create: `src/screens/scr_debug.h`
- Create: `src/screens/scr_debug.cpp`

On-demand screen (created fresh each visit, destroyed on exit) showing real-time system info.

- [ ] **Step 1: Create `src/screens/scr_debug.h`**

```cpp
#pragma once

#include "navigation.h"

extern const ScreenDef scr_debug_def;
```

- [ ] **Step 2: Create `src/screens/scr_debug.cpp`**

```cpp
#include "scr_debug.h"
#include "display.h"
#include "heap_monitor.h"
#include "ble_scanner.h"
#include "wifi_scanner.h"
#include <lvgl.h>
#include <esp_heap_caps.h>

static lv_obj_t* scr_root = NULL;
static lv_obj_t* lbl_lines[9] = {};
static uint32_t last_update_ms = 0;

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
    .destroy = destroy,  // Destroyed on exit to free memory
    .update = update,
    .enc_mode = ENC_MENU
};
```

- [ ] **Step 3: Register and verify**

Run: `pio run -e knob`
Navigate: Main Menu → System → Debug. Verify real-time values update every second.

---

## Task 16: Palm Cover / Stealth Mode (Experimental)

**Files:**
- Modify: `src/touch.h/.cpp` (add area detection if CST816T supports it)
- Modify: `src/navigation.cpp` (stealth mode logic)

- [ ] **Step 1: Test CST816T touch area detection**

Read CST816T register 0x03 (gesture ID) and check if large-area touch produces a distinguishable gesture code or if multiple-finger data is available. Log raw touch data for palm vs finger.

- [ ] **Step 2: Implement if hardware supports it, or skip**

If CST816T cannot distinguish palm from finger (likely based on FSD note), implement fallback: touch hold (3s) + encoder inactive = stealth. Or defer to Phase 3.

- [ ] **Step 3: Stealth mode actions**

```cpp
void navigation_enter_stealth() {
    esp_wifi_stop();
    ble_scanner_stop();
    // Blank display (backlight off)
    ledcWrite(0, 0);
    nav_state.stealth_mode = true;
    haptic_play(14);  // Long pulse
}

void navigation_exit_stealth() {
    ledcWrite(0, settings_get()->display_brightness * 255 / 100);
    // Restore previous screen state
    nav_state.stealth_mode = false;
    haptic_double_click();
}
```

- [ ] **Step 4: Build and verify**

Run: `pio run -e knob`
Test stealth trigger and recovery.

---

## Task 17: Integration, Tuning, and Acceptance Testing

- [ ] **Step 1: Gesture threshold tuning**

Flash and test backspin detection on physical device. Adjust `BACKSPIN_MIN_VELOCITY` (start at 20, tune based on feel). Verify:
- Normal fast scrolling does NOT trigger backspin
- Deliberate flick reliably triggers backspin
- Shake requires intentional back-and-forth (not triggered by backspin)

- [ ] **Step 2: Memory leak testing**

Run navigation stress test via serial monitoring:
- Navigate through all screens 100 times
- Watch `[HEAP]` serial output — free heap should not monotonically decrease
- Check `[HEAP BASELINE]` deltas for each screen create/destroy

- [ ] **Step 3: BLE + WiFi coexistence**

Leave both scanners active for 30 minutes. Monitor for crashes, heap degradation, or radio interference.

- [ ] **Step 4: Lock stress test**

Enter wrong lock code 20 times, verify escalating lockout (1s, 5s, 30s). Enter correct code on 21st — must unlock normally.

- [ ] **Step 5: Acceptance criteria verification**

Walk through all 52 acceptance criteria from FSD Section 10. Mark each as pass/fail.

---

## Implementation Order Summary

```
Task  1: Heap Monitor .................. standalone, immediate value
Task  2: Encoder Event Stream .......... foundation for gestures
Task  3: Gesture Module ................ backspin + shake detection
Task  4: Display Refactor .............. expose shared utilities
Task  5: Navigation Framework .......... screen lifecycle + state machine
Task  6: WiFi Scanner Screen ........... extract from display.cpp
Task  7: Main Menu Screen .............. first navigation UI
Task  8: Group Menu Screen ............. second navigation UI
Task  9: Main Loop Rewrite ............. wire everything together
Task 10: Settings + NVS ................ persistence layer
Task 11: Settings Screen ............... UI for settings
Task 12: Safe-Lock .................... logic + screen + boot flow
Task 13: BLE Scanner Module ............ NimBLE integration
Task 14: BLE Scanner Screen ............ UI for BLE
Task 15: Debug Screen .................. system visibility
Task 16: Palm Cover (experimental) ..... may skip if HW unsupported
Task 17: Integration & Tuning .......... acceptance testing
```

**Critical path:** Tasks 1-9 must be sequential (each builds on previous). Tasks 10-15 can be parallelized after Task 9 is stable. Task 16 is optional. Task 17 is final.

**First milestone (navigable):** After Task 9 — device boots to menu, WiFi scanner accessible via navigation, backspin/shake work.

**Second milestone (feature-complete):** After Task 15 — all screens working, settings persist, BLE scanning.

---

## Key Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Backspin triggers during normal scrolling | Require deceleration profile (not just speed); tune on device |
| NimBLE + WiFi memory contention | Add PSRAM allocation flags; monitor heap from day 1 |
| LVGL retained screens exceed budget | Heap baseline logging validates after each screen create |
| Safe-lock "stop" detection too sensitive | Tune stop timeout (500ms); require minimum rotation before confirming digit |
| Display refactor breaks WiFi scanner | Do refactor incrementally; verify WiFi works after each change |
