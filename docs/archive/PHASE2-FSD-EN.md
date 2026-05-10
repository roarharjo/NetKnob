---
type: specification
project: NetKnob
phase: 2
status: draft
created: 2026-05-05
tags: [fsd, specification, esp32, navigation, ble, scanner, phase-2, english]
---

# Functional Specification — NetKnob, Phase 2

> Navigation system, BLE scanner, safe-lock, and settings on the Waveshare ESP32-S3 Knob

**Version:** 1.0 — Draft
**Date:** 2026-05-05
**Author:** Roar Harjo

---

## 1. Introduction

### 1.1 Purpose

This specification describes the complete functionality for Phase 2 of NetKnob: a navigation system capable of scaling to ~20 screens, a BLE scanner module, a safe-lock security screen, a settings interface, and heap monitoring infrastructure. The document serves as the basis for implementation and acceptance testing.

### 1.2 Product Description

Phase 2 extends the Phase 1 WiFi scanner with multi-screen navigation, a second radio module (BLE via NimBLE), device security (safe-lock), user-configurable settings, and runtime diagnostics. The navigation system is the architectural foundation — all subsequent phases depend on it.

### 1.3 Target Audience

The developer (Roar Harjo) — development, learning, and validation of the multi-screen architecture before WiFi attack features are added in Phase 3.

### 1.4 Scope Exclusions

Phase 2 includes **only** the items listed in this document. The following are explicitly out of scope:

- WiFi attacks (deauth, beacon flood, probe sniffer, PMKID) — Phase 3
- Active BLE attacks (AppleJuice, SwiftPair, BLE spam) — Phase 3+
- Secondary ESP32 integration (BT Classic, audio) — Phase 4
- Battery management and deep sleep — Phase 7
- Radar/compass UI visualization — Phase 7
- OTA firmware updates
- Data export (PCAP, JSON, CSV)

### 1.5 Prerequisites from Phase 1

The following Phase 1 open items must be resolved before or early in Phase 2 implementation:

| Item | Action Required | Reference |
|------|----------------|-----------|
| Encoder interrupt vs. timer-polling | Document the decision, add `encoder_get_velocity()` | PHASE1-AAPNE-PUNKT §1 |
| Touch INT-pin behavior | Verify latch implementation, test palm detection capability | PHASE1-AAPNE-PUNKT §2 |
| Heap monitoring | Add periodic logging from day 1 | PHASE1-AAPNE-PUNKT §3 |

Promiscuous + injection testing is deferred to Phase 3 (not relevant for Phase 2).

---

## 2. System Architecture

### 2.1 Hardware Platform

| Component | Specification | Status |
|-----------|---------------|--------|
| MCU | ESP32-S3, 240 MHz, 16 MB flash, 8 MB PSRAM | Verified |
| Display | ST77916 360x360 QSPI, circular | Verified |
| Encoder | Bidi switch, interrupt-driven (GPIO 7 CW, GPIO 8 CCW) | Verified |
| Touch | CST816T I2C (address 0x15), pulsing INT pin (GPIO 9) | Verified |
| Haptic | DRV2605L LRA, I2C (address 0x5A) | Verified |
| WiFi | ESP32-S3 built-in, 2.4 GHz b/g/n, promiscuous mode | Verified |
| BLE | ESP32-S3 built-in, BLE 5.0 via NimBLE | New in Phase 2 |
| NVS | ESP32-S3 flash partition, key-value store | New in Phase 2 |

### 2.2 Software Stack

| Layer | Technology |
|-------|------------|
| Platform | PlatformIO, espressif32@6.6.0 |
| Framework | Arduino + ESP-IDF calls for WiFi/BLE |
| UI | LVGL 9.2 with custom QSPI driver |
| WiFi | `esp_wifi_set_promiscuous()` + callback (Phase 1) |
| BLE | NimBLE-Arduino (passive scanning, no connection) |
| Storage | ESP-IDF NVS (Non-Volatile Storage) for settings and lock code |
| Build | `pio run -e knob`, upload via COM9 (native USB) |

### 2.3 Project Structure

```
netknob/
├── platformio.ini
├── include/
│   └── board_pins.h              ← GPIO definitions (ported)
├── src/
│   ├── main.cpp                  ← Main loop, screen dispatch, encoder routing
│   ├── display.cpp / display.h   ← QSPI driver + all screen layouts
│   ├── encoder.cpp / encoder.h   ← Bidi switch driver + velocity calculation
│   ├── touch.cpp / touch.h       ← CST816T with latch debounce
│   ├── haptic.cpp / haptic.h     ← DRV2605L LRA
│   ├── wifi_scanner.cpp / .h     ← Promiscuous scan + beacon parser (Phase 1)
│   ├── ble_scanner.cpp / .h      ← NEW: NimBLE passive scan + device list
│   ├── navigation.cpp / .h       ← NEW: Backspin/shake detection, menu state machine
│   ├── gesture.cpp / .h          ← NEW: Encoder velocity, backspin, shake detection
│   ├── safe_lock.cpp / .h        ← NEW: Combination lock logic + NVS hash storage
│   ├── settings.cpp / .h         ← NEW: Settings storage, read/write, defaults
│   ├── heap_monitor.cpp / .h     ← NEW: Periodic heap logging + thresholds
│   ├── interchip.h               ← Reserved: ESP-NOW message format (Phase 4)
│   └── screens/                  ← NEW: Screen-specific render functions
│       ├── scr_main_menu.cpp/.h  ← Group selection (WiFi / BLE / System)
│       ├── scr_group_menu.cpp/.h ← Screen selection within a group
│       ├── scr_wifi_scan.cpp/.h  ← WiFi scanner (refactored from display.cpp)
│       ├── scr_ble_scan.cpp/.h   ← BLE device list + detail view
│       ├── scr_safe_lock.cpp/.h  ← Safe-lock visual (dial ring, markers)
│       ├── scr_settings.cpp/.h   ← Settings list + value editors
│       └── scr_debug.cpp/.h      ← Heap/system info display
└── reference/                    ← Reference code from waveshare-knob
```

### 2.4 Screen Lifecycle

Phase 2 replaces the Phase 1 destroy/recreate pattern with a **retain-and-hide** model:

```
┌─────────────────────────────────────────────────────┐
│                SCREEN LIFECYCLE                       │
├─────────────────────────────────────────────────────┤
│                                                      │
│  First visit → lv_obj_create() → populate → show    │
│  Leave       → lv_obj_set_flag(HIDDEN)              │
│  Return      → lv_obj_clear_flag(HIDDEN) → update   │
│  Destroy     → lv_obj_del() (only for safe-lock     │
│                after unlock, debug on exit)           │
│                                                      │
└─────────────────────────────────────────────────────┘
```

Screens retained in memory: Main Menu, Group Menu, WiFi Scanner, BLE Scanner, Settings.
Screens destroyed after use: Safe-lock (after unlock), Debug/Heap (on exit).

### 2.5 Data Flow — Navigation

```
Encoder CW/CCW (interrupt)
      │
      ▼
gesture_update()              ← Calculate velocity, detect backspin/shake
      │
      ├── Normal speed ──────── Route to active screen (channel hop, list scroll, etc.)
      │
      ├── Backspin (fast CCW) ── navigation_open_menu()
      │                              │
      │                              ▼
      │                         Show menu overlay → encoder routes to menu navigation
      │
      └── Shake (3+ reversals) ── navigation_emergency_stop()
                                       │
                                       ▼
                                  Stop all operations, return to main menu
```

