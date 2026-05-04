---
type: specification
project: NetKnob
phase: 1
status: draft
created: 2026-05-04
tags: [fsd, specification, esp32, wifi, scanner, phase-1, english]
---

# Functional Specification — NetKnob, Phase 1

> WiFi scanner with dial control on the Waveshare ESP32-S3 Knob

**Version:** 1.0 — Draft
**Date:** 2026-05-04
**Author:** Roar Harjo

---

## 1. Introduction

### 1.1 Purpose

This specification describes the complete functionality for Phase 1 of NetKnob: a WiFi channel scanner controlled by a rotary encoder on the Waveshare Knob platform. The document serves as the basis for implementation and acceptance testing.

### 1.2 Product Description

NetKnob Phase 1 is a new PlatformIO project (not a fork) that reuses verified drivers from `waveshare-knob` and adds WiFi scanning via ESP-IDF promiscuous mode. The user turns the dial to hop between WiFi channels and sees discovered access points in real time on the circular display.

### 1.3 Target Audience

The developer (Roar Harjo) — development, learning, and foundation for subsequent phases. Phase 1 validates the core architecture: promiscuous WiFi -> callback -> LVGL display -> encoder interaction.

### 1.4 Scope Exclusions

Phase 1 includes **only** passive WiFi scanning. The following are explicitly out of scope:

- Active attacks (deauth, beacon flood, probe sniffer)
- BLE scanning
- Secondary ESP32 (BT Classic, audio)
- Simultaneous multi-channel scanning
- Scan result storage/export (PCAP, JSON)
- Battery operation
- Navigation between multiple modes (single screen in Phase 1)

---

## 2. System Architecture

### 2.1 Hardware Platform

| Component | Specification | Status |
|-----------|---------------|--------|
| MCU | ESP32-S3, 240 MHz, 16 MB flash, 8 MB PSRAM | Verified |
| Display | ST77916 360x360 QSPI, circular | Verified |
| Encoder | Bidi switch, timer-polled at 3ms | Verified |
| Touch | CST816T I2C (address 0x15), pulsing INT pin | Verified |
| Haptic | DRV2605L LRA, I2C (address 0x5A) | Verified |
| WiFi | ESP32-S3 built-in, 2.4 GHz b/g/n | Unverified in promiscuous mode |

### 2.2 Software Stack

| Layer | Technology |
|-------|------------|
| Platform | PlatformIO, espressif32@6.6.0 |
| Framework | Arduino + ESP-IDF calls for WiFi promiscuous |
| UI | LVGL 9.2 with custom QSPI driver |
| WiFi | `esp_wifi_set_promiscuous()` + callback |
| Build | `pio run -e knob`, upload via COM9 (native USB) |

### 2.3 Project Structure

```
netknob/
├── platformio.ini
├── include/
│   └── board_pins.h              ← GPIO definitions (ported)
├── src/
│   ├── main.cpp                  ← Main loop, screen dispatch, encoder routing
│   ├── display.cpp / display.h   ← QSPI driver + screen layouts (ported + extended)
│   ├── encoder.cpp / encoder.h   ← Bidi switch driver (ported)
│   ├── touch.cpp / touch.h       ← CST816T with latch debounce (ported)
│   ├── haptic.cpp / haptic.h     ← DRV2605L LRA (ported)
│   ├── wifi_scanner.cpp / .h     ← NEW: promiscuous scan + beacon parser
│   └── interchip.h               ← Reserved: ESP-NOW message format (Phase 4)
└── reference/                    ← Reference code from waveshare-knob
```

### 2.4 Data Flow

```
Encoder CW/CCW
      │
      ▼
wifi_scanner_set_channel(ch)      ← Set new channel (1-13)
      │
      ▼
esp_wifi_set_channel(ch)          ← ESP-IDF channel switch
      │
      ▼
Promiscuous callback (350ms)      ← Captures beacon frames asynchronously
      │
      ▼
Beacon parser                     ← Extract SSID, BSSID, RSSI, encryption
      │
      ▼
AP list (sorted by RSSI)         ← Deduplicated, updated
      │
      ▼
display_wifi_scan()               ← LVGL render every 100ms on data change
      │
      ▼
Touch events                      ← Tap = select AP, hold = details
```

