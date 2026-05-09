# Spike Validation Design — NetKnob Phase 3 Pre-Validation

**Date:** 2026-05-09
**Author:** Roar Harjo + Claude
**Status:** Draft
**Purpose:** Validate ESP32-S3 WiFi TX capabilities before Phase 3 implementation

---

## 1. Overview

Phase 3 depends on `esp_wifi_80211_tx()` working for beacon and deauth frames, promiscuous RX continuing during TX, and memory remaining stable under TX load. This spike validates all four assumptions in an isolated test sketch before any Phase 3 code is written.

**If any test fails, the result informs Phase 3 architecture decisions:**
- TX+RX can't coexist → time-division multiplexing in attack engine
- Deauth direct injection blocked → rogue AP method becomes primary
- Memory leaks under TX → pre-allocated frame buffers, no dynamic allocation in TX path

---

## 2. File Structure

```
spike/
├── main.cpp              # Menu + test runner (single file)
├── platformio.ini        # Standalone build config
└── README.md             # How to run, what each test does
```

Standalone PlatformIO project inside the NetKnob repo. No dependencies on `src/` — only needs `espressif32` platform and ESP-IDF WiFi APIs. No LVGL, NimBLE, display, encoder, touch, or haptic.

**Removal plan:** Delete entire `spike/` folder once Phase 3 implementation begins and all four tests have documented pass/fail results.

### platformio.ini

Mirrors main project board config:
- Platform: espressif32 v6.6.0
- Board: esp32-s3-devkitc-1
- Flash: 16 MB QIO, OPI PSRAM
- Upload port: COM9
- Monitor speed: 115200
- No extra libraries needed

---

## 3. Serial Menu Interface

On boot, prints menu:

```
=================================
  NetKnob Phase 3 — Spike Validation
=================================
  [1] Beacon TX — Send 20 beacons, check phone WiFi list
  [2] TX+RX Coexistence — Beacon TX loop + promiscuous RX, count RX callbacks
  [3] Deauth Methods — Test all 3 methods sequentially
  [4] Memory Stability — 1000 beacon TX, measure heap drift
  [5] Run All (1-4 sequential)
  [R] Print results summary
=================================
>
```

### Flow per test

1. Print test name and description
2. Print "Starting in 3s..." (gives time to prepare — e.g., open phone WiFi list)
3. Run the test
4. Print result: `PASS` / `FAIL` / `INCONCLUSIVE` with details
5. Store result in `TestResult` struct array
6. Return to menu

### Results summary (`[R]`)

```
╔══════════════════════════════════════════════════════╗
║  TEST                  │ RESULT  │ DETAILS           ║
╠══════════════════════════════════════════════════════╣
║  1. Beacon TX          │ PASS    │ 20 frames sent    ║
║  2. TX+RX Coexistence  │ PASS    │ 847 RX during TX  ║
║  3a. Deauth: Direct    │ FAIL    │ blocked by driver  ║
║  3b. Deauth: Rogue AP  │ PASS    │ frames sent       ║
║  3c. Deauth: WSL patch │ FAIL    │ not available S3   ║
║  4. Memory Stability   │ PASS    │ drift: 128 bytes  ║
╚══════════════════════════════════════════════════════╝
```

"Run All" chains 1→2→3→4 with 3s pause between each, then auto-prints summary.

---

## 4. Configuration

Hardcoded `#define` block at top of `main.cpp`:

```cpp
// ---- SPIKE CONFIG ----
#define SPIKE_CHANNEL          6        // Channel for all tests
#define SPIKE_BEACON_COUNT     20       // SSIDs for test 1 & 2
#define SPIKE_BEACON_DURATION  15000    // ms — test 1 display window
#define SPIKE_COEXIST_DURATION 10000    // ms — test 2 TX+RX window
#define SPIKE_DEAUTH_COUNT     50       // Frames per deauth method
#define SPIKE_DEAUTH_DURATION  5000     // ms — per deauth sub-test
#define SPIKE_MEM_TX_COUNT     1000     // Frames for memory test
#define SPIKE_MEM_ROUNDS       3        // Repeat memory test N times

// ---- DEAUTH TARGET (edit before test 3) ----
#define TARGET_BSSID           {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
#define TARGET_CHANNEL         6
```

Target BSSID and channel edited before flashing for test 3. No runtime serial input for target selection — edit, flash, run.

Beacon SSIDs: first 20 from the FSD wordlist hardcoded in an array.

---

## 5. Test Implementations

### Test 1 — Beacon TX

**Goal:** Confirm `esp_wifi_80211_tx()` successfully sends beacon frames.

**Procedure:**
1. Init WiFi in STA mode + enable promiscuous mode
2. Set channel to `SPIKE_CHANNEL`
3. Generate 20 beacon frames — one per SSID from the FSD wordlist
4. Each frame: random BSSID, WPA2 RSN IE, DS parameter set matching channel
5. Loop for `SPIKE_BEACON_DURATION` (15s): send all 20 beacons every 100ms
6. Track `esp_wifi_80211_tx()` return values

**Pass criteria:** All calls return `ESP_OK`.

**Verification:** User checks phone WiFi list for fake SSIDs (manual, visual confirmation).

**Report:** Frames sent count, error count, list of any non-OK return codes.

### Test 2 — TX+RX Coexistence

**Goal:** Confirm promiscuous RX callback continues firing during TX loop.

**Procedure:**
1. WiFi in STA mode + promiscuous mode with RX callback registered
2. RX callback increments an atomic counter on every management frame received
3. Record RX counter before TX loop starts
4. Run beacon TX loop (20 SSIDs, ~10 beacons/sec per SSID) for `SPIKE_COEXIST_DURATION` (10s)
5. Record RX counter after TX loop ends
6. Calculate RX delta