### 2.6 Data Flow — BLE Scanner

```
NimBLE scan start
      │
      ▼
BLE advertisement callback    ← Passive scan, no connection attempt
      │
      ▼
Parse advertisement data      ← Name, MAC, RSSI, device type, service UUIDs
      │
      ▼
Device list (sorted by RSSI)  ← Deduplicated by MAC, max 50 devices
      │
      ▼
scr_ble_scan_update()         ← LVGL render on data change
      │
      ▼
Touch events                  ← Tap = select, hold = details (same as WiFi)
```

---

## 3. Functional Requirements

### 3.1 Navigation System

#### FR-01: Backspin Gesture Detection

| Property | Description |
|----------|-------------|
| **Trigger** | Fast CCW rotation exceeding velocity threshold |
| **Detection method** | Encoder ISR timestamps → velocity calculation in gesture module |
| **Velocity threshold** | >20 steps/sec CCW (tunable, starting value) |
| **Acceleration requirement** | High initial acceleration followed by deceleration (flick profile) |
| **Quiet period** | 100ms of no encoder activity after the burst confirms intent |
| **Result** | Opens navigation menu overlay on current screen |
| **Haptic** | Medium buzz on menu open |
| **Constraint** | Must not trigger during normal fast CCW scrolling (sustained speed without deceleration) |

#### FR-02: Shake Gesture Detection (Emergency Stop)

| Property | Description |
|----------|-------------|
| **Trigger** | 3+ direction reversals within 500ms |
| **Detection method** | Track direction changes via encoder ISR; count reversals in sliding window |
| **Parameters** | `SHAKE_REVERSALS = 3`, `SHAKE_WINDOW_MS = 500` (tunable) |
| **Result** | Immediately stops all active operations (WiFi scan, BLE scan), returns to main menu |
| **Haptic** | Strong double-pulse (alarm pattern) |
| **Priority** | Highest — overrides all other encoder processing |
| **Constraint** | Must not trigger during normal back-and-forth adjustment |

#### FR-03: Palm Cover Detection (Stealth Mode)

| Property | Description |
|----------|-------------|
| **Trigger** | Large touch area detected for >1 second |
| **Detection method** | CST816T touch data — test whether large area is distinguishable from finger |
| **Result** | Disable all radios (WiFi + BLE), blank display |
| **Exit** | Remove palm — radios re-enable, display restores previous screen |
| **Haptic** | Single long pulse on enter, double tap on exit |
| **Status** | **Experimental** — depends on CST816T hardware capability. If touch area cannot be distinguished, fall back to alternative trigger (e.g., 3-finger touch or long press + encoder hold) |

#### FR-04: Main Menu (Group Selection)

| Property | Description |
|----------|-------------|
| **Entry** | Backspin from any screen, or boot (after unlock if enabled) |
| **Content** | Three groups: WiFi, BLE, System |
| **Navigation** | Encoder CW/CCW scrolls between groups |
| **Selection** | Touch tap selects highlighted group → opens group menu |
| **Visual** | Group icons/labels arranged vertically or radially on circular display |
| **Haptic** | Click per group change |
| **Memory** | Remembers last selected group between visits |

#### FR-05: Group Menu (Screen Selection)

| Property | Description |
|----------|-------------|
| **Entry** | Selecting a group from main menu |
| **Content** | List of screens in selected group |
| **Navigation** | Encoder CW/CCW scrolls between screens in group |
| **Selection** | Touch tap activates highlighted screen |
| **Back** | Backspin returns to main menu |
| **Haptic** | Click per screen change |
| **Memory** | Remembers last selected screen per group |

#### FR-06: Screen Groups

| Group | Screens (Phase 2) | Future Screens |
|-------|-------------------|----------------|
| **WiFi** | WiFi Scanner (P1) | Deauth, Beacon Flood, Probe Sniffer, PMKID (P3) |
| **BLE** | BLE Scanner (P2) | AppleJuice, SwiftPair, BLE Tracker (P3+) |
| **System** | Settings, Debug/Heap | About, Boot Config (P7) |

#### FR-07: Encoder Mode Routing

| Property | Description |
|----------|-------------|
| **Mechanism** | Active screen registers its encoder mode on activation |
| **Modes** | `ENC_CHANNEL_HOP` (WiFi), `ENC_BLE_LIST` (BLE), `ENC_MENU` (menus), `ENC_SAFE_LOCK` (lock), `ENC_SETTINGS` (settings), `ENC_LOCKED` (during active operation) |
| **Gesture layer** | Backspin and shake detection runs BEFORE mode-specific routing |
| **Constraint** | Encoder mode switches atomically with screen transition — no ambiguous state |

#### FR-08: Context Preservation

| Property | Description |
|----------|-------------|
| **Behavior** | Switching away from a screen preserves its state |
| **WiFi Scanner** | Remembers current channel, AP list, selected AP index |
| **BLE Scanner** | Remembers device list, selected device, scan active/paused |
| **Settings** | Remembers scroll position |
| **Implementation** | State stored in module-level structs; LVGL objects hidden, not destroyed |

#### FR-09: Navigation Depth Guarantee

| Property | Description |
|----------|-------------|
| **Maximum depth** | 3 interactions from any screen to any other screen |
| **Path** | Backspin → Main Menu → Select Group → Select Screen |
| **Direct return** | Backspin from group menu returns to main menu (not previous screen) |

---

### 3.2 Safe-Lock

#### FR-10: Lock Enable/Disable

| Property | Description |
|----------|-------------|
| **Default** | Lock disabled (device boots directly to main menu) |
| **Enable** | Via Settings → "Lock enabled" toggle |
| **When enabled** | Lock screen shown at boot and after auto-lock timeout |
| **Bypass** | No bypass — if code is forgotten, flash erase is required |

#### FR-11: Lock Combination Mechanism

| Property | Description |
|----------|-------------|
| **Type** | 3-digit combination, safe-dial style with alternating directions |
| **Digit range** | 0–39 (40 positions, like a physical combination lock) |
| **Sequence** | CW to first digit → CCW past first to second digit → CW past second to third digit → short CCW to open |
| **Input** | Pure encoder rotation — no touch required |
| **Tolerance** | Exact match required (no ±1 tolerance) |
| **Default code** | 0-0-0 (must be changed on first enable) |

#### FR-12: Lock Visual Design

| Property | Description |
|----------|-------------|
| **Layout** | Circular number ring (0–39) around display edge |
| **Marker** | Fixed indicator at top — ring rotates with encoder |
| **Direction arrow** | Shows expected rotation direction (CW or CCW) |
| **Progress** | Visual indicator showing which digit is being entered (1/3, 2/3, 3/3) |
| **Haptic** | Click per number (every detent), simulating safe detents |
| **Error** | Red flash + buzz on wrong code, escalating lockout (1s, 5s, 30s) |
| **Success** | Green flash + double-tap haptic, transition to main menu |

#### FR-13: Lock Code Storage

| Property | Description |
|----------|-------------|
| **Storage** | NVS key `lock_code_hash` |
| **Algorithm** | SHA-256 hash of the 3-digit code concatenated with a device-unique salt |
| **Salt** | Derived from ESP32 MAC address (chip-unique, not random) |
| **Verification** | Hash user input, compare with stored hash |
| **Never stored** | Plaintext code is never written to flash |

#### FR-14: Auto-Lock

