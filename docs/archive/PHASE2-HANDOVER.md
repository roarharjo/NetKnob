# Phase 2 Handover Guide

> For anyone picking up this codebase for Phase 3 development.
> Covers architecture, how to add new screens, what to watch out for.

---

## Architecture Overview

### Main Loop Flow (main.cpp)

```
setup()
  settings_init()       ← NVS load (before display for brightness)
  display_init()        ← QSPI + LVGL
  display_splash()      ← Matrix rain animation
  encoder_init()        ← 3ms timer polling
  touch_init()          ← CST816T I2C
  haptic_init()         ← DRV2605L LRA
  gesture_init()        ← Velocity/backspin/shake state
  scanner_init()        ← WiFi promiscuous mode
  ble_scanner_init()    ← NimBLE
  safe_lock_init()      ← Lock state
  navigation_init()     ← Screen registry
  register all screens
  navigation_goto(SCREEN_SAFE_LOCK or SCREEN_MAIN_MENU)

loop()
  1. touch_read() + touch_update()
  2. gesture_update()           ← Consumes encoder events
  3. if SHAKE → emergency_stop()
     if BACKSPIN → open_menu()
     else:
       route encoder delta to active screen
       route tap/hold to active screen
  4. navigation_update()        ← Calls active screen's update()
  5. lv_timer_handler()
  6. heap_monitor_update()
  7. auto-lock check
  8. serial heartbeat
```

### Screen Lifecycle

Each screen implements `ScreenDef` (navigation.h):

```cpp
struct ScreenDef {
    const char*   name;
    ScreenGroup   group;       // GROUP_WIFI, GROUP_BLE, GROUP_SYSTEM
    ScreenId      id;
    void          (*create)(); // First-time LVGL object creation
    void          (*show)();   // Called when screen becomes active
    void          (*hide)();   // Called when screen is deactivated
    void          (*destroy)();// Free memory (NULL = retain forever)
    void          (*update)(); // Called each loop when active
    EncoderMode   enc_mode;
};
```

**Retained screens** (never destroyed): Main Menu, Group Menu, WiFi Scanner, BLE Scanner, Settings
**On-demand screens** (destroyed on exit): Debug, Safe-Lock

### Navigation Hierarchy

```
Main Menu (WiFi / BLE / System)
  ├─ WiFi Group
  │   └─ WiFi Scanner
  │       (Phase 3: Deauth, Beacon Flood, Probe Sniffer, PMKID)
  ├─ BLE Group
  │   └─ BLE Scanner
  │       (Phase 3: AppleJuice, SwiftPair, BLE Spam)
  └─ System Group
      ├─ Settings
      └─ Debug
```

### Gesture System

The gesture module (`gesture.cpp`) sits between the encoder and the navigation system:

```
Encoder timer (3ms) → Event ring buffer → gesture_update() → GestureEvent
                                                            → gesture_get_delta()
```

- **Backspin**: Fast CCW (>20 steps/sec) + 100ms quiet period → opens main menu
- **Shake**: 3+ direction reversals in 500ms → emergency stop
- **Normal delta**: Passed through to active screen's encoder handler

---

## How to Add a New Screen (Phase 3)

### 1. Add ScreenId to navigation.h

```cpp
enum ScreenId {
    // ... existing ...
    SCREEN_DEAUTH,    // Add here
    SCREEN_COUNT
};
```

### 2. Create the screen module

Create `src/screens/scr_deauth.h`:
```cpp
#pragma once
#include "navigation.h"

extern const ScreenDef scr_deauth_def;

void scr_deauth_on_encoder(int8_t delta);
void scr_deauth_on_tap();
void scr_deauth_on_hold();  // If needed
```

Create `src/screens/scr_deauth.cpp` following the pattern in `scr_wifi_scan.cpp`:
- Static `scr_root` created via `lv_obj_create(NULL)`
- `create()`: build LVGL objects on `scr_root`
- `show()`: `lv_screen_load(scr_root)`, start any operations
- `hide()`: stop operations, preserve state
- `update()`: called every loop iteration when active
- Define `ScreenDef` with appropriate `group`, `id`, `enc_mode`

### 3. Add to group menu

In `src/screens/scr_group_menu.cpp`, add the screen to the appropriate group in `populate_group()`:

```cpp
case GROUP_WIFI:
    group_screens[0] = SCREEN_WIFI_SCAN;
    group_screen_names[0] = "Scanner";
    group_screens[1] = SCREEN_DEAUTH;       // Add
    group_screen_names[1] = "Deauth";       // Add
    group_screen_count = 2;                 // Update count
    break;
```

### 4. Register and route in main.cpp

```cpp
// In setup():
#include "screens/scr_deauth.h"
navigation_register_screen(&scr_deauth_def);

// In loop() encoder routing:
case SCREEN_DEAUTH:
    scr_deauth_on_encoder(delta);
    break;

// In loop() tap routing:
case SCREEN_DEAUTH:
    scr_deauth_on_tap();
    break;
```

### 5. Build and test

No other changes needed. The navigation system handles create/show/hide transitions automatically.

---

## Key Interfaces

### WiFi Scanner (wifi_scanner.h)

