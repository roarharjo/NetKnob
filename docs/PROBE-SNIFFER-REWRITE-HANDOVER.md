# Probe Sniffer Rewrite — Implementation Handover

> Device-centric rewrite of the Phase 3 probe sniffer (Phase 3.1).
> Covers what changed, how the new data model works, and UI architecture.

---

## What Changed

### Before (Phase 3)
- Flat 100-entry circular buffer of `ProbeRequest` structs
- Event-log UI: 7 scrollable rows showing individual probe events
- O(n²) `recount_uniques()` sweep every 50 probes
- Encoder bound to channel hop (decorative, no practical use)
- Screen stopped capture on hide, restarted on show

### After (Phase 3.1)
- Device table: 32 real-MAC entries with per-device SSID lists (16 SSIDs each)
- Randomized-MAC aggregate: single record collapsing all randomized MACs
- Three-view UI: device list → device detail → randomized detail
- Encoder scrolls cursor through device list
- Capture persists across navigation (only shake stops it)
- Pause-while-running (hold gesture)

---

## Architecture

### Data Flow

```
802.11 RX (ISR context)
   │
   ▼
probe_sniffer_on_frame()          ← IRAM_ATTR, writes to ISR ring buffer
   │
   ▼  (main loop drains ring)
probe_sniffer_update()
   │
   ▼
parse_probe_request()
   │
   ├─ randomized MAC? → randomized_aggregate_update()
   └─ real MAC?       → find_or_create_device() → device_update()
                                                      └─ ssid_list_update()
```

### Files Changed

| File | Change | Size |
|------|--------|------|
| `src/wifi_probe_sniffer.h` | Rewritten | New data structures |
| `src/wifi_probe_sniffer.cpp` | Rewritten | Aggregation engine |
| `src/screens/scr_probe_sniff.cpp` | Rewritten | 3-view UI (~830 lines) |
| `src/navigation.cpp` | +2 lines | `probe_sniffer_stop()` in emergency stop |

No other files changed. The callback signature (`probe_sniffer_on_frame`) is preserved — `wifi_scanner.cpp` calls it identically.

---

## Data Model

### DeviceEntry (real-MAC devices)

```cpp
struct DeviceEntry {
    uint8_t      mac[6];           // Source MAC (unique key)
    char         vendor[20];       // OUI lookup result
    int8_t       rssi_last/min/max;
    uint16_t     probe_count;      // Total probes from this device
    uint8_t      ssid_count;       // Distinct SSIDs probed (max 16)
    DeviceSSID   ssids[16];        // Per-SSID probe count + last_seen
    uint32_t     first_seen_ms;
    uint32_t     last_seen_ms;
    uint8_t      last_channel;
};
```

- Capacity: 32 devices
- Eviction: LRU by `last_seen_ms` when table is full
- SSID eviction: oldest `last_seen_ms` within the device when 16 slots full
- OUI vendor lookup done once at creation time

### RandomizedAggregate

```cpp
struct RandomizedAggregate {
    uint16_t    unique_macs;       // Approximate (sliding window)
    uint16_t    total_probes;
    uint8_t     ssid_count;
    DeviceSSID  ssids[16];         // Union of all SSIDs probed by randomized MACs
    int8_t      rssi_strongest;
    uint32_t    last_seen_ms;
    uint8_t     recent_macs[8][6]; // Sliding window for distinct-MAC counting
    uint8_t     recent_macs_count;
};
```

- All randomized MACs collapse into this single record
- `unique_macs` is approximate: a MAC returning after window eviction is double-counted
- SSID list is exact (deduplicated by string)

### Session Lifecycle

```
probe_sniffer_init()     ← setup(), once at boot
probe_sniffer_start()    ← first screen entry, memsets state, starts capture
  ... capture runs across navigation (backspin) ...
probe_sniffer_stop()     ← shake (navigation_emergency_stop), sets running=false
probe_sniffer_start()    ← re-entry after stop, memsets (fresh session)
```

Key: `hide()` does NOT stop capture. `start()` is a no-op if already running.

---

## Screen Architecture

### Three Views

| View | Purpose | Gesture In | Gesture Out |
|------|---------|-----------|-------------|
| LIST | Device list + randomized row | Screen entry | Tap → detail, Backspin → menu |
| DEVICE_DETAIL | Single device's identity + SSIDs | Tap on device row | Tap/Backspin → list |
| RANDOMIZED_DETAIL | Aggregate stats + SSIDs + MACs | Tap on [rnd] row | Tap/Backspin → list |

### List View