| Property | Description |
|----------|-------------|
| **Trigger** | No encoder or touch activity for `lock_timeout` minutes |
| **Default timeout** | 5 minutes |
| **Configurable** | Via Settings → "Lock timeout" (0 = never auto-lock) |
| **Behavior** | Fade to lock screen, require full combination to unlock |
| **During active operation** | Auto-lock is suspended while WiFi scan or BLE scan is actively running |

#### FR-15: Lock Screen Destruction

| Property | Description |
|----------|-------------|
| **After unlock** | All lock screen LVGL objects are destroyed (`lv_obj_del`) |
| **Rationale** | Frees ~5-8 KB SRAM not needed until next lock event |
| **Re-creation** | On next lock trigger, objects are created fresh |

---

### 3.3 BLE Scanner

#### FR-16: BLE Passive Scanning

| Property | Description |
|----------|-------------|
| **Library** | NimBLE-Arduino |
| **Mode** | Passive scan — listen for advertisements, no connection attempts |
| **Scan type** | Active scan (sends scan request to get scan response data for device name) |
| **Duration** | Continuous until user exits BLE screen or triggers emergency stop |
| **Coexistence** | BLE and WiFi promiscuous run on independent radio paths on ESP32-S3 |
| **Memory** | NimBLE runtime allocated from PSRAM where possible (`CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y`) |

#### FR-17: BLE Device Parsing

The scan callback extracts the following per advertisement:

| Field | Source | Format |
|-------|--------|--------|
| **Device name** | Complete/Short Local Name AD type, or scan response | String, max 29 chars. Empty if not advertised |
| **MAC address** | Advertisement header | 6 bytes, display: `XX:XX:XX:XX:XX:XX` |
| **Address type** | BLE address type field | Public, Random, RPA (Resolvable Private Address) |
| **RSSI** | Scan result metadata | Signed integer, dBm |
| **Device type** | Appearance AD type or service UUIDs | Enum: Phone, Computer, Watch, Headphones, Speaker, Beacon, IoT, Unknown |
| **Service UUIDs** | 16-bit/128-bit service UUID AD types | List of known services (Heart Rate, Battery, HID, etc.) |
| **Manufacturer data** | Manufacturer Specific Data AD type | First 2 bytes = company ID (Apple=0x004C, Microsoft=0x0006, etc.) |
| **TX Power** | TX Power Level AD type | Signed integer, dBm (if advertised) |

#### FR-18: BLE Device Deduplication

| Property | Description |
|----------|-------------|
| **Key** | MAC address |
| **Behavior** | If same MAC seen again, update RSSI (running average) and last-seen timestamp |
| **Max count** | 50 devices in list. Excess discarded (weakest RSSI first) |
| **Aging** | Devices not seen for 60 seconds are dimmed. Removed after 120 seconds |
| **RPA handling** | Random addresses may rotate — devices with rotating MACs appear as new entries |

#### FR-19: BLE Device List Display

| Property | Description |
|----------|-------------|
| **Layout** | Vertical list, up to 7 visible lines on 360px display |
| **Sorting** | By RSSI, strongest signal at top |
| **Per line** | Device name or MAC (left), RSSI bar + dBm value (right) |
| **Color coding** | Green: RSSI > -50 dBm. Orange: -50 to -70 dBm. Red: < -70 dBm |
| **Unknown name** | Shown as `[Unknown]` with truncated MAC |
| **Device type icon** | Small icon prefix indicating device type (phone, computer, beacon, etc.) |
| **Scrolling** | Encoder CW/CCW scrolls the list when more than 7 devices |
| **Selection** | Touch tap selects next device in visible list (highlighted) |

#### FR-20: BLE Detail View

| Property | Description |
|----------|-------------|
| **Trigger** | Touch hold (>1 second) on selected device |
| **Layout** | Full-screen view showing: |
| | - Device name (large font, centered) |
| | - MAC address (full `XX:XX:XX:XX:XX:XX`) |
| | - Address type (Public / Random / RPA) |
| | - RSSI in dBm with color code |
| | - Device type |
| | - Service UUIDs (list, scrollable if >3) |
| | - Manufacturer (from company ID lookup) |
| | - TX Power (if available) |
| | - First seen / Last seen timestamps |
| **Back** | Tap anywhere returns to device list |
| **Haptic** | Double click on entering detail view |
| **Live RSSI** | RSSI value updates in real-time while in detail view |

#### FR-21: BLE Manufacturer Lookup

| Property | Description |
|----------|-------------|
| **Implementation** | Static table in code, ~25 common Bluetooth SIG company IDs |
| **Lookup** | First 2 bytes of manufacturer-specific data |
| **Unknown** | Displayed as hex company ID if not in table |

**Minimum table:**

| Company ID | Manufacturer |
|------------|-------------|
| `0x004C` | Apple |
| `0x0006` | Microsoft |
| `0x000F` | Broadcom |
| `0x000D` | Texas Instruments |
| `0x0059` | Nordic Semiconductor |
| `0x00E0` | Google |
| `0x0075` | Samsung |
| `0x0171` | Amazon |
| `0x0157` | Xiaomi |
| `0x038F` | Garmin |
| `0x0087` | Fitbit |
| `0x0310` | Tile |
| `0x02FF` | Bose |
| `0x012D` | Sony |
| `0x0269` | Sonos |
| `0x0499` | Ruuvi |
| `0x0822` | Govee |
| `0x0131` | Huawei |
| `0x0046` | MediaTek |
| `0x0002` | Intel |
| `0x000A` | Qualcomm |
| `0x0056` | Harman |
| `0x0094` | Bang & Olufsen |
| `0x0078` | Nike |
| `0x038B` | Espressif |

---

### 3.4 Settings

#### FR-22: Settings Screen Layout

| Property | Description |
|----------|-------------|
| **Location** | System group → Settings |
| **Layout** | Vertical list of setting name + current value pairs |
| **Navigation** | Encoder CW/CCW scrolls between settings |
| **Edit** | Touch tap on selected setting opens inline editor |
| **Back** | Backspin returns to System group menu |
| **Persistence** | All values written to NVS immediately on change |

#### FR-23: Settings List (Phase 2)

| Setting | Type | Default | Range | Key (NVS) |
|---------|------|---------|-------|------------|
| Lock enabled | Toggle | Off | On/Off | `lock_en` |
| Change code | Action | — | — | — |
| Lock timeout | Value (min) | 5 | 0–60 | `lock_tout` |
| WiFi region | Choice | ETSI | ETSI(1-13) / FCC(1-11) / Japan(1-14) | `wifi_rgn` |
| Display brightness | Value (%) | 80 | 10–100 | `disp_brt` |
| Haptic feedback | Toggle | On | On/Off | `haptic_en` |
| Haptic strength | Choice | Medium | Weak/Medium/Strong | `haptic_str` |
| Auto-scan on boot | Toggle | On | On/Off | `auto_scan` |
| Splash duration | Value (sec) | 2 | 0–5 | `splash_dur` |

#### FR-24: Settings Value Editing

| Type | Interaction | Visual |
|------|-------------|--------|
| **Toggle** | Touch tap toggles immediately | Switch icon flips On/Off |
| **Value** | Encoder CW/CCW adjusts value, touch tap confirms | Number with ↑↓ arrows |
| **Choice** | Encoder CW/CCW cycles options, touch tap confirms | Current option highlighted |
| **Action (Change code)** | Opens dedicated flow: verify old code → enter new code → confirm new code | Step-by-step with progress indicator |

#### FR-25: Settings Defaults and Reset

