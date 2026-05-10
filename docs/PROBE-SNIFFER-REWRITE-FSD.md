---
type: specification
project: NetKnob
phase: 3.1
status: draft
created: 2026-05-10
tags: [fsd, specification, esp32, wifi, probe-sniffer, rewrite, english]
---

# Functional Specification — NetKnob Probe Sniffer Rewrite

> Device-centric rewrite of the Phase 3 probe sniffer. Replaces the event-log UI and unused channel dial with a usable device list, drill-down detail, and pause-while-running.

**Version:** 1.0 — Draft
**Date:** 2026-05-10
**Author:** Roar Harjo

---

## 1. Introduction

### 1.1 Purpose

The Phase 3 probe sniffer ships in firmware but the GUI surfaces almost none of the data it captures. The screen renders a flat event log: roughly seven probe rows are visible, the headline counts (e.g., "105 probes · 3 devices") have no drill-down, and the encoder dial advertises a channel-hop control that has no perceivable effect or use during a session. This specification rewrites the screen and the data model behind it so that captured data becomes reachable and the dial does something meaningful.

### 1.2 Background

Phase 3 captures probe requests via the existing promiscuous-mode callback (see PHASE3-FSD-EN.md §3.2 and PHASE3-HANDOVER.md). It stores up to 100 probes in a circular buffer, counts unique MACs and SSIDs, and renders a scrolling event view. As deployed, the screen has three concrete problems:

1. **Channel dial is decorative.** The encoder is bound to channel hop, but the user is never investigating "what is on channel N" on this screen — they are investigating who is around. The control occupies the primary input mechanism without serving the screen's purpose.
2. **Headline stats are dead-ends.** "3 devices" and "105 probes" appear in the status bar. Neither value is reachable by interaction. The user cannot see what those three devices are or what the 105 probes contained.
3. **Event-log framing buries the signal.** A single device probing for five SSIDs becomes five list rows, three of which scroll off-screen within seconds in a busy environment. The data the screen exists to reveal — which devices are present, what networks they leak — is fragmented across rows.

### 1.3 Scope

This rewrite covers the probe sniffer screen and its supporting data model only. It does **not** add new probe-sniffer features beyond the device-centric reframe. Specifically out of scope:

- Aggregated room-level "personlighetsprofil" view (see IDEER-V2.md §Probe request-personlighetsprofil)
- Whitelist / own-emission audit mode
- Dwell-time classification (forbipasserende / besøkende / stasjonær)
- Co-presence graphs across radios
- PCAP export
- Probe-request RSSI directionality

The data model added here is designed so those features can extend it later without a second rewrite, but none of them are implemented as part of this work.

### 1.4 Prerequisites

| Prerequisite | Source | Status |
|--------------|--------|--------|
| Phase 3 probe sniffer module (`wifi_probe_sniffer.cpp`) | Phase 3 | Present (rewritten body, same callback signature) |
| Promiscuous callback hook | Phase 1 | Unchanged |
| OUI vendor lookup | Phase 3 | Unchanged |
| Screen lifecycle (retain/hide) | Phase 2 | Unchanged |
| Encoder/tap/hold/backspin/shake routing | Phase 2 | Unchanged |
| Heap monitoring | Phase 2 | Required (sizing budget) |

No hardware spike is required. The work is firmware-only on the ESP32-S3 main chip.

---

## 2. Architecture

### 2.1 Modules Touched

```
src/
├── wifi_probe_sniffer.cpp / .h      ← REWRITTEN: data model, aggregation, randomized handling
└── screens/
    └── scr_probe_sniff.cpp / .h     ← REWRITTEN: list view + detail views
```

No other modules change. `wifi_scanner.cpp`'s promiscuous callback continues to invoke `probe_sniffer_on_frame()` exactly as today (subtype 0x40 early-return branch). The callback signature is preserved.

### 2.2 Data Flow

