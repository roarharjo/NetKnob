# Phase 3 Handover Guide

> For anyone picking up this codebase for Phase 4 development.
> Covers what was added, how the attack system works, and what to watch out for.

---

## What Phase 3 Added

### Beacon Flood
- Broadcasts fake AP beacons via `esp_wifi_80211_tx()`
- 3 SSID generation modes: random, wordlist (20 entries), clone from scanner
- Configurable: SSID count (1-50), TX rate (1-100/s per SSID), duration (5-300s or infinite)
- Pre-built frame templates — no allocation during TX
- Verified on hardware: 50 SSIDs, 5 minutes, ~500 pkt/s, no crash or memory leak

### Probe Sniffer
- Captures probe request frames (subtype 0x04) from the existing promiscuous callback
- Circular buffer of 100 entries with unique MAC and SSID counting
- Detail view with OUI vendor lookup, all SSIDs probed by a device
- Randomized MAC detection (bit 1 of first byte)

### Attack State Machine
- General-purpose lifecycle: IDLE → CONFIG → ARMED (1s countdown) → RUNNING → COMPLETE
- Lives in `attack_common.cpp` — shared by beacon flood, ready for future attacks
- Safety: touch-hold to start/stop, shake emergency stop, encoder lock during RUNNING
- Auto-timeout after configured duration
- Attack persists across navigation (backspin leaves attack running)

---

## Architecture

### New Modules

```
src/
├── attack_common.cpp/.h       ← State machine, safety, timing
├── wifi_attack.cpp/.h         ← Beacon flood engine, frame crafting
├── wifi_probe_sniffer.cpp/.h  ← Probe request capture + parse
└── screens/
    ├── scr_beacon_flood.cpp/.h
    └── scr_probe_sniff.cpp/.h
```

### Module Dependencies

```
main.cpp
  ├── attack_common        ← State machine (always updated in loop)
  ├── wifi_attack           ← TX engine (always updated in loop)
  ├── wifi_probe_sniffer   ← Capture (always updated in loop)
  ├── scr_beacon_flood     ← UI (encoder/tap/hold routed when active)
  └── scr_probe_sniff      ← UI (encoder/tap/hold routed when active)

wifi_attack depends on:
  ├── attack_common (reads/writes AttackState)
  └── wifi_scanner (reads channel, AP list for clone mode)

wifi_probe_sniffer depends on:
  └── wifi_scanner (called from promisc_callback for probe request frames)

scr_beacon_flood depends on:
  ├── attack_common (state machine control)
  ├── wifi_attack (start/stop beacon flood)
  └── wifi_scanner (read current channel)

scr_probe_sniff depends on:
  ├── wifi_probe_sniffer (start/stop, read state)
  └── wifi_scanner (channel hopping, OUI lookup)
```

### Main Loop Changes

```
loop()
  1. touch_read() + touch_update()
  2. gesture_update()
  3. if SHAKE → emergency_stop()     ← Now also calls attack_emergency_stop()
     if BACKSPIN → open_menu()
     else:
       route encoder/tap/hold to active screen
       (new cases: SCREEN_BEACON_FLOOD, SCREEN_PROBE_SNIFF)
  4. navigation_update()
  5. attack_update()                 ← NEW: countdown, timeout, stats
  6. wifi_attack_update()            ← NEW: send beacons if RUNNING
  7. probe_sniffer_update()          ← NEW: drain ring buffer
  8. lv_timer_handler()
  9. heap_monitor_update()
  10. auto-lock check
  11. serial heartbeat
```

### Navigation Hierarchy (Updated)

```
Main Menu (WiFi / BLE / System)
  ├─ WiFi Group
  │   ├─ Scanner
  │   ├─ Beacon Flood           ← NEW
  │   └─ Probe Sniffer          ← NEW
  ├─ BLE Group
  │   └─ BLE Scanner
  └─ System Group
      ├─ Settings
      └─ Debug
```

### Enum Updates

```cpp
enum ScreenId {
    // ... Phase 1-2 entries ...
    SCREEN_BEACON_FLOOD,     // Phase 3
    SCREEN_PROBE_SNIFF,      // Phase 3
    SCREEN_COUNT
};

enum EncoderMode {
    // ... Phase 1-2 entries ...
    ENC_ATTACK_PARAM,        // Phase 3: adjust attack parameters
    ENC_LOCKED               // Encoder ignored (during active attack)
};
```

---

## How the Attack System Works

### State Machine Flow

```
attack_start(type)        → CONFIG
  User adjusts params via encoder
attack_confirm()          → ARMED (1s countdown)
  attack_update() auto-transitions after ATTACK_COUNTDOWN_MS
                          → RUNNING
  wifi_attack_update() sends frames at configured rate
  attack_update() checks auto-timeout
attack_stop()             → COMPLETE (show results)
  or
attack_emergency_stop()   → IDLE (immediate, no results)
```

### Adding a New Attack Type