| Property | Description |
|----------|-------------|
| **First boot** | If NVS keys do not exist, use compiled defaults |
| **Validation** | On read, clamp values to valid range (protects against NVS corruption) |
| **No factory reset** | Not implemented in Phase 2. Flash erase is the reset mechanism |

---

### 3.5 Debug/Heap Screen

#### FR-26: Debug Screen Content

| Property | Description |
|----------|-------------|
| **Location** | System group → Debug |
| **Content** | Real-time system information |
| **Update rate** | Every 1 second |
| **Creation** | Created on first visit, destroyed on exit (on-demand lifecycle) |

**Displayed values:**

| Field | Source | Format |
|-------|--------|--------|
| Free internal SRAM | `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` | `XXX KB` |
| Free PSRAM | `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` | `X.X MB` |
| Largest free block | `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` | `XXX KB` |
| Minimum ever free | `esp_get_minimum_free_heap_size()` | `XXX KB` |
| Active screens | Count of non-destroyed screen objects | `N screens` |
| WiFi status | Promiscuous on/off, current channel | `CH 6 / ON` |
| BLE status | Scanning on/off, device count | `SCAN / 12 dev` |
| Uptime | `millis() / 1000` | `HH:MM:SS` |
| CPU temperature | Internal temp sensor (if available on S3) | `XX °C` |

---

### 3.6 Heap Monitoring

#### FR-27: Periodic Heap Logging

| Property | Description |
|----------|-------------|
| **Interval** | Every 10 seconds |
| **Output** | Serial (UART) |
| **Format** | `[HEAP] free=%d min=%d largest=%d psram=%d screens=%d` |
| **Overhead** | Negligible (<1ms per log call) |
| **Always active** | Runs regardless of which screen is active |

#### FR-28: Heap Threshold Alerts

| Level | Trigger | Action |
|-------|---------|--------|
| **Warning** | Free internal SRAM < 80 KB | Serial: `[HEAP WARNING] %d KB remaining` |
| **Critical** | Free internal SRAM < 30 KB | Serial: `[HEAP CRITICAL] %d KB remaining` + haptic alert (if on debug screen) |

#### FR-29: Heap Baseline Measurement

| Property | Description |
|----------|-------------|
| **When** | At boot (after all init), and after each screen create/destroy operation |
| **Log** | `[HEAP BASELINE] after=%s free=%d delta=%d` |
| **Purpose** | Track per-screen memory cost empirically |

---

## 4. User Interaction — Summary

```
┌─────────────────────────────────────────────────────────────────┐
│                    NAVIGATION GESTURES                            │
│                                                                   │
│  Encoder CW/CCW (slow) ── Screen-specific function               │
│  Encoder CCW (fast backspin) ── Open navigation menu             │
│  Encoder shake (3+ reversals) ── Emergency stop → main menu     │
│  Palm cover (1s) ── Stealth mode (experimental)                  │
│                                                                   │
│                    MENU NAVIGATION                                │
│                                                                   │
│  In Main Menu:                                                    │
│    Encoder CW/CCW ── Scroll groups (WiFi / BLE / System)         │
│    Touch tap ──────── Select group                               │
│                                                                   │
│  In Group Menu:                                                   │
│    Encoder CW/CCW ── Scroll screens                              │
│    Touch tap ──────── Activate screen                            │
│    Backspin ────────── Back to main menu                         │
│                                                                   │
│                    SAFE-LOCK                                      │
│                                                                   │
│  Encoder CW ───────── Rotate dial clockwise (digits 1, 3)       │
│  Encoder CCW ──────── Rotate dial counter-clockwise (digit 2)    │
│  (No touch required)                                              │
│                                                                   │
│                    BLE SCANNER                                    │
│                                                                   │
│  Encoder CW/CCW ──── Scroll device list                          │
│  Touch tap ─────────── Select next device                        │
│  Touch hold (1s) ──── Show device details                        │
│  Tap in detail ─────── Back to device list                       │
│                                                                   │
│                    SETTINGS                                       │
│                                                                   │
│  Encoder CW/CCW ──── Scroll settings list                        │
│  Touch tap ─────────── Edit selected setting                     │
│  (In edit mode:)                                                  │
│    Encoder CW/CCW ── Adjust value                                │
│    Touch tap ──────── Confirm change                             │
│  Backspin ──────────── Back to System group menu                 │
│                                                                   │
│                    HAPTIC SUMMARY                                 │
│                                                                   │
│  Encoder step ────────── Single click                            │
│  Menu open (backspin) ── Medium buzz                             │
│  Screen activate ─────── Double click                            │
│  Emergency stop ──────── Strong double-pulse                     │
│  Lock correct ────────── Double-tap                              │
│  Lock incorrect ──────── Long buzz                               │
│  Stealth enter ───────── Single long pulse                       │
│  Stealth exit ────────── Double tap                              │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 5. Screen Designs

### 5.1 Boot Flow

```
Power on
    │
    ▼
┌──────────────┐
│   SPLASH     │  ← Logo/name, duration from settings (default 2s)
│   NetKnob    │
└──────┬───────┘
       │
       ▼
   Lock enabled?
       │
  ┌────┴────┐
  │ YES     │ NO
  ▼         ▼
┌────────┐  ┌────────────┐
│ SAFE   │  │ MAIN MENU  │
│ LOCK   │  │            │
└───┬────┘  └────────────┘
    │
    ▼ (correct code)
┌────────────┐
│ MAIN MENU  │
└────────────┘
```

### 5.2 Safe-Lock Screen

```
     ╭──────────────────────╮
    ╱    ← Direction: CW →    ╲
   │  38 39  0  1  2  3  4  5  │   ← Number ring (0-39)
   │ 37                      6  │
   │ 36          ▼           7  │   ← Fixed marker at top
   │ 35                      8  │
   │ 34      [1 / 3]         9  │   ← Progress indicator
   │ 33                     10  │
   │ 32                     11  │
   │  31 30 29 28 27 ... 12    │
    ╲                        ╱
     ╰──────────────────────╯
```

### 5.3 Main Menu

```
     ╭──────────────────────╮
    ╱                        ╲
   │                          │
   │       ┌─────────┐       │
   │       │  WiFi   │       │   ← Currently selected (highlighted)
   │       └─────────┘       │
   │                          │
   │         BLE              │   ← Below: dimmed
   │                          │
   │         System           │   ← Below: dimmed
   │                          │
    ╲                        ╱
     ╰──────────────────────╯
         CW/CCW = scroll
         Tap = select
```

### 5.4 Group Menu (Example: WiFi Group)

```
     ╭──────────────────────╮
    ╱         WiFi            ╲
   │  ─────────────────────    │
   │                           │
   │  ▶ Scanner               │   ← Selected (highlighted)
   │    (future: Deauth)       │   ← Greyed out / not shown in P2
   │    (future: Beacon Flood) │
   │                           │
   │                           │
   │      ← backspin = back    │
    ╲                        ╱
     ╰──────────────────────╯
```

### 5.5 BLE Scanner — Device List

```
     ╭──────────────────────╮
    ╱      BLE Scanner        ╲
   │     Scanning... 23 devices │   ← Status bar
   │  ─────────────────────    │
   │  📱 iPhone (Roar)  ████ -38│   ← Green
   │  🎧 AirPods Pro    ███  -52│   ← Orange
   │  ▶ 💻 ThinkPad X1   ██  -61│   ← Selected (orange)
   │  📍 Tile Mate      ██   -67│   ← Orange
   │  ❓ [Unknown]      █    -75│   ← Red
   │  📍 Ruuvi E4:5A    █    -81│   ← Red
   │  🔊 JBL Flip 6     ░    -87│   ← Red (dimmed = stale)
    ╲                        ╱
     ╰──────────────────────╯