```
802.11 RX
   │
   ▼
promisc_callback() (wifi_scanner.cpp)
   │   subtype == 0x40?
   ▼
probe_sniffer_on_frame(frame, len, rssi, channel)        [ISR context]
   │
   ▼  parse src_mac, ssid, randomized_bit
   │
   ├─ raw[write_idx++ % RAW_BUFFER_SIZE] = ProbeRequest{...}     ← unchanged ring write (small)
   │
   ▼
   (ring buffer drained from main loop)

probe_sniffer_update() (main loop)                       [task context]
   │
   ▼  drain raw ring, for each unread entry:
   │
   ├─ if randomized → randomized_aggregate_update()
   └─ else          → device_update() (find_or_create + LRU evict)

scr_probe_sniff_render()                                 [LVGL refresh]
   │
   ▼  read DeviceEntry[] sorted by last_seen_ms desc
   │  + RandomizedAggregate (sticky bottom)
   ▼
   LVGL list rebuild (only if state.paused == false)
```

The ISR-safe pattern from Phase 3 is preserved: the callback writes to the raw ring buffer; all device-table mutation happens in `probe_sniffer_update()` from the main loop.

---

## 3. Functional Requirements

### 3.1 Capture & Aggregation

#### FR-01: Frame capture (unchanged from Phase 3)

| Property | Value |
|----------|-------|
| Frame type | Management, subtype Probe Request (0x04) |
| Source | `promisc_callback()` early-return branch (existing) |
| Channel | Auto-hop (same rate as WiFi scanner: 350 ms/channel). No user control. |
| Data extracted | Source MAC, SSID, RSSI, timestamp, channel, randomized bit |

#### FR-02: Real-MAC device table

For each non-randomized source MAC, the firmware maintains a `DeviceEntry` containing identity, signal range, probe count, and a per-device list of distinct SSIDs probed (each with its own count and last-seen timestamp). Entries are created on first probe, updated on each subsequent probe.

| Cap | Value | Behavior on overflow |
|-----|-------|----------------------|
| Devices tracked | 32 | Evict device with oldest `last_seen_ms` |
| SSIDs per device | 16 | Evict SSID slot with oldest `last_seen_ms` within that device |

#### FR-03: Randomized-MAC aggregate

All randomized MACs (bit 1 of byte 0 set, per Phase 3 detection) collapse into a single `RandomizedAggregate` record holding the union of distinct SSIDs probed, the strongest RSSI observed, the most-recent `last_seen_ms`, and a sliding window of the 8 most recently seen distinct randomized MACs (`recent_macs[8]`). The 8-entry window approximates the distinct-MAC count: if a randomized MAC arrives that is not in the window, `unique_macs` increments and the oldest is evicted.

The aggregate is exact for SSIDs (the SSID list is deduplicated). It is approximate-only for `unique_macs`: a randomized MAC that returns after being evicted from the 8-entry window will be double-counted. This is accepted as the cost of avoiding O(n) tracking of every randomized MAC ever seen.

#### FR-04: Session lifecycle

Capture starts on first entry to the screen and continues across navigation (matching the beacon-flood persistence model from Phase 3). The capture is stopped only by:

- Shake (emergency stop → main menu)
- Phase 4 settings reset (out of scope here)

Re-entering the screen shows the accumulated session data. There is no per-entry reset.

#### FR-05: Pause

`state.paused` freezes the UI's view of the device list (no row reorder, no count refresh in the status bar). The capture pipeline (`probe_sniffer_on_frame`, `probe_sniffer_update`) keeps running and accumulating. Resume merges accumulated changes back into the rendered list.

---

### 3.2 List View (Primary Screen)

#### FR-06: Layout

The list view occupies `SCREEN_PROBE_SNIFF`. Top section: status bar (2 lines). Middle: scrollable device list (up to 3 real-device rows visible at a time, 2 lines per row). Bottom: sticky randomized-aggregate row (1 line, dimmed). Footer: gesture hint (1 line).

#### FR-07: Status bar

```
HOPPING · 0:42                    ← line 1: channel state + elapsed session time
3 devs · 105p · 14 SSIDs          ← line 2: aggregate counts
```

While `state.paused == true`, line 1 reads `PAUSED · <elapsed>` in an amber tint. Aggregate counts continue to update internally but are frozen on screen until resume.

#### FR-08: Device row

Two lines per row:

```
A4:C3:F0:9B:21:E7  TP-Link
12p · 5 SSIDs · -58 dBm
```