---

## 3. Functional Requirements

### FR-01: Channel Navigation via Encoder

| Property | Description |
|----------|-------------|
| **Trigger** | User turns the encoder (CW or CCW) |
| **Behavior** | Channel number changes by 1 per detent. CW = next channel, CCW = previous channel |
| **Range** | Channel 1-13 (ETSI). Wrapping: channel 13 + CW = channel 1, channel 1 + CCW = channel 13 |
| **Haptic** | Single click (DRV2605L effect) per channel change |
| **Constraint** | Encoder controls channel, NOT screen switching, when the scanner screen is active |

### FR-02: WiFi Scanning per Channel

| Property | Description |
|----------|-------------|
| **Trigger** | Channel change via encoder OR automatic on entering the scanner screen |
| **Method** | ESP-IDF `esp_wifi_set_promiscuous(true)` with callback |
| **Dwell time** | 350ms per channel. Configurable as constant |
| **Filter** | Management frames only (beacon/probe response) in promiscuous callback |
| **WiFi mode** | STA mode without connecting to any AP. No network connection in Phase 1 |
| **Indicator** | Visual "Scanning..." indicator with animated dots during dwell period |

### FR-03: Beacon Parsing

The promiscuous callback extracts the following per beacon frame:

| Field | Source | Format |
|-------|--------|--------|
| **SSID** | SSID IE (tag 0) from beacon body | String, max 32 chars. Empty for hidden networks |
| **BSSID** | MAC address from 802.11 header | 6 bytes, display: `XX:XX:XX:XX:XX:XX` |
| **RSSI** | `rx_ctrl.rssi` from promiscuous callback metadata | Signed integer, dBm (typical -30 to -90) |
| **Channel** | DS Parameter Set IE (tag 3) | Integer 1-13 |
| **Encryption** | RSN IE (tag 48) and/or WPA IE (vendor-specific) | Enum: `OPEN`, `WEP`, `WPA`, `WPA2`, `WPA3` |
| **Vendor** | First 3 bytes of BSSID (OUI) | String from static lookup table |

### FR-04: AP Deduplication

| Property | Description |
|----------|-------------|
| **Key** | BSSID (MAC address) |
| **Behavior** | If the same BSSID is captured multiple times during one dwell, keep the best (strongest) RSSI |
| **Max count** | 32 APs per channel. Excess discarded (weakest RSSI first) |
| **Lifetime** | AP list is cleared and rebuilt on each channel change |

### FR-05: AP List Display

| Property | Description |
|----------|-------------|
| **Layout** | Vertical list, up to 7 visible lines on 360px display |
| **Sorting** | By RSSI, strongest signal at top |
| **Per line** | Truncated SSID (left), RSSI bar (ASCII/graphic), dBm value (right) |
| **Color coding** | Green: RSSI > -50 dBm. Orange: -50 to -70 dBm. Red: < -70 dBm |
| **Hidden networks** | Shown as `[Hidden]` with BSSID instead of SSID |
| **Empty list** | Text: "No APs found on channel X" |

### FR-06: Channel Indicator

| Property | Description |
|----------|-------------|
| **Position** | Top of screen |
| **Content** | Channel number (large), frequency in MHz, number of APs found |
| **Activity gauge** | Circular LVGL arc that fills proportionally with AP count (0 APs = 2%, 20+ APs = 98%) |
| **Arc constraint** | Values clamped to 2-98 to avoid LVGL 9.2 crash at 0 and 100 |

### FR-07: AP Selection via Touch

| Property | Description |
|----------|-------------|
| **Tap** | Selects next AP in the list. Selected AP is visually highlighted (highlight/border) |
| **Cycle** | Tap cycles through the list circularly: last AP -> first AP |
| **Visual marking** | Selected AP has inverted background or visible border |

### FR-08: Detail View

| Property | Description |
|----------|-------------|
| **Trigger** | Touch hold (>1 second) on selected AP |
| **Layout** | Temporarily replaces the AP list. Full-screen view showing: |
| | - SSID (large font, centered) |
| | - MAC address (full `XX:XX:XX:XX:XX:XX`) |
| | - RSSI in dBm with color code |
| | - Channel number |
| | - Encryption type (OPEN/WEP/WPA/WPA2/WPA3) |
| | - Vendor name from OUI lookup |
| **Back** | Tap anywhere returns to the AP list |
| **Haptic** | Double click on entering detail view |