```

### 5.6 BLE Detail View

```
     ╭──────────────────────╮
    ╱                        ╲
   │                          │
   │     AirPods Pro          │   ← Name, large font
   │                          │
   │  MAC   4C:57:CA:1B:3E:A2│
   │  Type  Random (RPA)      │
   │  RSSI  -52 dBm     [███]│   ← Color coded orange
   │  Kind  Headphones        │
   │  Mfr   Apple             │
   │  Svc   HID, Battery      │
   │                          │
   │       [ tap = back ]     │
    ╲                        ╱
     ╰──────────────────────╯
```

### 5.7 Settings Screen

```
     ╭──────────────────────╮
    ╱        Settings         ╲
   │  ─────────────────────    │
   │  Lock enabled      [ OFF ]│
   │  ▶ WiFi region     [ETSI]│   ← Selected
   │  Display brightness  80%  │
   │  Haptic feedback   [ ON ] │
   │  Haptic strength  [Med]   │
   │  Auto-scan boot    [ ON ] │
   │  Splash duration    2s    │
   │                           │
    ╲   ← backspin = back    ╱
     ╰──────────────────────╯
```

### 5.8 Settings Value Editor (Inline)

```
     ╭──────────────────────╮
    ╱        Settings         ╲
   │  ─────────────────────    │
   │  Lock enabled      [ OFF ]│
   │                           │
   │  ┌─────────────────────┐ │
   │  │ WiFi region         │ │   ← Edit mode (highlighted box)
   │  │                     │ │
   │  │   ← ETSI →         │ │   ← Encoder scrolls options
   │  │                     │ │
   │  │    [tap = confirm]  │ │
   │  └─────────────────────┘ │
   │                           │
    ╲                        ╱
     ╰──────────────────────╯
```

### 5.9 Debug/Heap Screen

```
     ╭──────────────────────╮
    ╱      System Debug       ╲
   │  ─────────────────────    │
   │  SRAM Free:    247 KB     │
   │  SRAM Min:     231 KB     │
   │  Largest Blk:  198 KB     │
   │  PSRAM Free:   7.2 MB     │
   │  Screens:      5 active   │
   │  WiFi:    CH 6 / ON       │
   │  BLE:     SCAN / 23 dev   │
   │  Uptime:  00:14:32        │
   │                           │
    ╲   ← backspin = back    ╱
     ╰──────────────────────╯
```

---

## 6. Data Structures

### 6.1 Screen Management

```cpp
enum ScreenGroup {
    GROUP_WIFI,
    GROUP_BLE,
    GROUP_SYSTEM,
    GROUP_COUNT
};

enum Screen {
    SCREEN_WIFI_SCAN,       // Phase 1 (existing)
    SCREEN_BLE_SCAN,        // Phase 2
    SCREEN_SETTINGS,        // Phase 2
    SCREEN_DEBUG,           // Phase 2
    // SCREEN_DEAUTH,       // Phase 3
    // SCREEN_BEACON_FLOOD, // Phase 3
    // SCREEN_BLE_SPAM,     // Phase 3
    // SCREEN_BT_SCAN,      // Phase 4
    // SCREEN_AUDIO_MON,    // Phase 5
    SCREEN_COUNT
};

struct ScreenDef {
    const char*   name;          // Display name
    ScreenGroup   group;         // Which group this belongs to
    Screen        id;            // Enum ID
    void          (*create)();   // First-time creation function
    void          (*show)();     // Called when screen becomes active
    void          (*hide)();     // Called when screen is deactivated
    void          (*destroy)();  // Called to free memory (optional, NULL = retain)
    void          (*update)();   // Called each loop when active (live data)
    EncoderMode   enc_mode;      // Encoder behavior when this screen is active
};
```

### 6.2 Encoder and Gesture State

```cpp
enum EncoderMode {
    ENC_CHANNEL_HOP,      // WiFi scanner: encoder controls channel
    ENC_BLE_LIST,         // BLE scanner: encoder scrolls device list
    ENC_MENU,             // Navigation menus: encoder scrolls items
    ENC_SAFE_LOCK,        // Safe-lock: encoder rotates dial
    ENC_SETTINGS,         // Settings: encoder scrolls/adjusts values
    ENC_LOCKED            // During active operation — encoder ignored for nav
};

struct GestureState {
    // Velocity tracking
    uint32_t  delta_t_ring[8];      // Ring buffer of inter-pulse times (microseconds)
    uint8_t   ring_index;           // Current position in ring buffer
    float     velocity;             // Current velocity (steps/sec)
    float     peak_velocity;        // Peak velocity in current burst

    // Backspin detection
    bool      backspin_armed;       // True when fast CCW detected
    uint32_t  backspin_quiet_start; // millis() when fast burst ended
    uint8_t   ccw_burst_count;      // Consecutive fast CCW steps

    // Shake detection
    int8_t    last_direction;       // +1 CW, -1 CCW
    uint8_t   reversal_count;       // Direction changes in window
    uint32_t  reversal_timestamps[6]; // Timestamps of recent reversals
};
```

### 6.3 Navigation State

```cpp
struct NavigationState {
    Screen        active_screen;       // Currently displayed screen
    ScreenGroup   active_group;        // Currently active group
    uint8_t       group_selection;     // Selected index in main menu
    uint8_t       screen_selection[GROUP_COUNT]; // Selected index per group
    bool          menu_open;           // True when main/group menu is showing
    bool          stealth_mode;        // True when all radios disabled
    uint32_t      last_activity_ms;    // For auto-lock timeout
};
```

### 6.4 BLE Device

```cpp
struct BleDevice {
    uint8_t   mac[6];              // MAC address
    uint8_t   addr_type;           // 0=Public, 1=Random, 2=RPA
    char      name[30];            // Device name (29 chars + null)
    int8_t    rssi;                // Signal strength in dBm
    int8_t    rssi_avg;            // Running average RSSI
    int8_t    tx_power;            // TX Power Level (INT8_MIN if unavailable)
    uint8_t   device_type;         // Enum: Phone, Computer, Watch, etc.
    uint16_t  company_id;          // Manufacturer ID from BT SIG
    uint16_t  service_uuids[8];    // Up to 8 16-bit service UUIDs
    uint8_t   service_count;       // Number of service UUIDs discovered
    uint32_t  first_seen_ms;       // millis() at first detection
    uint32_t  last_seen_ms;        // millis() at last advertisement
    bool      stale;               // True if not seen for >60 seconds
};
```

### 6.5 BLE Scanner State

```cpp
struct BleScannerState {
    BleDevice   devices[50];         // Max 50 tracked devices
    uint8_t     device_count;        // Number of devices currently tracked
    uint8_t     selected_index;      // Selected device in list
    uint8_t     scroll_offset;       // First visible device in list
    bool        scanning;            // True when NimBLE scan is active
    bool        detail_view;         // True when showing device details
};
```

### 6.6 Safe-Lock State

```cpp
enum LockPhase {
    LOCK_DIGIT_1_CW,      // Rotating CW to first digit
    LOCK_DIGIT_2_CCW,     // Rotating CCW to second digit
    LOCK_DIGIT_3_CW,      // Rotating CW to third digit
    LOCK_OPEN_CCW,        // Short CCW to confirm/open
    LOCK_SUCCESS,         // Correct code entered
    LOCK_FAILED           // Wrong code
};