| Field | Source | Notes |
|-------|--------|-------|
| MAC | `device.mac` formatted full | Full `XX:XX:XX:XX:XX:XX` |
| Vendor | `device.vendor` from OUI | Empty string if no OUI match. Truncated with `…` if MAC + vendor exceeds line width. |
| Probe count | `device.probe_count` | Suffix `p` |
| SSID count | `device.ssid_count` | Singular `SSID` if 1, else `SSIDs` |
| RSSI | `device.rssi_last` | Last measured, integer dBm |

The highlighted (cursor) row is rendered with a filled background, leading `▶` marker is optional (background fill is sufficient).

#### FR-09: Sort order

Devices sorted by `last_seen_ms` descending (most recently active first). Sort is applied on every render tick. Sort is not user-changeable.

#### FR-10: Cursor anchoring

The selection cursor tracks the **MAC**, not the row index. When a probe arrives for a non-top device and the list reorders, the cursor follows its device. If the cursor's device is evicted (FR-02 LRU), the cursor jumps to the now-top row.

#### FR-11: Scrolling

When more real devices exist than fit the viewport (3 real-device rows × 2 lines = 6 lines available), turning the encoder past the last visible real-device row scrolls the list of real devices. The randomized aggregate row is sticky: it remains pinned to the bottom of the viewport regardless of scroll position, and is always reachable in one cursor step past the last real-device row regardless of how many real devices exist or how the list is currently scrolled. The aggregate row never scrolls out of view.

#### FR-12: Randomized aggregate row

```
[rnd] 23 MACs · 47p · 8 SSIDs
```

Rendered dimmed to demote it visually. `MACs` reflects `randomized.unique_macs` (approximate per FR-03).

#### FR-13: Empty state

When `device_count == 0 && randomized.unique_macs == 0`, the list area renders the literal text `Listening…` centered, with the elapsed timer continuing in the status bar. The randomized aggregate row is hidden in this state.

#### FR-14: Gesture map (list view)

| Gesture | Action |
|---------|--------|
| Encoder CW/CCW | Move cursor down/up; auto-scroll viewport when cursor reaches edge |
| Touch tap | Open detail view for cursor's device (or randomized aggregate detail if cursor on `[rnd]` row) |
| Touch hold (1 s) | Toggle `state.paused` |
| Backspin | Return to WiFi group menu (capture continues) |
| Shake | Emergency stop → main menu (capture stops) |

---

### 3.3 Real-Device Detail View

#### FR-15: Layout

```
     ╭──────────────────────╮
    ╱     DEVICE DETAIL       ╲
   │  78:2A:CD:11:3F:B0       │
   │  Apple · 8p · -64 dBm    │
   │  -71 .. -52 · seen 0:03  │
   │  first 0:42 · CH 6 last  │
   │  ─────────────────────    │
   │  Probing for:             │
   │    Telenor          (5×) │
   │    eduroam          (3×) │
   │  ▶ HomeNet-5G       (2×) │
   │    Hotell-Berlin    (1×) │
   │    iPhone-Henrik    (1×) │
   │              ↻ more       │
    ╲     tap = back          ╱
     ╰──────────────────────╯
```

#### FR-16: Identity block

Top four lines, fixed:

| Line | Content |
|------|---------|
| 1 | Full MAC (`XX:XX:XX:XX:XX:XX`) |
| 2 | `<vendor> · <probe_count>p · <rssi_last> dBm` — vendor falls back to literal `Real (no OUI)` if empty; vendor truncated with `…` if line exceeds width |
| 3 | `<rssi_min> .. <rssi_max> · seen <last_seen_ago>` |
| 4 | `first <first_seen_ago> · CH <last_channel> last` |

Time format: `M:SS` for under one hour, `HH:MM` beyond.

#### FR-17: SSID list

Below the separator, the per-device SSID list, sorted by `probe_count` descending (most-probed SSIDs first; ties broken by `last_seen_ms` desc). Each row: SSID name (truncated at ~17 chars with `…`) and probe count in parentheses. Empty/broadcast probes render as the literal `(broadcast)`. The cursor (`▶`) marks the highlighted SSID. The encoder scrolls this list. A `↻ more` indicator renders at the bottom when the list extends past the visible window.

