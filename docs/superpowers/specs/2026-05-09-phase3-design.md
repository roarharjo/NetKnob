# Phase 3 Implementation Design — NetKnob

**Date:** 2026-05-09
**Author:** Roar Harjo + Claude
**Status:** Draft
**Scope:** Beacon Flood + Probe Sniffer (deauth descoped to Phase 4)

---

## 1. Overview

Phase 3 adds two WiFi attack/recon capabilities to NetKnob: a beacon flood that creates fake access points visible to nearby devices, and a probe sniffer that captures probe request frames revealing what networks nearby devices are searching for.

Deauth is descoped from Phase 3. The ESP32-S3 WiFi blob blocks deauth (0xC0) and disassoc (0xA0) frame types with hardened dual-layer validation that cannot be bypassed via `--wrap` or function override. Deauth moves to Phase 4 (secondary ESP32 with known working bypass).

**Spike validation results (2026-05-09):**
- Beacon TX via `esp_wifi_80211_tx()`: works (2980 frames, 0 errors)
- TX+RX coexistence: works (152 RX during 3940 TX, no time-division needed)
- Memory: 7.4 KB one-time init cost, 0 drift after (not a leak)
- Deauth: all three methods blocked on S3

---

## 2. Scope

### In scope
- Attack common module — state machine (IDLE → CONFIG → ARMED → RUNNING → COMPLETE), safety layer, timing
- Frame crafting — beacon frames (reusing spike-validated construction)
- Beacon flood engine — TX scheduling, SSID generation (random/wordlist/clone), round-robin
- Beacon flood screen — config, running, complete views with border glow
- Probe sniffer module — capture probe requests from existing promiscuous callback
- Probe sniffer screen — live scrolling list, detail view, channel control
- Navigation integration — new screens in WiFi group, new encoder modes, attack persistence
- Emergency stop — shake halts active attack from anywhere

### Out of scope
- Deauth attack, screen, frame crafting — blocked on S3, Phase 4
- Re-association detection — depends on deauth
- BLE attacks — Phase 3+ or later
- PMKID, Evil Portal, WPA handshake — Phase 3+ or later

---

## 3. File Structure

### New files
```
src/
├── attack_common.cpp / .h       ← State machine, safety layer, timing
├── wifi_attack.cpp / .h         ← Attack engine (beacon flood TX, frame crafting)
├── wifi_probe_sniffer.cpp / .h  ← Probe request capture + parse
└── screens/
    ├── scr_beacon_flood.cpp/.h  ← Beacon flood UI
    └── scr_probe_sniff.cpp/.h   ← Probe sniffer UI
```

### Modified files
- `src/navigation.h` — add `SCREEN_BEACON_FLOOD`, `SCREEN_PROBE_SNIFF` to ScreenId; add `ENC_TARGET_SELECT`, `ENC_ATTACK_PARAM` to EncoderMode
- `src/screens/scr_group_menu.cpp` — add beacon flood and probe sniffer to WiFi group
- `src/main.cpp` — register new screens, route encoder/touch/hold, add update calls
- `src/wifi_scanner.cpp` — extend promiscuous callback to forward probe requests to sniffer

---

## 4. Data Structures

### 4.1 Attack State Machine (attack_common.h)

```cpp
enum AttackType {
    ATTACK_NONE,
    ATTACK_BEACON_FLOOD,
    ATTACK_PROBE_SNIFF
};

enum AttackPhase {
    ATTACK_IDLE,
    ATTACK_CONFIG,
    ATTACK_ARMED,          // 1s countdown
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
    uint16_t     duration_sec;      // 0 = infinite
    uint16_t     tx_rate;           // packets per second

    // Beacon-specific
    uint8_t      ssid_count;
    uint8_t      ssid_source;       // 0=random, 1=wordlist, 2=clone
};
```

### 4.2 Beacon Templates (wifi_attack.h)

```cpp
#define BEACON_MAX_SSIDS         50
#define BEACON_DEFAULT_RATE      10
#define BEACON_MAX_RATE          500
#define ATTACK_DEFAULT_DURATION  30
#define ATTACK_COUNTDOWN_MS      1000
#define ATTACK_CONFIRM_HOLD_MS   1000
#define ATTACK_HAPTIC_INTERVAL   100

struct BeaconTemplate {
    uint8_t  frame[128];
    uint8_t  frame_len;
    uint16_t seq_number;
};
```