1. Add entry to `AttackType` enum in `attack_common.h`
2. Add attack-specific fields to `AttackState` struct (if needed)
3. Create engine module (e.g., `wifi_deauth.cpp/.h`) with `start()`, `stop()`, `update()`
4. Create screen module following `scr_beacon_flood.cpp` pattern
5. Add `ScreenId` entry, register screen, add routing in `main.cpp`
6. Add to WiFi group in `scr_group_menu.cpp`

The state machine, safety layer, and UI lifecycle pattern are all reusable — only the TX logic and config parameters change per attack type.

### Beacon Frame Construction

```
MAC header (24 bytes):
  FC 0x80 0x00 | Duration 0 | Dest broadcast | Src BSSID | BSSID | SeqCtrl

Body (12 bytes):
  Timestamp 0 | Interval 100 TU | Capability ESS+Privacy

Tags:
  0: SSID | 1: Rates | 3: DS channel | 48: RSN WPA2-PSK
```

Frames are pre-built in `wifi_attack_start_beacon_flood()` and stored in `BeaconTemplate` array. During RUNNING, `wifi_attack_update()` sends one frame per call in round-robin, incrementing the sequence number. No allocation in the TX path.

### TX Rate Calculation

`tx_rate` is per-SSID per-second. Total packets/sec = `tx_rate * ssid_count`. The update loop calculates `interval_ms = 1000 / total_pps` and sends one beacon per interval, cycling through SSIDs round-robin.

### Promiscuous Callback Extension

`wifi_scanner.cpp`'s `promisc_callback()` now has an early-return branch for probe requests:

```cpp
if (fc0 == 0x40) {
    probe_sniffer_on_frame(frame, frame_len, rssi, channel);
    return;
}
```

This fires before the beacon/probe-response filter. The sniffer uses its own ring buffer (8 slots, 128 bytes each) with the same ISR-safe pattern as the scanner.

---

## Spike Validation Results

Pre-Phase 3 testing confirmed:

| Test | Result | Detail |
|------|--------|--------|
| Beacon TX | PASS | `esp_wifi_80211_tx()` works for 0x80 frames |
| TX+RX Coexistence | PASS | Scanner RX continues during TX, no time-division needed |
| Deauth Direct | FAIL | S3 blob blocks 0xC0 frames |
| Deauth Rogue AP | FAIL | S3 blob also blocks 0xA0 (disassoc) |
| Deauth WSL Bypass | FAIL | `--wrap` bypasses one check, second check still blocks |
| Memory | PASS | 7.4 KB one-time init, 0 drift after |

Full results in `docs/spike-results.md`.

**Deauth conclusion:** ESP32-S3 with espressif32@6.6.0 cannot send deauth or disassoc frames. The WiFi blob has hardened dual-layer frame type validation. Deauth must use the secondary ESP32 (non-S3) in Phase 4.

---

## Memory Budget

```
Internal SRAM:  44.1% used (145 KB / 328 KB)
Flash:          37.5% used (1.25 MB / 3.3 MB)
After 5-min stress test: 93 KB free (stable, no drift)
```

Phase 3 added ~21 KB over Phase 2 baseline, plus 7.4 KB one-time WiFi TX init.

---

## Known Issues / Gotchas

### ESP32 Arduino float printf
`%f` and `%.1f` format specifiers don't work in `lv_label_set_text_fmt()` or `snprintf()` — ESP32 Arduino uses newlib-nano which strips float formatting. Cast to `(unsigned long)` and use `%lu` instead.

### TX+RX coexistence
Works naturally (no time-division needed), but RX rate drops during heavy TX (~15 RX/s during ~400 TX/s). Scanner data updates more slowly during active beacon flood. This is expected and acceptable.

### Probe sniffer unique counting
`recount_uniques()` is O(n^2) over the 100-entry buffer. Called every 50 new probes. Fine at current buffer size but would need optimization if buffer grows significantly.

### Attack persistence
Beacon flood continues running when navigating away. The attack state machine runs in the main loop regardless of active screen. Only shake (emergency stop) halts it from outside the beacon flood screen. There is no visual indicator in the group menu when an attack is running in background — this is a known gap from the spec.

### MAC spoofing works
`esp_wifi_set_mac(WIFI_IF_AP, target_bssid)` successfully clones BSSIDs on S3. This is useful for Phase 4 rogue AP features even though deauth frames are blocked.

---

## Phase 4 Considerations

### Secondary ESP32
The board has a second ESP32 (non-S3) on COM7. This chip:
- Can likely send deauth frames (original ESP32 has known working bypass)
- Has Bluetooth Classic (A2DP audio, HFP, SPP)
- Communicates with the S3 via ESP-NOW (`interchip.h` types already defined)

### Suggested Phase 4 Scope
- ESP-NOW inter-chip communication protocol
- Deauth via secondary ESP32 (S3 sends target info, ESP32 sends deauth frames)
- BT Classic scanning on secondary chip
- Potentially: audio monitoring via I2S on secondary chip

### What's Ready for Phase 4
- `interchip.h` defines ESP-NOW message types (stub, needs implementation)
- Attack state machine supports adding new `AttackType` entries
- `AttackState` struct can be extended with deauth-specific fields
- Screen pattern is established — new screens follow the same template