**Pass criteria:** RX delta > 0 (at least some RX callbacks fired during TX).

**Report:** Total TX sent, total RX received during TX window, approximate RX rate (frames/sec).

### Test 3 — Deauth Methods

**Goal:** Determine which deauth delivery methods work on ESP32-S3.

Three sub-tests, each gets its own result row.

#### 3a — Direct Injection

1. Craft 26-byte deauth frame: frame control 0xC0 0x00, destination broadcast (or target BSSID), source spoofed to target BSSID
2. Reason code 7 (Class 3 frame from non-associated)
3. Send 50 frames over 5 seconds via `esp_wifi_80211_tx()`
4. Pass: `ESP_OK` returned (does not prove delivery — user verifies target device disconnects)
5. Report: frames sent, return codes, any errors

#### 3b — Rogue AP

1. `esp_wifi_set_mac(WIFI_IF_AP, target_bssid)` — clone target AP's BSSID
2. Start SoftAP on target channel with cloned BSSID
3. Send disassociation frame (0xA0) — less filtered than deauth
4. Send 50 frames over 5 seconds
5. Pass: SoftAP starts with spoofed MAC, TX returns `ESP_OK`
6. Report: MAC spoofing success, SoftAP status, frames sent, errors

#### 3c — WSL / libnet80211 Patch

1. Attempt to locate and bypass `ieee80211_raw_frame_sanity_check` internal function
2. This is architecture-dependent and expected to be unavailable on S3
3. If function not found at link time: report `NOT AVAILABLE`
4. If found: attempt bypass, send deauth frame, report result
5. Pass: no crash. Expected result: `NOT AVAILABLE`.
6. Report: function availability, bypass attempt result

After all three: print summary of which methods are available for Phase 3.

### Test 4 — Memory Stability

**Goal:** Confirm no memory leak during sustained TX.

**Procedure:**
1. Record `esp_get_free_heap_size()` before
2. Send 1000 beacon frames (single SSID, fast loop, no delay beyond TX time)
3. Record `esp_get_free_heap_size()` after
4. Repeat for `SPIKE_MEM_ROUNDS` (3 rounds)

**Pass criteria:** All round deltas < 1 KB.

**Report:** Heap before, heap after, drift per round, max drift across rounds.

---

## 6. Data Structures

```cpp
enum TestResult {
    RESULT_NOT_RUN,
    RESULT_PASS,
    RESULT_FAIL,
    RESULT_INCONCLUSIVE
};

struct SpikeResult {
    TestResult  result;
    char        details[64];    // Human-readable result detail
};

// 6 result slots: test 1, test 2, test 3a, 3b, 3c, test 4
SpikeResult results[6];
```

---

## 7. Beacon Frame Construction

Inline in `main.cpp` — not designed for reuse. Phase 3 proper will have `attack_common.cpp` with clean frame crafting functions.

```
802.11 Beacon Frame (variable length, max ~128 bytes):
  [2] Frame Control: 0x80 0x00
  [2] Duration: 0x00 0x00
  [6] Destination: FF:FF:FF:FF:FF:FF (broadcast)
  [6] Source: random MAC (unique per SSID)
  [6] BSSID: same as source
  [2] Sequence Control: incrementing
  [8] Timestamp: fake
  [2] Beacon Interval: 0x64 0x00 (100 TU)
  [2] Capability Info: 0x31 0x04 (ESS + privacy)
  Tagged Parameters:
    [tag 0]  SSID (variable length)
    [tag 1]  Supported Rates (8 bytes)
    [tag 3]  DS Parameter Set (1 byte — channel)
    [tag 48] RSN IE (~20 bytes — fake WPA2)
```

---

## 8. WiFi Initialization

```
1. esp_netif_init()
2. esp_event_loop_create_default()
3. esp_netif_create_default_wifi_sta()
4. esp_wifi_init() with default config
5. esp_wifi_set_mode(WIFI_MODE_STA)     — or APSTA for test 3b
6. esp_wifi_start()
7. esp_wifi_set_channel(SPIKE_CHANNEL)
8. esp_wifi_set_promiscuous(true)
9. esp_wifi_set_promiscuous_rx_cb(rx_callback)
```

For test 3b (rogue AP): switch to `WIFI_MODE_APSTA`, configure SoftAP with spoofed MAC, then switch back to STA after test completes.

---

## 9. Scope Exclusions

This spike does NOT include:
- Display, LVGL, or any UI
- Encoder, touch, haptic, BLE
- NVS or persistent settings
- FreeRTOS task-based TX scheduling (simple blocking loops suffice)
- Reusable frame construction abstractions
- Any code intended to survive into Phase 3

The spike is throwaway. The only deliverable is the results table documenting what works on the S3.

---

## 10. Decision Matrix

| Spike Result | Phase 3 Consequence |
|---|---|
| Test 1 PASS | Beacon flood uses `esp_wifi_80211_tx()` directly |
| Test 1 FAIL | Phase 3 blocked — investigate alternative TX method |
| Test 2 PASS | Scanner and attacks coexist naturally, no time-division needed |
| Test 2 FAIL | Implement time-division multiplexing (TX burst → RX window) in attack engine |
| Test 3a PASS | Direct injection is primary deauth method |
| Test 3a FAIL + 3b PASS | Rogue AP is primary deauth method |
| Test 3a FAIL + 3b FAIL + 3c PASS | WSL bypass is primary (fragile, document risks) |
| Test 3 all FAIL | Deauth feature descoped from Phase 3 or requires different hardware |
| Test 4 PASS | No special memory handling needed in TX path |
| Test 4 FAIL | All frames must be pre-allocated; investigate leak source before Phase 3 |