```cpp
void scanner_init();
void scanner_set_channel(uint8_t ch);
void scanner_update();                    // Drain ring buffer, age APs
WifiScannerState* scanner_get_state();    // AP list, channel, counts
```

For Phase 3 attacks: WiFi is already in promiscuous mode. To inject frames, call `esp_wifi_80211_tx()` directly. The scanner module doesn't need modification — attack modules can coexist.

### BLE Scanner (ble_scanner.h)

```cpp
void ble_scanner_init();
void ble_scanner_start();
void ble_scanner_stop();
void ble_scanner_update();                // Age devices, sort
BleScannerState* ble_scanner_get_state(); // Device list
```

For Phase 3 BLE attacks (AppleJuice, SwiftPair): stop the passive scanner, switch NimBLE to advertising mode. The scanner can be restarted afterward.

### Settings (settings.h)

```cpp
const Settings* settings_get();
uint8_t settings_max_channel();  // Returns 11/13/14 based on wifi_region
```

To add new settings: add field to `Settings` struct, add NVS key in `settings_init()`/`settings_save()`, add setter function, add row in `scr_settings.cpp`.

### Navigation (navigation.h)

```cpp
void navigation_goto(ScreenId id);
void navigation_open_menu();          // Backspin handler
void navigation_emergency_stop();     // Shake handler
ScreenId navigation_get_active();
EncoderMode navigation_get_encoder_mode();
```

### Display Shared Utilities (display.h)

```cpp
lv_display_t* display_get_disp();     // For lv_refr_now()

// Colour palette
COL_BG, COL_CYAN, COL_CYAN_DIM, COL_GREEN, COL_ORANGE, COL_RED,
COL_WHITE, COL_GRAY, COL_DARK, COL_SELECTED

// RSSI helpers (shared by WiFi + BLE screens)
lv_color_t rssi_color(int8_t rssi);
const char* rssi_bars(int8_t rssi);
```

---

## Constants to Tune

| Constant | File | Default | Effect |
|---|---|---|---|
| `BACKSPIN_MIN_VELOCITY` | gesture.h | 20 | Steps/sec CCW threshold for menu open |
| `BACKSPIN_QUIET_MS` | gesture.h | 100 | Silence after burst to confirm backspin |
| `SHAKE_REVERSALS` | gesture.h | 3 | Direction changes for emergency stop |
| `SHAKE_WINDOW_MS` | gesture.h | 500 | Time window for counting reversals |
| `LOCK_STOP_MS` | safe_lock.h | 500 | No-encoder time to confirm a lock digit |
| `BLE_STALE_MS` | ble_scanner.h | 60000 | Time before BLE device is dimmed |
| `BLE_REMOVE_MS` | ble_scanner.h | 120000 | Time before BLE device is evicted |
| `BLE_MAX_DEVICES` | ble_scanner.h | 50 | Maximum tracked BLE devices |
| `HEAP_WARN_THRESHOLD` | heap_monitor.h | 81920 | Warning at <80 KB free SRAM |
| `HEAP_CRITICAL_THRESHOLD` | heap_monitor.h | 30720 | Critical at <30 KB free SRAM |

---

## Known Gotchas

### Backspin vs Fast Scrolling

Backspin detection requires both high velocity AND a deceleration profile (burst followed by silence). Sustained fast scrolling does not trigger backspin because there's no quiet period. If adding a screen with very fast encoder interaction, test that backspin doesn't false-trigger.

### BLE + WiFi Coexistence

Both radios work simultaneously on ESP32-S3. However, under heavy load both can slow each other down. WiFi promiscuous mode is always running (even when on the BLE screen). BLE scan starts on BLE screen entry and stops on exit.

### LVGL Object Lifetimes

Never access an LVGL object pointer after `lv_obj_del()` or after its parent screen is destroyed. The safe-lock and debug screens null all their pointers in `destroy()`. If adding a new destroyable screen, null every static pointer.

### NVS Write Timing

`settings_save()` calls `nvs_commit()` which blocks for ~10-50ms. All setter functions call `settings_save()` immediately. Never call setters from a callback or ISR — only from the main loop.

### I2C Bus Sharing

Touch (CST816T, 0x15) and haptic (DRV2605L, 0x5A) share the I2C bus. `haptic_play()` does 4 I2C writes. Calling haptic during a touch read can cause the next touch read to fail. The main loop handles this by reading touch first, then processing haptics.

---

## Phase 3 Scope (from FSD)

Phase 3 adds active attack features:
- **WiFi**: Deauthentication, Beacon Flood, Probe Sniffer, PMKID capture
- **BLE**: AppleJuice (fake AirPods popup), SwiftPair (fake Windows pairing), BLE spam
- **Active BLE**: Connection attempts, GATT enumeration

Each attack module should be a standalone `.cpp/.h` pair (like `wifi_scanner.cpp`), with a corresponding screen module in `src/screens/`. The navigation system handles everything else.

---

## Dependencies

```ini
platform = espressif32@6.6.0
lib_deps =
    lvgl/lvgl@~9.2.0
    h2zero/NimBLE-Arduino@^1.4.0
```

Pin these versions. See Phase 1 handover for LVGL and platform version warnings.