### FR-09: OUI Lookup

| Property | Description |
|----------|-------------|
| **Implementation** | Static table in code, ~20 common vendors |
| **Lookup** | First 3 bytes of BSSID matched against table |
| **Unknown** | Displayed as "Unknown" if OUI is not in table |

**Minimum table:**

| OUI Prefix | Vendor |
|------------|--------|
| `00:1A:2B` | Apple |
| `C8:2A:14` | Samsung |
| `B0:BE:76` | TP-Link |
| `F8:1A:67` | TP-Link |
| `AC:84:C6` | TP-Link |
| `20:A6:CD` | Netgear |
| `B4:FB:E4` | Ubiquiti |
| `FC:EC:DA` | Ubiquiti |
| `DC:9F:DB` | Ubiquiti |
| `78:8A:20` | Ubiquiti |
| `00:24:D7` | Intel |
| `34:02:86` | Intel |
| `E8:6F:38` | Cisco/Meraki |
| `00:18:74` | Cisco |
| `D8:B3:70` | Asus |
| `AC:9E:17` | Asus |
| `8C:AA:B5` | Samsung |
| `DC:A6:32` | Raspberry Pi |
| `28:6C:07` | Xiaomi |
| `64:CE:73` | Xiaomi |

> Table will be expanded as needed in later phases.

---

## 4. User Interaction — Summary

```
┌────────────────────────────────────────────────────────┐
│                    WIFI SCANNER                        │
│                                                        │
│  Encoder CW ──────── Next channel (wrap 13→1)         │
│  Encoder CCW ─────── Previous channel (wrap 1→13)     │
│  Touch tap ──────── Select next AP in list             │
│  Touch hold (1s) ── Show details for selected AP       │
│  Tap in detail mode ─ Back to AP list                 │
│                                                        │
│  Haptic:                                               │
│    Channel change ── Single click                      │
│    Detail view ───── Double click                      │
│                                                        │
└────────────────────────────────────────────────────────┘
```

---

## 5. Screen Design

### 5.1 Main Screen — AP List

```
     ╭──────────────────────╮
    ╱                        ╲
   │    Channel 6 — 2437 MHz  │
   │     ████████░░  12 APs    │   ← Activity gauge (arc)
   │  ─────────────────────    │
   │  ▶ Telenor-5G    ████ -42│   ← Selected AP (green)
   │    Get-Connect   ███  -58│   ← Orange
   │    WiFi-Guest    ██   -65│   ← Orange
   │    NETGEAR-1234  ██   -68│   ← Orange
   │    [Hidden]      █    -78│   ← Red
   │    TP-Link_AE2F  █    -82│   ← Red
   │    Telia-HomeWiFi ░   -88│   ← Red
    ╲                        ╱
     ╰──────────────────────╯
```

### 5.2 Detail View

```
     ╭──────────────────────╮
    ╱                        ╲
   │                          │
   │       Telenor-5G         │   ← SSID, large font
   │                          │
   │  MAC   C8:2A:14:0B:3E:91│
   │  RSSI  -42 dBm     [███]│   ← Color coded green
   │  CH    6                 │
   │  Enc   WPA2              │
   │  Vendor Samsung          │
   │                          │
   │       [ tap = back ]     │
    ╲                        ╱
     ╰──────────────────────╯
```

### 5.3 Scanning Indicator

```
     ╭──────────────────────╮
    ╱                        ╲
   │                          │
   │    Channel 6 — 2437 MHz  │
   │                          │
   │                          │
   │       Scanning...        │   ← Animated dots
   │                          │
   │                          │
    ╲                        ╱
     ╰──────────────────────╯
```

---

## 6. Data Structures

### 6.1 Access Point

```cpp
struct AccessPoint {
    char     ssid[33];           // 32 chars + null
    uint8_t  bssid[6];           // MAC address
    int8_t   rssi;               // Signal strength in dBm
    uint8_t  channel;            // WiFi channel (1-13)
    uint8_t  encryption;         // 0=OPEN, 1=WEP, 2=WPA, 3=WPA2, 4=WPA3
    bool     hidden;             // True if SSID is empty
};
```