#### FR-18: Gesture map (detail view)

| Gesture | Action |
|---------|--------|
| Encoder CW/CCW | Scroll SSID list within detail view |
| Touch tap | Back to list view |
| Touch hold (1 s) | Reserved (no-op in v1) |
| Backspin | Back to list view |
| Shake | Emergency stop → main menu |

---

### 3.4 Randomized Aggregate Detail View

#### FR-19: Layout

```
     ╭──────────────────────╮
    ╱  RANDOMIZED DEVICES     ╲
   │  23 MACs · 47 probes      │
   │  -52 dBm strongest        │
   │  last seen 0:01 ago       │
   │  ─────────────────────    │
   │  All SSIDs probed:        │
   │    (broadcast)     (28×) │
   │    Telenor          (8×) │
   │  ▶ free_wifi        (4×) │
   │    Starbucks        (3×) │
   │  ─────────────────────    │
   │  Last 8 MACs:             │
   │  DA:23:1F:* C2:8A:F3:*    │
   │  4E:1B:90:* …             │
    ╲     tap = back          ╱
     ╰──────────────────────╯
```

#### FR-20: Content blocks

| Block | Content |
|-------|---------|
| Identity | `<unique_macs> MACs · <total_probes> probes`, `<rssi_strongest> dBm strongest`, `last seen <ago>` |
| SSID list | `randomized.ssids[]`, sorted by `probe_count` desc; same row format as FR-17 |
| Recent MACs | Up to 8 entries from `recent_macs[]`, each truncated to first 3 octets followed by `:*`, wrapping to multiple lines as needed; `…` if window is full |

#### FR-21: Gesture map

Identical to FR-18.

---

## 4. Data Structures

### 4.1 New / Replaced Types

```cpp
#define MAX_DEVICES        32     // Real-MAC devices tracked
#define MAX_SSIDS_PER_DEV  16     // Distinct SSIDs remembered per device
#define RAW_BUFFER_SIZE    32     // Reduced raw ring (debug / detail tail only)
#define RANDOMIZED_MAC_WINDOW 8   // Sliding window for distinct-randomized count

struct DeviceSSID {
    char     ssid[33];
    uint16_t probe_count;
    uint32_t last_seen_ms;
};

struct DeviceEntry {
    uint8_t      mac[6];
    char         vendor[20];
    int8_t       rssi_last;
    int8_t       rssi_min;
    int8_t       rssi_max;
    uint16_t     probe_count;
    uint8_t      ssid_count;
    DeviceSSID   ssids[MAX_SSIDS_PER_DEV];
    uint32_t     first_seen_ms;
    uint32_t     last_seen_ms;
    uint8_t      last_channel;
};

struct RandomizedAggregate {
    uint16_t    unique_macs;        // Approximate (FR-03)
    uint16_t    total_probes;
    uint8_t     ssid_count;
    DeviceSSID  ssids[MAX_SSIDS_PER_DEV];
    int8_t      rssi_strongest;
    uint32_t    last_seen_ms;
    uint8_t     recent_macs[RANDOMIZED_MAC_WINDOW][6];
    uint8_t     recent_macs_count;  // 0..RANDOMIZED_MAC_WINDOW
};

struct ProbeSnifferState {
    DeviceEntry          devices[MAX_DEVICES];
    uint8_t              device_count;
    RandomizedAggregate  randomized;
    ProbeRequest         raw[RAW_BUFFER_SIZE];   // Same ProbeRequest struct as Phase 3
    uint16_t             raw_write_index;
    uint32_t             total_probes;
    uint32_t             session_start_ms;
    bool                 running;
    bool                 paused;                  // FR-05
};
```

`ProbeRequest` is unchanged from Phase 3 (PHASE3-FSD-EN.md §3.2 FR-09). Only its containing buffer shrinks from 100 → 32 entries.

### 4.2 Memory Budget

