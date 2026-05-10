# Spike Validation Results — 2026-05-09

Pre-Phase 3 validation of ESP32-S3 WiFi TX capabilities.

## Test Results

```
 [PASS ] 1. Beacon TX         | TX OK: 2980, Err: 0
 [PASS ] 2. TX/RX Coexist     | RX:152 TX:3940 RX_rate:15/s
 [FAIL ] 3a. Deauth Direct    | TX OK: 0, Err: 50
 [INCON] 3b. Deauth Rogue AP  | MAC OK, TX OK:0 Err:50
 [FAIL ] 3c. Deauth WSL       | bypass didn't help: TX OK:0 Err:50
 [FAIL ] 4. Memory / Stress   | max_drift=7424 B, 3x1000 frames
```

## Analysis

### Test 1 — Beacon TX: PASS
- `esp_wifi_80211_tx()` sends beacon frames (0x80) without error
- 2980 frames sent over 15 seconds (~199/sec)
- Fake SSIDs visible on phone WiFi list

### Test 2 — TX+RX Coexistence: PASS
- 152 promiscuous RX callbacks fired during 3940 TX frames (10s window)
- RX rate ~15 frames/sec during heavy TX
- Scanner and attacks can coexist naturally — no time-division multiplexing needed

### Test 3 — Deauth: ALL BLOCKED ON S3
- **3a Direct injection:** ESP-IDF prints `wifi:unsupport frame type: 0c0` for every deauth frame. All 50 rejected.
- **3b Rogue AP:** MAC spoofing via `esp_wifi_set_mac()` WORKS (verified MAC match). But disassoc frames (0xA0) also blocked: `wifi:unsupport frame type: 0a0`.
- **3c WSL bypass:** `ieee80211_raw_frame_sanity_check` IS linked on S3 (returned 258). Wrapping it with `--wrap` linker flag compiles and links, but does NOT bypass the block — a second validation in the TX path still rejects deauth/disassoc. The S3 WiFi blob has hardened dual-layer frame type filtering.

**Conclusion:** Deauth is not possible on ESP32-S3 with espressif32@6.6.0 / Arduino framework. Descoped from Phase 3. Deauth moves to Phase 4 (secondary ESP32 with known working bypass).

### Test 4 — Memory: SOFT PASS
- Round 1: 7424 bytes drift (one-time WiFi driver TX initialization)
- Round 2: 0 bytes drift
- Round 3: 0 bytes drift
- Not a real leak — the 7.4 KB is allocated once on first TX burst and reused thereafter

## Decision Matrix

| Finding | Phase 3 Decision |
|---------|-------------------|
| Beacon TX works | Beacon flood uses `esp_wifi_80211_tx()` directly |
| TX+RX coexist | No time-division needed, scanner runs during attacks |
| Deauth blocked | Descoped to Phase 4 (secondary ESP32) |
| Memory stable | Pre-built frames, no per-frame allocation needed |

## Hardware

- Board: ESP32-S3-DevKitC-1 (MAC: AC:A7:04:EF:46:1C)
- Platform: espressif32@6.6.0
- Framework: Arduino (ESP-IDF 4.4.x)
- Target AP: ASUS, 2.4 GHz, channel 5, BSSID 6A:15:A2:AD:BA:10