SSID wordlist (20 entries, from FSD):
```
Free WiFi, FBI Surveillance Van, Pretty Fly for a Wi-Fi,
Drop It Like Its Hotspot, The LAN Before Time,
Wu-Tang LAN, Router I Hardly Know Her,
Bill Wi the Science Fi, LAN Solo, The Promised LAN,
Nacho WiFi, Get Off My LAN, It Burns When IP,
No More Mr Wi-Fi, Silence of the LANs,
Loading..., Searching..., Connecting...,
Not Your WiFi, Virus Detected
```

### 4.3 Probe Sniffer (wifi_probe_sniffer.h)

```cpp
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
```

### 4.4 Navigation Enum Updates (navigation.h)

```cpp
enum ScreenId {
    // ... existing entries ...
    SCREEN_BEACON_FLOOD,
    SCREEN_PROBE_SNIFF,
    SCREEN_COUNT
};

enum EncoderMode {
    // ... existing entries ...
    ENC_TARGET_SELECT,
    ENC_ATTACK_PARAM,
    ENC_LOCKED              // already exists
};
```

---

## 5. Module APIs

### 5.1 attack_common

```cpp
void attack_init();
void attack_update();                    // Main loop — countdown, timeout, stats
void attack_start(AttackType type);      // → CONFIG
void attack_confirm();                   // CONFIG → ARMED
void attack_stop();                      // Any → COMPLETE (graceful)
void attack_emergency_stop();            // Any → IDLE (immediate, no results)
AttackState* attack_get_state();
bool attack_is_running();               // True if ARMED or RUNNING
```

Safety features:
- Touch hold (1s) required to start — no accidental taps
- 1s countdown in ARMED before TX begins
- Auto-timeout after configured duration
- Encoder locked during RUNNING (on attack screen only)
- Emergency stop via shake — immediate halt, return to main menu

### 5.2 wifi_attack

```cpp
void wifi_attack_init();
void wifi_attack_start_beacon_flood();   // Build frames, prepare TX
void wifi_attack_stop();                 // Stop TX
void wifi_attack_update();              // Main loop — send frames at rate
void wifi_attack_set_ssid_count(uint8_t count);
void wifi_attack_set_ssid_source(uint8_t source);  // 0=random, 1=wordlist, 2=clone
void wifi_attack_set_tx_rate(uint16_t rate);
void wifi_attack_set_duration(uint16_t seconds);
```

No LVGL dependency. All frames pre-built during CONFIG→ARMED transition. No dynamic allocation during RUNNING. Uses `esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false)`.

SSID generation modes:
- **Random:** 8-12 char alphanumeric SSIDs
- **Wordlist:** Cycle through 20 built-in funny names
- **Clone nearby:** Copy SSIDs from scanner AP list, randomize last 2 chars of BSSID

### 5.3 wifi_probe_sniffer

```cpp
void probe_sniffer_init();
void probe_sniffer_start();
void probe_sniffer_stop();
void probe_sniffer_update();             // Age entries, update unique counts
ProbeSnifferState* probe_sniffer_get_state();
```

Hooks into existing promiscuous callback. Filters probe request frames (frame control byte 0x40, subtype 0x04). Extracts source MAC, SSID, RSSI, channel. Detects randomized MACs via bit 1 of first byte. Writes to circular buffer (oldest overwritten).

### 5.4 Promiscuous callback integration

The existing `promisc_callback` in `wifi_scanner.cpp` filters for beacons (0x80) and probe responses (0x50). Extend it to also forward probe request frames (0x40) to a second ring buffer owned by `wifi_probe_sniffer.cpp`.

---

## 6. Screen Behavior

### 6.1 Beacon Flood Screen

**ScreenDef:**
```cpp
const ScreenDef scr_beacon_flood_def = {
    .name = "Beacon Flood",
    .group = GROUP_WIFI,
    .id = SCREEN_BEACON_FLOOD,
    .create = create,
    .show = show,
    .hide = hide,
    .destroy = NULL,        // retained
    .update = update,
    .enc_mode = ENC_ATTACK_PARAM
};
```

**CONFIG state:**
- Displays: channel, SSID count [1-50], source mode [Random/Wordlist/Clone], TX rate [1-100/s/AP], duration [5-300s, 0=infinite]
- Encoder CW/CCW adjusts selected parameter
- Touch tap cycles to next parameter
- Touch hold 1s → ARMED
- Shows calculated total TX rate preview

**ARMED state (1 second):**
- Visual countdown
- Haptic triple click
- Auto-transitions to RUNNING