struct SafeLockState {
    uint8_t    current_position;     // 0-39, current dial position
    uint8_t    entered_digits[3];    // Digits entered so far
    LockPhase  phase;                // Current entry phase
    uint8_t    attempt_count;        // Failed attempts (for lockout)
    uint32_t   lockout_until_ms;     // millis() when lockout ends
};
```

### 6.7 Settings

```cpp
struct Settings {
    bool     lock_enabled;         // Safe-lock on/off
    uint8_t  lock_timeout_min;     // Auto-lock timeout (0 = never)
    uint8_t  wifi_region;          // 0=ETSI, 1=FCC, 2=Japan
    uint8_t  display_brightness;   // 10-100 (percent)
    bool     haptic_enabled;       // Haptic on/off
    uint8_t  haptic_strength;      // 0=Weak, 1=Medium, 2=Strong
    bool     auto_scan_on_boot;    // Start WiFi scan after unlock
    uint8_t  splash_duration_sec;  // 0-5 seconds
};
```

---

## 7. Non-Functional Requirements

### NFR-01: Performance

| Requirement | Value |
|-------------|-------|
| Backspin detection latency | <150ms from flick end to menu visible |
| Shake detection latency | <100ms from 3rd reversal to stop action |
| Screen transition time | <50ms (hide old + show new, no creation) |
| BLE list update rate | Every 500ms on data change |
| Settings save latency | <50ms (NVS write) |
| Encoder response time | <3ms (interrupt-driven) |
| Display update during scroll | Every 100ms maximum |

### NFR-02: Memory Budget

| Requirement | Value |
|-------------|-------|
| Total LVGL screen allocation (all retained) | <60 KB internal SRAM |
| BLE device list (50 devices) | <8 KB internal SRAM |
| NimBLE runtime | Allocated from PSRAM where possible |
| Minimum free internal SRAM | >80 KB at all times (warning threshold) |
| Critical threshold | >30 KB (hard floor) |

### NFR-03: Stability

| Requirement | Description |
|-------------|-------------|
| Navigation stress test | Backspin 50 times in rapid succession without crash |
| BLE + WiFi coexistence | Both radios scanning simultaneously for 30 minutes without crash |
| Screen cycling | Navigate through all screens 100 times without memory leak |
| Lock screen stress | Enter wrong code 20 times, correct code on 21st — no crash or lockout bypass |
| Auto-lock recovery | Lock triggers during active BLE scan — scan pauses, resumes after unlock |

### NFR-04: Security

| Requirement | Description |
|-------------|-------------|
| Lock code storage | SHA-256 hash only, never plaintext |
| Escalating lockout | 1s, 5s, 30s delays after 1st, 2nd, 3rd failed attempts |
| No settings behind lock | All settings require device to be unlocked first |
| Stealth mode | All radios confirmed off (verified via `esp_wifi_stop()` / NimBLE stop) |

### NFR-05: Maintainability

| Requirement | Description |
|-------------|-------------|
| Screen module independence | Each screen module has no dependencies on other screen modules |
| Gesture detection isolated | `gesture.cpp` knows nothing about UI — only outputs gesture events |
| Navigation decoupled from screens | `navigation.cpp` manages transitions; screens implement lifecycle interface |
| Tunable constants | All gesture thresholds as `#define` with documented valid ranges |
| NVS abstraction | `settings.cpp` is the only module that calls NVS API directly |

---

## 8. Known Limitations

| Limitation | Consequence | Accepted? |
|------------|-------------|-----------|
| BLE 5.0 extended advertising not parsed | Some modern devices may not show full data | Yes — standard advertising covers >95% of devices |
| RPA rotation creates duplicate entries | Privacy-enabled devices appear as new entries periodically | Yes — acceptable for passive recon |
| No BLE connection/pairing | Cannot enumerate GATT services beyond advertised UUIDs | Yes — Phase 3+ for active BLE |
| Palm detection may not work | CST816T may not distinguish palm from finger | Yes — fallback interaction defined |
| Lock code is 3 digits on 40 positions | 64,000 combinations — not cryptographically strong | Yes — physical access device, lockout escalation mitigates brute force |
| No OTA update | Must flash via USB for updates | Yes — Phase 7 |
| BLE scan stops WiFi channel-hopping | Single radio constraint on some operations | No — verify coexistence early |

---

## 9. Technical Pitfalls and Mitigations

| Pitfall | Source | Mitigation |
|---------|--------|------------|
| Backspin triggers during fast list scroll | Velocity threshold too low | Require acceleration profile (flick shape) + quiet period, not just speed |
| Shake triggers during backspin | Rapid direction change at end of flick | Shake requires 3+ full reversals; backspin is unidirectional burst |
| NimBLE + WiFi memory contention | Both stacks allocate from same heap | `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y`, measure at boot |
| LVGL objects accumulate in SRAM | Retain-and-hide keeps all objects alive | Budget verified: ~60KB for all P2 screens within 310KB available |
| NVS write during scan callback | NVS write blocks ~10-50ms | Only write settings from main loop, never from callbacks |
| Lock hash comparison timing attack | Comparing hashes byte-by-byte leaks timing info | Use constant-time comparison (`mbedtls_ct_memcmp` or manual XOR-accumulate) |
| CST816T ghost touches during palm cover | Large area may register as multiple taps | Debounce: ignore tap events when touch area exceeds threshold |
| Encoder bounce at high speed | False pulses at backspin velocities | 5ms debounce in ISR; verified sufficient for mechanical max |
| BLE scan saturates device list | Busy environment fills 50 slots quickly | Age-based eviction: devices not seen for 120s removed first, then weakest RSSI |
| Screen state stale after long absence | WiFi AP list outdated after visiting BLE for 5 minutes | On screen show: trigger fresh scan/refresh, keep stale data visible until new data arrives |

---

## 10. Acceptance Criteria

Phase 2 is accepted when all of the following are met:

### 10.1 Navigation System

- [ ] **AC-01:** Backspin (fast CCW flick) opens navigation menu from WiFi scanner screen
- [ ] **AC-02:** Backspin opens navigation menu from BLE scanner screen
- [ ] **AC-03:** Backspin from group menu returns to main menu
- [ ] **AC-04:** Shake gesture (3+ reversals in 500ms) triggers emergency stop and returns to main menu
- [ ] **AC-05:** Main menu shows three groups: WiFi, BLE, System
- [ ] **AC-06:** Encoder CW/CCW scrolls between groups in main menu
- [ ] **AC-07:** Touch tap in main menu enters selected group
- [ ] **AC-08:** Group menu shows available screens for selected group
- [ ] **AC-09:** Touch tap in group menu activates selected screen
- [ ] **AC-10:** Maximum 3 interactions from any screen to any other screen
- [ ] **AC-11:** Context preserved: leave WiFi scanner (CH 6, 12 APs), visit BLE, return to WiFi — still on CH 6 with AP list intact
- [ ] **AC-12:** Encoder mode switches correctly per active screen (channel hop in WiFi, list scroll in BLE, menu scroll in menus)

### 10.2 Safe-Lock

- [ ] **AC-13:** When lock enabled, lock screen appears after splash on boot
- [ ] **AC-14:** Correct 3-digit combination (alternating CW/CCW/CW) unlocks device
- [ ] **AC-15:** Incorrect combination shows error with haptic buzz
- [ ] **AC-16:** Escalating lockout: 1s after 1st fail, 5s after 2nd, 30s after 3rd
- [ ] **AC-17:** Lock code stored as SHA-256 hash in NVS (verified via NVS dump — no plaintext)
- [ ] **AC-18:** Auto-lock engages after configured timeout of inactivity
- [ ] **AC-19:** Auto-lock suspends during active scan operation
- [ ] **AC-20:** Lock screen LVGL objects destroyed after successful unlock (heap measurement confirms)