| Component | Size | Notes |
|-----------|------|-------|
| `DeviceEntry` (per) | ~700 B | 6 + 20 + 6 + 2 + 1 + (16 × 41) + 8 + 1 ≈ 700 |
| Device table (32 × ~700) | ~22 KB | Worst-case full table |
| `RandomizedAggregate` | ~750 B | Same SSID-list overhead + 48 B MAC window |
| Raw ring (32 × ~50) | ~1.6 KB | `ProbeRequest` struct from Phase 3 |
| **Total** | **~24 KB** | Up from ~5 KB in Phase 3 |

Phase 3 left ~93 KB free internal SRAM after init. The +19 KB delta fits with margin. No PSRAM use.

### 4.3 Eviction Rules

| Cap reached | Action |
|-------------|--------|
| `device_count == MAX_DEVICES`, new real MAC arrives | Evict device with smallest `last_seen_ms`; new device takes its slot |
| `device.ssid_count == MAX_SSIDS_PER_DEV`, new SSID arrives | Evict SSID slot in that device with smallest `last_seen_ms` |
| Same for `randomized.ssids[]` | Same rule |
| `recent_macs_count == RANDOMIZED_MAC_WINDOW`, new randomized MAC | Shift FIFO; oldest drops; `unique_macs++` |

---

## 5. User Interaction — Summary

```
┌─────────────────────────────────────────────────────────────────┐
│                    PROBE SNIFFER (LIST VIEW)                    │
│                                                                 │
│  Encoder CW/CCW ── Move cursor (auto-scroll at edge)           │
│  Touch tap ──────── Open detail (device or randomized)         │
│  Touch hold (1s) ── Pause/resume rendering                     │
│  Backspin ────────── Back to WiFi group (capture continues)    │
│  Shake ────────────── Emergency stop → main menu               │
│                                                                 │
│                    DEVICE / RANDOMIZED DETAIL                   │
│                                                                 │
│  Encoder CW/CCW ── Scroll SSID list                            │
│  Touch tap ──────── Back to list view                          │
│  Backspin ────────── Back to list view                         │
│  Shake ────────────── Emergency stop → main menu               │
└─────────────────────────────────────────────────────────────────┘
```

---

## 6. Migration from Current Implementation

### 6.1 What is Removed

- `state.write_index`, `state.unique_macs`, `state.unique_ssids` (`ProbeSnifferState` is replaced; the new struct exposes equivalent counts via `device_count` and the SSID totals are derivable per-screen).
- `recount_uniques()` O(n²) sweep — no longer needed; counts maintained incrementally on insert/evict.
- The 100-entry `probes[]` array is replaced by a 32-entry raw ring used only for the optional debug tail.
- The `ENC_CHANNEL_HOP` binding for this screen — removed. No encoder mode is registered for the probe sniffer; the encoder routes directly to list scroll. `ENC_CHANNEL_HOP` continues to exist for the WiFi scanner, untouched.
- The current `scr_probe_sniff` event-log row layout and channel-hop footer hint.

### 6.2 What is Reused

- `probe_sniffer_on_frame()` callback signature (frame, len, rssi, channel).
- OUI vendor lookup helper (called once at `find_or_create()` time).
- Randomized-MAC bit detection (bit 1 of byte 0).
- The 350 ms channel auto-hop timer in the WiFi scanner — the probe sniffer still benefits from it; no probe-sniffer-specific channel logic exists in the rewrite.
- Screen lifecycle integration (`SCREEN_PROBE_SNIFF`, group menu entry).

### 6.3 What Changes Names

The current `probe_sniffer_on_frame()` body is rewritten but the symbol stays. New internal helpers:

- `device_update(DeviceEntry*, ssid, rssi, channel, now_ms)`
- `find_or_create_device(mac) -> DeviceEntry*`
- `randomized_aggregate_update(mac, ssid, rssi, channel, now_ms)`
- `evict_oldest_device()`
- `evict_oldest_ssid(DeviceSSID[], count)`

---

## 7. Acceptance Criteria