**RUNNING state:**
- Magenta border glow (animated)
- Shows: SSIDs active, actual TX rate, packets sent, elapsed/total time, progress bar
- Encoder: ENC_LOCKED
- Touch hold 1s → COMPLETE (manual stop)
- Shake → emergency stop → main menu
- Haptic light tap every 100 packets

**COMPLETE state:**
- Shows: duration, total TX, avg rate, SSIDs, stop reason
- Touch tap → dismiss → back to CONFIG
- Touch hold 1s → run again with same params
- Haptic pulsing pattern

### 6.2 Probe Sniffer Screen

**ScreenDef:**
```cpp
const ScreenDef scr_probe_sniff_def = {
    .name = "Probe Sniffer",
    .group = GROUP_WIFI,
    .id = SCREEN_PROBE_SNIFF,
    .create = create,
    .show = show,
    .hide = hide,
    .destroy = NULL,        // retained
    .update = update,
    .enc_mode = ENC_CHANNEL_HOP    // reuses existing mode
};
```

**List view (default):**
- Status bar: channel mode (HOPPING/LOCKED CH N), total probes, device count, SSID count
- Scrolling list: `MAC:* → SSID` (newest at top)
- Randomized MACs dimmed
- Encoder CW/CCW hops channel (same as WiFi scanner)
- Touch tap selects entry
- Touch hold shows detail view
- Backspin → WiFi group menu

**Detail view:**
- Full MAC address
- MAC type: Real (OUI lookup) or Randomized
- Vendor (if real MAC)
- All SSIDs probed by this device
- RSSI range, first/last seen, probe count
- Touch tap → back to list

### 6.3 Attack Persistence

- Beacon flood continues running when user navigates away (backspin)
- WiFi group name shows indicator when attack active
- Return to beacon flood screen shows live stats
- Only shake (emergency stop) halts attack from outside the screen
- Probe sniffer stops capture when user leaves screen (no background capture needed)

---

## 7. Main Loop Integration

### New in setup()
```
attack_init();
wifi_attack_init();
probe_sniffer_init();
navigation_register_screen(&scr_beacon_flood_def);
navigation_register_screen(&scr_probe_sniff_def);
```

### New in loop()

**Always-run updates (after navigation_update):**
```
attack_update();
wifi_attack_update();
probe_sniffer_update();
```

**Encoder routing — add cases:**
```
case SCREEN_BEACON_FLOOD: scr_beacon_flood_on_encoder(delta); break;
case SCREEN_PROBE_SNIFF:  scr_probe_sniff_on_encoder(delta); break;
```

**Touch tap routing — add cases:**
```
case SCREEN_BEACON_FLOOD: scr_beacon_flood_on_tap(); break;
case SCREEN_PROBE_SNIFF:  scr_probe_sniff_on_tap(); break;
```

**Touch hold routing — add cases:**
```
case SCREEN_BEACON_FLOOD: scr_beacon_flood_on_hold(); break;
case SCREEN_PROBE_SNIFF:  scr_probe_sniff_on_hold(); break;
```

**Emergency stop extension:**
`navigation_emergency_stop()` additionally calls `attack_emergency_stop()`.

### WiFi group menu update (scr_group_menu.cpp)
```
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

---

## 8. Beacon Frame Construction

Validated in spike. 802.11 beacon frame, variable length (max ~128 bytes):

```
MAC header (24 bytes):
  Frame Control: 0x80 0x00 (beacon)
  Duration: 0x00 0x00
  Dest: FF:FF:FF:FF:FF:FF (broadcast)
  Source: random MAC per SSID (locally administered, unicast)
  BSSID: same as source
  Sequence Control: incrementing per-AP

Beacon body (12 bytes):
  Timestamp: 8 bytes (fake)
  Beacon Interval: 0x0064 (100 TU)
  Capability: 0x3104 (ESS + privacy)

Tagged parameters:
  Tag 0:  SSID (variable)
  Tag 1:  Supported Rates (8 standard b/g rates)
  Tag 3:  DS Parameter Set (channel)
  Tag 48: RSN IE (WPA2-PSK appearance, ~20 bytes)