### 10.3 BLE Scanner

- [ ] **AC-21:** BLE scan discovers devices in range and displays them in a sorted list
- [ ] **AC-22:** Device list sorted by RSSI (strongest at top)
- [ ] **AC-23:** RSSI color coding matches WiFi scanner (green >-50, orange -50 to -70, red <-70)
- [ ] **AC-24:** Devices not seen for 60s are visually dimmed (stale)
- [ ] **AC-25:** Devices not seen for 120s are removed from list
- [ ] **AC-26:** Touch tap selects next device (visual highlight)
- [ ] **AC-27:** Touch hold (>1s) opens detail view with MAC, type, RSSI, services, manufacturer
- [ ] **AC-28:** Tap in detail view returns to device list
- [ ] **AC-29:** RSSI updates in real-time in both list and detail view
- [ ] **AC-30:** BLE scanner and WiFi scanner can both be active (verify coexistence)

### 10.4 Settings

- [ ] **AC-31:** Settings screen accessible via System → Settings
- [ ] **AC-32:** All 9 settings listed and editable
- [ ] **AC-33:** Toggle settings (lock, haptic, auto-scan) toggle on tap
- [ ] **AC-34:** Value settings (brightness, timeout, splash) adjustable with encoder, confirmed with tap
- [ ] **AC-35:** Choice settings (region, haptic strength) cycle with encoder, confirmed with tap
- [ ] **AC-36:** WiFi region change affects channel range (ETSI=1-13, FCC=1-11, Japan=1-14)
- [ ] **AC-37:** Display brightness change takes effect immediately
- [ ] **AC-38:** All settings persist across reboot (NVS verified)
- [ ] **AC-39:** Change code flow: verify old → enter new → confirm new (rejects mismatch)

### 10.5 Debug/Heap

- [ ] **AC-40:** Debug screen shows real-time heap values (free SRAM, PSRAM, largest block, minimum ever)
- [ ] **AC-41:** Debug screen shows active screen count, WiFi status, BLE status, uptime
- [ ] **AC-42:** Values update every 1 second
- [ ] **AC-43:** Debug screen objects destroyed on exit (heap measurement confirms)

### 10.6 Heap Monitoring

- [ ] **AC-44:** Serial output shows heap status every 10 seconds
- [ ] **AC-45:** Warning logged when free internal SRAM drops below 80 KB
- [ ] **AC-46:** Baseline measurement logged after each screen create/destroy
- [ ] **AC-47:** After 30 minutes of continuous use (navigating all screens, BLE+WiFi active), no memory leak (free heap does not monotonically decrease)

### 10.7 Stability

- [ ] **AC-48:** 50 consecutive backspin gestures without crash
- [ ] **AC-49:** Navigate through all screens 100 times without crash or memory leak
- [ ] **AC-50:** BLE + WiFi scanning simultaneously for 30 minutes without crash
- [ ] **AC-51:** Enter wrong lock code 20 times, then correct code — device unlocks normally
- [ ] **AC-52:** Rapid encoder rotation (full speed CW then immediate full speed CCW) does not trigger false backspin or shake

---

## 11. Sequence Diagrams

### 11.1 Backspin Navigation

```
User            Encoder       Gesture         Navigation      Display
  │                │              │                │              │
  │──fast CCW─────>│              │                │              │
  │                │──ISR×N──────>│                │              │
  │                │  (timestamps)│──velocity>20──>│              │
  │                │              │  ccw_burst     │              │
  │                │              │                │              │
  │  (stops)       │              │                │              │
  │                │  ──100ms──   │                │              │
  │                │              │──quiet_period──>│              │
  │                │              │  confirmed     │──show menu──>│
  │                │              │                │  (haptic)    │──render
  │<──────────────────────────────────────────────────────────────│
  │                │              │                │              │
  │──CW turn──────>│              │                │              │
  │                │──────────────────────────────>│              │
  │                │              │  (menu mode)   │──scroll      │
  │                │              │                │  group──────>│
  │<──────────────────────────────────────────────────────────────│
  │                │              │                │              │
  │──tap───────────────────────────────────────────>│              │
  │                │              │                │──activate    │
  │                │              │                │  screen─────>│
  │<──────────────────────────────────────────────────────────────│
```

### 11.2 Safe-Lock Unlock

```
User            Encoder       SafeLock        NVS             Display
  │                │              │              │               │
  │──CW to 15────>│              │              │               │
  │                │──delta──────>│              │               │
  │                │  (per click) │──position=15─────────────────>│ ring rotates
  │                │              │  phase=DIGIT_1│              │
  │  (stops)       │              │──digit[0]=15──┐              │
  │                │              │<──────────────┘              │
  │                │              │──show "2/3"──────────────────>│
  │──CCW to 28───>│              │              │               │
  │                │──delta──────>│──position=28─────────────────>│ ring rotates
  │                │              │──digit[1]=28──┐              │
  │                │              │<──────────────┘              │
  │                │              │──show "3/3"──────────────────>│
  │──CW to 7─────>│              │              │               │
  │                │──delta──────>│──position=7──────────────────>│ ring rotates
  │                │              │──digit[2]=7───┐              │
  │                │              │<──────────────┘              │
  │──short CCW───>│              │              │               │
  │                │──delta──────>│──verify──────>│              │
  │                │              │  hash(15,28,7)│              │
  │                │              │<──match!──────│              │
  │                │              │──unlock───────────────────────>│ success anim
  │                │              │  (haptic                     │
  │                │              │   double-tap)                │
  │                │              │──destroy lock screen         │
  │                │              │──nav: show main menu─────────>│
  │<──────────────────────────────────────────────────────────────│
```

### 11.3 BLE Device Discovery

```
NimBLE          ble_scanner       Display         User
  │                │                │               │
  │──adv callback──>│                │               │
  │  (raw adv data)│──parse──┐      │               │
  │                │<────────┘      │               │
  │                │──dedup/update   │               │
  │                │  device list    │               │
  │                │                │               │
  │──adv callback──>│                │               │
  │                │──parse + update │               │
  │                │                │               │
  │  ─── 500ms ─── │                │               │
  │                │──if changed────>│               │
  │                │                │──render list  │
  │                │                │<──────────────│
  │                │                │               │──sees list
  │                │                │               │
  │                │                │               │──hold touch
  │                │                │<──hold event──│
  │                │<──get device───│               │
  │                │──device data──>│               │
  │                │                │──render       │
  │                │                │  detail───────>│
```

### 11.4 Emergency Stop (Shake)

```
User            Encoder       Gesture         Navigation      WiFi/BLE
  │                │              │                │              │
  │──CW─────────>│──ISR─────────>│ dir=CW         │              │
  │──CCW────────>│──ISR─────────>│ reversal #1    │              │
  │──CW─────────>│──ISR─────────>│ reversal #2    │              │
  │──CCW────────>│──ISR─────────>│ reversal #3    │              │
  │                │              │  (3 in <500ms) │              │
  │                │              │──SHAKE!────────>│              │
  │                │              │                │──stop_all───>│
  │                │              │                │  wifi_stop() │
  │                │              │                │  ble_stop()  │
  │                │              │                │              │
  │                │              │                │──show main   │
  │                │              │                │  menu        │
  │                │              │  (strong       │              │
  │                │              │   haptic)      │              │
  │<──────────────────────────────────────────────────────────────│
```