- **Status bar**: "HOPPING · M:SS" (or "PAUSED · M:SS" in orange)
- **Stats line**: device count · probe count · unique SSID count
- **3 visible device rows**: full MAC + vendor / probe count + SSID count + RSSI
- **Sticky [rnd] row**: always visible at bottom, dimmed
- **Footer hint**: gesture reminder

Cursor tracks by MAC, not index. When the list reorders (sort by `last_seen_ms` desc), the cursor follows its device. If the device is evicted, cursor jumps to top.

### Detail Views

Center-aligned design matching the project's circular display patterns:
- Cyan header (MAC or "RANDOMIZED") centered at top
- White identity block, center-aligned
- Separator line (200px, `COL_CYAN_DIM`)
- Green SSID list (scrollable with encoder)
- Gray footer hint

### Gesture Map

| Context | Encoder | Tap | Hold | Backspin | Shake |
|---------|---------|-----|------|----------|-------|
| List | Cursor ↑↓ | Open detail | Toggle pause | WiFi menu | Emergency stop |
| Detail | Scroll SSIDs | Back to list | (no-op) | Back to list | Emergency stop |

### Pause (FR-05)

- Hold gesture toggles `state.paused`
- While paused: UI frozen (no row reorder, no count update on screen)
- Capture continues in background (ISR ring drained, device table updated)
- On resume: accumulated changes rendered immediately
- Status bar shows "PAUSED" in `COL_ORANGE`

---

## Memory Budget

| Component | Size |
|-----------|------|
| Device table (32 × ~700 B) | ~22 KB |
| Randomized aggregate | ~750 B |
| Raw ring (32 × ~50 B) | ~1.6 KB |
| **Total** | **~24 KB** |

Measured RAM: 50.7% (166 KB / 320 KB) — up ~21 KB from Phase 3 baseline (44.1%).
Free internal SRAM: ~154 KB remaining.

---

## Key Design Decisions

1. **No channel control on this screen.** The encoder scrolls the device list. Channel hopping runs automatically via the WiFi scanner's 350ms timer. The old `ENC_CHANNEL_HOP` binding was removed; `enc_mode` is now `ENC_MENU`.

2. **Randomized MACs collapsed, not tracked individually.** Per-MAC tracking of randomized addresses is impossible at bounded memory. The sliding window (8 MACs) approximates distinct count.

3. **Capture persists across navigation.** Matches the beacon flood persistence model. Only shake (emergency stop) resets the session.

4. **Sort on every render tick.** With max 32 devices, insertion sort is negligible cost (~microseconds). This ensures the most recently active device is always at the top.

5. **SSID deduplication at insert time.** `ssid_list_update()` checks for existing SSID before adding. No periodic recount needed.

6. **Circular display layout.** All detail views use center-aligned LVGL objects (`LV_ALIGN_TOP_MID`) with proper separator lines. Left-aligned content uses x≥40 to stay within the circle boundary at all y-positions.

---

## What Was Removed

- `PROBE_BUFFER_SIZE` (100) → replaced by `RAW_BUFFER_SIZE` (32)
- `recount_uniques()` — O(n²) sweep no longer needed
- `state.unique_macs`, `state.unique_ssids` — replaced by device table counts
- `state.channel_hop` — no longer relevant
- Event-log row layout
- `ENC_CHANNEL_HOP` binding for this screen
- `scanner_set_channel()` calls from this screen

---

## What Was Preserved

- `probe_sniffer_on_frame()` — same IRAM_ATTR callback signature
- ISR ring buffer (8 slots, 128 bytes, spinlock) — unchanged
- `ProbeRequest` struct — identical to Phase 3
- OUI vendor lookup (`oui_lookup()` from `wifi_scanner.h`)
- Randomized MAC detection (bit 1 of byte 0)
- Screen registration (`SCREEN_PROBE_SNIFF`, WiFi group)
- Exported function signatures (`on_encoder`, `on_tap`, `on_hold`)

---

## Gotchas

### ESP32 float printf
Same as Phase 3: `%f` doesn't work in Arduino ESP32 newlib-nano. Use `%lu` with `(unsigned long)` casts for uint32_t values.

### Lambda in count_total_unique_ssids()
The unique SSID counter uses a C++11 lambda with a static buffer. ESP32 Arduino supports this, but if you encounter compiler issues on different toolchain versions, replace with a regular nested loop.

### Cursor anchoring edge case
If all 32 device slots are full and a new device evicts the cursor's device, the cursor jumps to the top (row 0). This is intentional per the FSD. The user sees the cursor "jump" but the new top device is the most recently active one, which is the right default.

### Pause does not freeze capture
The ISR ring is always drained even while paused. Only the screen rendering freezes. The device table continues to grow/update in the background. On resume, the user may see devices jump positions or new devices appear.