| ID | Criterion |
|----|-----------|
| AC-01 | Entering `SCREEN_PROBE_SNIFF` for the first time starts capture and shows the empty state until the first probe arrives. |
| AC-02 | After ≥1 real-MAC probe, the list view renders one device row per distinct real source MAC, sorted by most recent activity. |
| AC-03 | The status bar shows the elapsed session timer, total probe count, distinct device count, and total distinct SSID count. |
| AC-04 | Encoder turns move the cursor through device rows; the viewport auto-scrolls when the cursor reaches the bottom edge. |
| AC-05 | Touch tap on a real-device row opens the device detail view showing the SSID list sorted by probe count. |
| AC-06 | The device detail view renders identity, RSSI range, first/last seen, and last channel, with the SSID list as the dominant block. |
| AC-07 | Touch tap or backspin in detail view returns to the list view with the cursor on the same MAC. |
| AC-08 | Touch hold (1 s) on the list view toggles paused state; status bar shows `PAUSED`; rows do not reorder or update visually until resumed; capture totals continue to advance internally and are reflected after resume. |
| AC-09 | The randomized aggregate row is sticky at the bottom of the viewport regardless of scroll position; tapping it opens the randomized aggregate detail view. |
| AC-10 | Randomized aggregate detail view shows MAC count, total probes, strongest RSSI, last-seen, full union SSID list (with counts), and the last 8 distinct randomized MACs. |
| AC-11 | When 32 real devices are tracked and a 33rd arrives, the device with the oldest `last_seen_ms` is evicted; cursor follows MAC and jumps to top row if its device was evicted. |
| AC-12 | When a per-device SSID list is full and a new SSID arrives, the oldest SSID slot is evicted (verified by reproducing the case in test or by code inspection). |
| AC-13 | Sort by `last_seen_ms` descending: a probe arriving for a non-top device promotes it to row 0 on the next render tick. The cursor remains anchored to its device's MAC. |
| AC-14 | Backspin from the list view returns to the WiFi group menu; on re-entry the same accumulated device data is shown (capture persisted). |
| AC-15 | Shake on either screen halts capture and returns to main menu; re-entry starts a fresh session. |
| AC-16 | Heap-free measurement: after 5 minutes of capture in a busy environment (≥ 200 probes/min), free internal SRAM remains within 5 KB of the first-minute baseline (no leak). |
| AC-17 | Empty SSID probes (broadcast) appear in detail views as the literal `(broadcast)` and are counted toward the SSID count. |

---

## 8. Out of Scope (Explicit)

| Item | Rationale |
|------|-----------|
| Aggregated room-level personality profile | IDEER-V2.md feature; future screen |
| Whitelist / own-emission audit | IDEER-V2.md feature; needs MAC-prefix entry UI first |
| Dwell-time classification | IDEER-V2.md feature; needs longer-term data structure |
| User-changeable sort order | YAGNI; "most recently active" is the right default for live use |
| User-controlled channel lock | Contradicts FR-01 (auto-hop); the WiFi scanner already provides per-channel inspection |
| Forget / blacklist a device from the screen | YAGNI; LRU eviction handles bounded memory |
| Promote a randomized MAC to its own real-device row | YAGNI; randomized MACs are ephemeral by definition |
| PCAP export / NVS persistence | Future feature, separate spec |

---

## 9. Open Questions

None at time of writing. All clarifying questions from the brainstorming pass were resolved before the spec was written.

---

## 10. Appendix A — Tunable Constants

| Name | File | Value | Range | Purpose |
|------|------|-------|-------|---------|
| `MAX_DEVICES` | `wifi_probe_sniffer.h` | 32 | 16–64 | Real-MAC table size |
| `MAX_SSIDS_PER_DEV` | `wifi_probe_sniffer.h` | 16 | 8–32 | Per-device SSID slots |
| `RAW_BUFFER_SIZE` | `wifi_probe_sniffer.h` | 32 | 16–100 | Raw probe ring (debug) |
| `RANDOMIZED_MAC_WINDOW` | `wifi_probe_sniffer.h` | 8 | 4–16 | Distinct-MAC sliding window |
| `PAUSE_HOLD_MS` | `scr_probe_sniff.cpp` | 1000 | 500–2000 | Touch-hold threshold for pause |
| `PROBE_MAC_RANDOMIZED_BIT` | `wifi_probe_sniffer.h` | 0x02 | — | Unchanged from Phase 3 |