### 11.5 Settings Edit (Value Type)

```
User            Encoder       Settings        NVS             Display
  │                │              │              │               │
  │  (settings screen active, "Brightness" selected)            │
  │──tap───────────────────────────>│              │               │
  │                │              │──enter edit mode──────────────>│ show editor
  │                │              │              │               │
  │──CW (+5)─────>│──delta──────>│              │               │
  │                │              │──brightness=85────────────────>│ update value
  │──CW (+5)─────>│──delta──────>│              │               │
  │                │              │──brightness=90────────────────>│ update value
  │                │              │              │               │
  │──tap (confirm)──────────────────>│              │               │
  │                │              │──write───────>│              │
  │                │              │  "disp_brt"=90│              │
  │                │              │<──ok──────────│              │
  │                │              │──apply_brightness(90)        │
  │                │              │──exit edit mode───────────────>│ back to list
  │<──────────────────────────────────────────────────────────────│
```

---

## 12. Tunable Constants

All gesture and navigation constants are `#define` in their respective header files. These are starting values that require tuning on the physical device.

| Constant | File | Default | Range | Effect |
|----------|------|---------|-------|--------|
| `DEBOUNCE_US` | `encoder.h` | 5000 | 2000–10000 | Minimum microseconds between valid pulses |
| `BACKSPIN_MIN_VELOCITY` | `gesture.h` | 20 | 15–30 | Steps/sec threshold for backspin (CCW only) |
| `FLICK_ACCEL_THRESHOLD` | `gesture.h` | TBD | — | Acceleration needed to qualify as flick |
| `BACKSPIN_QUIET_MS` | `gesture.h` | 100 | 50–200 | Silence after burst before confirming backspin |
| `BACKSPIN_MIN_STEPS` | `gesture.h` | 3 | 2–5 | Minimum CCW steps in burst |
| `SHAKE_REVERSALS` | `gesture.h` | 3 | 2–4 | Direction changes needed for emergency stop |
| `SHAKE_WINDOW_MS` | `gesture.h` | 500 | 300–800 | Time window for counting reversals |
| `VELOCITY_RING_SIZE` | `gesture.h` | 4 | 3–8 | Number of samples for velocity smoothing |
| `BLE_SCAN_INTERVAL_MS` | `ble_scanner.h` | 100 | 50–500 | NimBLE scan interval |
| `BLE_SCAN_WINDOW_MS` | `ble_scanner.h` | 80 | 30–100 | NimBLE scan window (≤ interval) |
| `BLE_MAX_DEVICES` | `ble_scanner.h` | 50 | 20–100 | Maximum tracked BLE devices |
| `BLE_STALE_MS` | `ble_scanner.h` | 60000 | 30000–120000 | Time before device is marked stale |
| `BLE_REMOVE_MS` | `ble_scanner.h` | 120000 | 60000–300000 | Time before device is evicted |
| `LOCK_POSITIONS` | `safe_lock.h` | 40 | — | Number of positions on the dial (0–39) |
| `LOCK_DIGITS` | `safe_lock.h` | 3 | — | Number of digits in combination |
| `LOCKOUT_DELAYS_MS` | `safe_lock.h` | {1000, 5000, 30000} | — | Escalating lockout durations |
| `AUTO_LOCK_DEFAULT_MIN` | `settings.h` | 5 | 0–60 | Default auto-lock timeout |
| `HEAP_LOG_INTERVAL_MS` | `heap_monitor.h` | 10000 | 5000–60000 | Heap logging frequency |
| `HEAP_WARN_THRESHOLD` | `heap_monitor.h` | 81920 | — | Warning at <80 KB free SRAM |
| `HEAP_CRITICAL_THRESHOLD` | `heap_monitor.h` | 30720 | — | Critical at <30 KB free SRAM |

---

## 13. Memory Budget — Phase 2

### 13.1 Estimated System Baseline

| Component | Internal SRAM | PSRAM |
|-----------|--------------|-------|
| FreeRTOS + task stacks | ~30 KB | — |
| WiFi stack (promiscuous) | ~60–80 KB | — |
| NimBLE (BLE scanning) | ~30 KB IRAM + ~14 KB DRAM | ~88 KB |
| LVGL framebuffer (double) | — | ~506 KB |
| LVGL core + drivers | ~32–44 KB | — |
| **System subtotal** | **~170–200 KB** | **~600 KB** |

### 13.2 Phase 2 Application Memory

| Component | Internal SRAM | Notes |
|-----------|--------------|-------|
| LVGL screens (all retained) | ~55 KB | Main menu + groups + WiFi + BLE + Settings |
| WiFi AP list (32 structs) | ~5 KB | Existing from Phase 1 |
| BLE device list (50 structs) | ~8 KB | New |
| Gesture state + ring buffers | ~1 KB | New |
| Navigation state | <1 KB | New |
| NVS settings cache | ~2 KB | New |
| Serial logging buffer | ~4 KB | New |
| **Application subtotal** | **~76 KB** | |

### 13.3 Budget Summary

```
Total internal SRAM:           512 KB
System baseline:              ~200 KB
Application (Phase 2):         ~76 KB
                              ────────
Estimated remaining:          ~236 KB
Safety margin:                 ~46% free

PSRAM total:                    8 MB
System (framebuffer + BLE):   ~700 KB
Remaining:                    ~7.3 MB (comfortable)
```

### 13.4 Risk Mitigation

1. NimBLE configured to allocate from PSRAM (`CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y`)
2. Safe-lock screen destroyed after unlock (frees ~5–8 KB)
3. Debug screen created/destroyed on demand (not retained)
4. Heap monitoring validates these estimates empirically from day 1

---

## 14. Implementation Order

The following order minimizes integration risk and provides testable milestones:

| Step | Deliverable | Validates |
|------|-------------|-----------|
| 1 | Heap monitoring (`heap_monitor.cpp`) | Baseline measurements, logging infrastructure |
| 2 | Encoder velocity + gesture detection (`gesture.cpp`) | Backspin/shake detection without UI |
| 3 | Screen lifecycle framework (`ScreenDef`, create/show/hide/destroy) | Retain-and-hide architecture |
| 4 | Navigation state machine (`navigation.cpp`) | Menu transitions, encoder routing |
| 5 | Main menu + group menu screens | Visual navigation working end-to-end |
| 6 | Refactor WiFi scanner into screen module | Existing functionality in new architecture |
| 7 | Settings module + NVS (`settings.cpp`) | Persistence, value editing |
| 8 | Settings screen (`scr_settings.cpp`) | UI for settings |
| 9 | Safe-lock logic + screen | Lock/unlock flow |
| 10 | BLE scanner module (`ble_scanner.cpp`) | NimBLE integration, device parsing |
| 11 | BLE scanner screen (`scr_ble_scan.cpp`) | UI, coexistence with WiFi |
| 12 | Debug screen (`scr_debug.cpp`) | System visibility |
| 13 | Palm cover / stealth (experimental) | Hardware capability test |
| 14 | Integration testing + gesture tuning | All acceptance criteria |

---

## 15. Changelog

| Date | Version | Change |
|------|---------|--------|
| 2026-05-05 | 1.0 | Initial draft based on PHASE2-STARTUP, NAVIGASJON, PHASE1-FSD-EN, and PHASE1-AAPNE-PUNKT |