### 6.2 Scanner State

```cpp
struct WifiScannerState {
    uint8_t       current_channel;       // 1-13
    AccessPoint   ap_list[32];           // Max 32 APs per channel
    uint8_t       ap_count;              // Number found
    uint8_t       selected_index;        // Selected AP in list
    bool          scanning;              // True during dwell period
    bool          detail_view;           // True in detail mode
    uint32_t      scan_start_ms;         // millis() at scan start
};
```

### 6.3 Encoder Mode

```cpp
enum EncoderMode {
    ENC_CHANNEL_HOP,      // Phase 1: encoder controls channel selection
    ENC_TARGET_SELECT,    // Phase 2+: encoder selects target
    ENC_SCREEN_SWITCH,    // Phase 2+: encoder switches screen
    ENC_LOCKED            // During active attack
};
```

### 6.4 Screen Enum (with reserved slots)

```cpp
enum Screen {
    SCREEN_WIFI_SCAN,
    // SCREEN_DEAUTH,        // Phase 2
    // SCREEN_BEACON_FLOOD,  // Phase 2
    // SCREEN_BLE_SCAN,      // Phase 3
    // SCREEN_BT_SCAN,       // Phase 4
    // SCREEN_AUDIO_MON,     // Phase 5
    // SCREEN_DUAL_STATUS,   // Phase 6
    // SCREEN_BOOT_MENU,     // Phase 7
    SCREEN_COUNT
};
```

### 6.5 Inter-Chip Message Format (reserved for Phase 4)

```cpp
// interchip.h — interface definition only, no implementation in Phase 1
struct EspNowMessage {
    uint8_t type;
    uint8_t data[32];
};
```

---

## 7. Non-Functional Requirements

### NFR-01: Performance

| Requirement | Value |
|-------------|-------|
| Display update | Every 100ms on data change |
| Encoder response time | <3ms (timer polling) |
| Dwell time per channel | 350ms (configurable constant) |
| Max latency from channel change to visual update | <500ms |

### NFR-02: Stability

| Requirement | Description |
|-------------|-------------|
| No crash on rapid channel hopping | Fast rotation 1->13->1 without crash |
| No crash on repeated detail view | Open/close details >50 times without crash |
| LVGL arc safety | All arc values clamped to 2-98 |
| Pointer safety | All static LVGL pointers nullified after `lv_obj_clean()` |

### NFR-03: Maintainability

| Requirement | Description |
|-------------|-------------|
| Modular architecture | `wifi_scanner` is a standalone module with no display dependency |
| Clear separation of concerns | Scanner produces data, display renders data |
| Configurable constants | Dwell time, max APs, channel range as `#define` |

---

## 8. Known Limitations

| Limitation | Consequence | Accepted? |
|------------|-------------|-----------|
| 2.4 GHz only | Cannot scan 5 GHz networks | Yes — most IoT networks are 2.4 GHz |
| One channel at a time | Does not show full picture across all channels | Yes — Phase 2 can add aggregated view |
| Promiscuous mode, not monitor mode | May miss some frames | Yes — beacons are sent frequently enough |
| OUI table limited to ~20 vendors | Many devices shown as "Unknown" | Yes — expanded as needed |
| No 5 GHz | Newer networks are 5 GHz primary | Yes — ESP32 hardware limitation |

---

## 9. Technical Pitfalls and Mitigations

| Pitfall | Source | Mitigation |
|---------|--------|------------|
| LVGL 9.2 arc crash at value 0 or 100 | Known LVGL bug | Clamp all arc values to 2-98 |
| Dangling label pointers on screen change | waveshare-knob experience | Null ALL static pointers after `lv_obj_clean()` |
| Touch INT pin pulses (not level) | CST816T hardware | Latch system with 150ms gap detection (ported from touch.cpp) |
| LVGL rendering blocks touch reads | Known pattern | Only render on data change, not every loop iteration |
| I2C bus sharing touch + haptic | Shared bus | No new I2C devices in Phase 1 — not an issue |
| Promiscuous callback in ISR context | ESP-IDF design | Callback only copies data to buffer, parsing happens in main loop |

---

## 10. Acceptance Criteria

Phase 1 is accepted when all of the following are met:

### 10.1 Core Functionality

- [ ] **AC-01:** Screen `SCREEN_WIFI_SCAN` is shown at startup
- [ ] **AC-02:** Encoder CW changes channel up (1->2->...->13->1)
- [ ] **AC-03:** Encoder CCW changes channel down (13->12->...->1->13)
- [ ] **AC-04:** Each channel change triggers a new 350ms dwell scan
- [ ] **AC-05:** APs appear on screen with SSID and RSSI within 500ms of channel change
- [ ] **AC-06:** RSSI color coding is correct: green >-50, orange -50 to -70, red <-70
- [ ] **AC-07:** AP list is sorted by RSSI (strongest at top)
- [ ] **AC-08:** Touch tap selects next AP in list (visual highlight)
- [ ] **AC-09:** Touch hold (>1s) opens detail view with MAC, channel, encryption, vendor
- [ ] **AC-10:** Tap in detail view returns to AP list

### 10.2 Stability

- [ ] **AC-11:** Rapid channel hopping (1->13->1 in <5 seconds) causes no crash
- [ ] **AC-12:** Repeated opening/closing of detail view (>20 times) without crash
- [ ] **AC-13:** No LVGL arc crash (values clamped to 2-98)
- [ ] **AC-14:** Serial output shows correct debug information for each scan

### 10.3 Preparation for Future Phases

- [ ] **AC-15:** `interchip.h` exists with `EspNowMessage` struct
- [ ] **AC-16:** Screen enum has commented reserved slots for Phases 2-7
- [ ] **AC-17:** Callback stub `on_secondary_esp_message()` exists in `main.cpp`

---

## 11. Sequence Diagrams

### 11.1 Channel Change and Scan

```
User            Encoder        wifi_scanner       ESP-IDF         Display
  │                │                │                │               │
  │──turn CW─────>│                │                │               │
  │                │──channel++───>│                │               │
  │                │  (haptic      │──set_channel──>│               │
  │                │   click)      │──promisc on───>│               │
  │                │               │                │──beacon──┐    │
  │                │               │                │          │    │
  │                │               │<──callback─────┤          │    │
  │                │               │  (copy to      │          │    │
  │                │               │   buffer)      │          │    │
  │                │               │                │──beacon──┤    │
  │                │               │<──callback─────┤          │    │
  │                │               │                │          │    │
  │                │               │ ── 350ms ──    │          │    │
  │                │               │                │          │    │
  │                │               │──promisc off──>│          │    │
  │                │               │──sort APs────┐ │               │
  │                │               │<─────────────┘ │               │
  │                │               │──ap_list──────────────────────>│
  │                │               │                │               │──render
  │<───────────────────────────────────────────────────────────────│
```

### 11.2 AP Selection and Detail View

```
User            Touch           Display          wifi_scanner
  │                │                │                │
  │──tap──────────>│                │                │
  │                │──touch_event──>│                │
  │                │               │──highlight      │
  │                │               │  next AP        │
  │                │               │                │
  │──hold (1s)────>│                │                │
  │                │──hold_event──>│                │
  │                │               │──get selected──>│
  │                │               │<──AP data──────│
  │                │               │──show details  │
  │<───────────────────────────────│                │
  │                │               │                │
  │──tap──────────>│                │                │
  │                │──touch_event──>│                │
  │                │               │──back to       │
  │                │               │  AP list       │
  │<───────────────────────────────│                │
```

---

## 12. Channel Frequencies (Reference)

| Channel | Frequency (MHz) |
|---------|-----------------|
| 1 | 2412 |
| 2 | 2417 |
| 3 | 2422 |
| 4 | 2427 |
| 5 | 2432 |
| 6 | 2437 |
| 7 | 2442 |
| 8 | 2447 |
| 9 | 2452 |
| 10 | 2457 |
| 11 | 2462 |
| 12 | 2467 |
| 13 | 2472 |

Channel range configurable via constant. Default: 1-13 (ETSI). FCC: 1-11. Japan: 1-14.

---

## 13. Changelog

| Date | Version | Change |
|------|---------|--------|
| 2026-05-04 | 1.0 | Initial draft based on NETKNOB-CONCEPT, PHASE1-FORPROSJEKT, PHASE1-PLAN and NAVIGASJON |