```

Each fake AP gets a unique random MAC (generated once, reused for attack duration) and an incrementing sequence number.

---

## 9. Memory Budget

| Component | SRAM | Notes |
|-----------|------|-------|
| AttackState struct | ~3 KB | 50 SSIDs + metadata |
| Beacon templates (50) | ~7 KB | Pre-built frames |
| Probe sniffer buffer (100) | ~5 KB | Circular buffer |
| Beacon flood screen LVGL | ~3 KB | Retained |
| Probe sniffer screen LVGL | ~3 KB | Retained |
| **Phase 3 subtotal** | **~21 KB** | |
| Phase 1+2 existing | ~76 KB | |
| **Total** | **~97 KB** | |
| **Remaining** | **~215 KB** | 42% safety margin |

Plus 7.4 KB one-time WiFi TX init cost (absorbed on first beacon TX, not a leak).

---

## 10. Acceptance Criteria

### Beacon Flood
- [ ] AC-01: Beacon flood screen accessible via WiFi group menu
- [ ] AC-02: Encoder adjusts SSID count (1-50)
- [ ] AC-03: Three SSID source modes available (random, wordlist, clone)
- [ ] AC-04: Touch hold (1s) starts attack with 1-second countdown
- [ ] AC-05: During attack: magenta border glow, stats updating, encoder locked
- [ ] AC-06: External device (phone) sees fake SSIDs in WiFi settings
- [ ] AC-07: Attack auto-stops after configured duration
- [ ] AC-08: Touch hold (1s) during attack stops it manually
- [ ] AC-09: Shake gesture immediately stops attack from any state
- [ ] AC-10: Results screen shows total packets, duration, average rate
- [ ] AC-11: WiFi scanner data continues updating during beacon flood

### Probe Sniffer
- [ ] AC-12: Probe sniffer screen accessible via WiFi group menu
- [ ] AC-13: Captures probe requests from nearby devices in real-time
- [ ] AC-14: Displays source MAC (truncated) and probed SSID
- [ ] AC-15: Encoder CW/CCW hops channels (same behavior as WiFi scanner)
- [ ] AC-16: Touch tap selects entry, touch hold shows detail view
- [ ] AC-17: Detail view shows full MAC, vendor (OUI), all SSIDs probed
- [ ] AC-18: Randomized MACs identified and visually indicated
- [ ] AC-19: Circular buffer correctly wraps — oldest overwritten, newest shown
- [ ] AC-20: Stats bar shows total probes, unique devices, unique SSIDs

### Safety and Stability
- [ ] AC-21: Encoder locked during active attack — rotation does not navigate
- [ ] AC-22: Attack does not auto-resume after device reboot
- [ ] AC-23: Beacon flood runs for 5 minutes at maximum rate without crash
- [ ] AC-24: Start/stop beacon flood 50 times — no memory leak (heap stable)
- [ ] AC-25: Navigate away from attack screen and back — attack still running, stats correct
- [ ] AC-26: BLE scanner active during WiFi beacon flood — both functional
- [ ] AC-27: Heap monitoring shows no growth trend during 5-minute attack

### Integration
- [ ] AC-28: Both Phase 3 screens appear in WiFi group menu
- [ ] AC-29: Navigation (backspin) works correctly to/from all attack screens
- [ ] AC-30: Settings still accessible during active attack
- [ ] AC-31: WiFi region setting affects channel range for attack screens

---

## 11. Implementation Order

| Step | Deliverable | Validates | Blocks |
|------|-------------|-----------|--------|
| 1 | `attack_common.cpp/.h` | State machine, safety layer, timing | Steps 2-4, 8 |
| 2 | `wifi_attack.cpp/.h` — frame crafting + beacon TX | Correct frames, TX at rate | Steps 3-4 |
| 3 | Navigation updates — enums, group menu | New screens accessible | Steps 4, 6 |
| 4 | `scr_beacon_flood.cpp/.h` | Full UX: config → run → results | — |
| 5 | `wifi_probe_sniffer.cpp/.h` + callback extension | Probe capture to buffer | Step 6 |
| 6 | `scr_probe_sniff.cpp/.h` | Live display, detail view | — |
| 7 | Main loop integration | Routing, update calls | Step 8 |
| 8 | Attack persistence + emergency stop | Background attack, shake stops | — |
| 9 | Stability testing | All 31 acceptance criteria | — |

---

## 12. Known Limitations

| Limitation | Consequence | Accepted |
|------------|-------------|----------|
| No deauth on S3 | Deauth descoped to Phase 4 (secondary ESP32) | Yes |
| Beacon flood single channel | Fake APs only visible on one channel | Yes |
| 100-probe circular buffer | Old probes overwritten in busy environments | Yes |
| Probe sniffer sees randomized MACs | Cannot uniquely identify most modern phones | Yes |
| 7.4 KB one-time TX init cost | First beacon TX allocates WiFi driver buffers | Yes — not a leak |
