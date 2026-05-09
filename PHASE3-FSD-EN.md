---
type: specification
project: NetKnob
phase: 3
status: draft
created: 2026-05-05
tags: [fsd, specification, esp32, wifi, attacks, deauth, beacon, phase-3, english]
---

# Functional Specification — NetKnob, Phase 3

> WiFi active attacks: Beacon Flood, Probe Sniffer, and Deauthentication

**Version:** 1.0 — Draft
**Date:** 2026-05-05
**Author:** Roar Harjo

---

## 1. Introduction

### 1.1 Purpose

This specification describes the complete functionality for Phase 3 of NetKnob: WiFi active attack capabilities built on the Phase 1 scanner and Phase 2 navigation architecture. The attacks are implemented in order of increasing technical risk: beacon flood (uses official TX API) → probe sniffer (passive capture with display) → deauthentication (requires injection of blocked frame types).

### 1.2 Product Description

Phase 3 transforms NetKnob from a passive reconnaissance tool into an active WiFi security testing device. The user selects targets from the existing WiFi scanner, configures attack parameters via the dial, and launches attacks with a deliberate gesture. All attacks can be stopped instantly via the shake emergency stop (Phase 2).

### 1.3 Target Audience

The developer (Roar Harjo) — authorized security testing, education, and validation of active WiFi capabilities on ESP32-S3.

### 1.4 Scope Exclusions

Phase 3 includes **only** the attacks listed in this document. Explicitly out of scope:

- BLE attacks (AppleJuice, SwiftPair, BLE spam) — Phase 3+ or later
- PMKID capture — Phase 3+ (passive, depends on P3 infrastructure)
- Evil Portal — Phase 3+ (requires SoftAP + HTTP server)
- WPA handshake capture (4-way) — Phase 3+ (requires deauth + capture timing)
- Secondary ESP32 involvement — Phase 4
- Packet capture/export (PCAP) — future
- 5 GHz attacks — hardware limitation (impossible)

### 1.5 Prerequisites

| Prerequisite | Source | Status |
|--------------|--------|--------|
| Navigation system functional | Phase 2 | Required |
| WiFi scanner operational | Phase 1 | Required |
| Screen lifecycle model (retain/hide) | Phase 2 | Required |
| Encoder mode routing | Phase 2 | Required |
| Heap monitoring active | Phase 2 | Required |
| **TX + RX coexistence verified** | Pre-Phase 3 spike | **BLOCKER — must test before implementation** |

### 1.6 Critical Pre-Phase 3 Validation

Before implementing any Phase 3 feature, the following must be tested in an isolated test sketch:

| Test | Method | Pass Criteria |
|------|--------|---------------|
| `esp_wifi_80211_tx()` sends beacon frames | Craft beacon, send, verify with external sniffer | Frame appears on target channel |
| Promiscuous RX continues during TX | Enable promiscuous + callback, send TX in loop, count RX callbacks | RX callbacks fire between TX sends |
| Deauth frame delivery | Attempt via official API, then rogue AP method, then WSL bypass | At least one method delivers deauth |
| Memory stability under TX load | Send 1000 beacons, measure heap before/after | No leak (< 1 KB drift) |

**If TX + RX cannot coexist:** Implement time-division multiplexing (scan → attack → scan → attack) with configurable duty cycle. UX degrades but functionality preserved.

**If deauth is blocked on S3:** Use rogue AP method as primary deauth mechanism (see §3.3).

---

## 2. System Architecture

### 2.1 Module Structure

```
src/
├── wifi_attack.cpp / .h         ← NEW: Attack engine (beacon flood, deauth TX)
├── wifi_probe_sniffer.cpp / .h  ← NEW: Probe request capture + display
├── attack_common.cpp / .h       ← NEW: Shared attack utilities
│                                   (frame crafting, MAC spoofing, timing)
├── wifi_scanner.cpp / .h        ← EXTENDED: Target selection API for attacks
└── screens/
    ├── scr_beacon_flood.cpp/.h  ← NEW: Beacon flood UI
    ├── scr_probe_sniff.cpp/.h   ← NEW: Probe sniffer UI
    └── scr_deauth.cpp/.h        ← NEW: Deauth attack UI
```

### 2.2 Attack Engine Architecture

```
┌───────────────────────────────────────────────────┐
│                  ATTACK ENGINE                      │
├───────────────────────────────────────────────────┤
│                                                    │
│  ┌──────────────┐    ┌──────────────────────┐     │
│  │ attack_state │    │ Frame Crafter        │     │
│  │  .running    │    │  craft_beacon()      │     │
│  │  .type       │    │  craft_deauth()      │     │
│  │  .target     │    │  craft_probe_resp()  │     │
│  │  .params     │    │  set_src_mac()       │     │
│  │  .stats      │    └──────────────────────┘     │
│  └──────┬───────┘                                  │
│         │                                          │
│         ▼                                          │
│  ┌──────────────────────────────────────────┐     │
│  │ TX Scheduler                             │     │
│  │  • Timed packet send (configurable rate) │     │
│  │  • Interleave with RX if needed          │     │
│  │  • Auto-stop on timeout                  │     │
│  │  • Stats: packets sent, duration         │     │
│  └──────────────────────────────────────────┘     │
│                                                    │
│  ┌──────────────────────────────────────────┐     │
│  │ Safety Layer                             │     │
│  │  • Max duration timeout (configurable)   │     │
│  │  • Emergency stop hook (shake gesture)   │     │
│  │  • ENC_LOCKED during active attack       │     │
│  │  • Cannot start without explicit confirm │     │
│  └──────────────────────────────────────────┘     │
│                                                    │
└───────────────────────────────────────────────────┘
```

### 2.3 Data Flow — Attack Lifecycle

```
WiFi Scanner (target selected)
      │
      ▼
Navigation → Attack Screen
      │
      ▼
Configure parameters (encoder)    ← Adjust count/rate/channel/target
      │
      ▼
Confirm start (touch hold 1s)    ← Deliberate gesture required
      │
      ▼
Attack running                    ← ENC_LOCKED, border glow, stats updating
      │
      ├── Normal end (timeout/count reached) → Results screen
      │
      ├── Emergency stop (shake) → Immediate halt → main menu
      │
      └── Manual stop (touch hold 1s) → Results screen
```

### 2.4 Screen Group Update

| Group | Phase 2 Screens | Phase 3 Additions |
|-------|----------------|-------------------|
| **WiFi** | Scanner | Beacon Flood, Probe Sniffer, Deauth |
| **BLE** | Scanner | (unchanged) |
| **System** | Settings, Debug | (unchanged) |

---

## 3. Functional Requirements

### 3.1 Beacon Flood

#### FR-01: Beacon Flood — Purpose

Flood the airspace with fake access point beacons. Nearby devices see dozens of fake networks in their WiFi settings. This is the lowest-risk active attack (uses official `esp_wifi_80211_tx()` API for beacon frames, which Espressif explicitly supports).

#### FR-02: Beacon Flood — Configuration

| Parameter | Input | Default | Range | Encoder Step |
|-----------|-------|---------|-------|-------------|
| SSID count | Encoder CW/CCW | 20 | 1–50 | ±1 |
| Channel | Inherited from scanner | Last scanned channel | 1–13 | — |
| SSID source | Choice | Random | Random / Wordlist / Clone nearby | Encoder cycles |
| TX rate | Configurable | 10 beacons/sec per SSID | 1–100 | ±5 |
| Duration | Configurable | 30 seconds | 5–300 seconds (0 = infinite) | ±5 |

#### FR-03: Beacon Flood — SSID Generation

| Mode | Behavior |
|------|----------|
| **Random** | Generate random 8-12 character SSIDs (alphanumeric) |
| **Wordlist** | Cycle through built-in list of common/funny SSIDs |
| **Clone nearby** | Copy SSIDs from scanner AP list, randomize last 2 chars of BSSID |

**Built-in wordlist (20 entries):**
```
Free WiFi, FBI Surveillance Van, Pretty Fly for a Wi-Fi,
Drop It Like It's Hotspot, The LAN Before Time,
Wu-Tang LAN, Router? I Hardly Know Her,
Bill Wi the Science Fi, LAN Solo, The Promised LAN,
Nacho WiFi, Get Off My LAN, It Burns When IP,
No More Mr Wi-Fi, Silence of the LANs,
Loading..., Searching..., Connecting...,
Not Your WiFi, Virus Detected
```

#### FR-04: Beacon Flood — Frame Construction

```cpp
// 802.11 Beacon frame structure
struct BeaconFrame {
    // MAC header (24 bytes)
    uint8_t  frame_control[2];   // 0x80 0x00 (beacon)
    uint8_t  duration[2];        // 0x00 0x00
    uint8_t  dest_addr[6];       // FF:FF:FF:FF:FF:FF (broadcast)
    uint8_t  src_addr[6];        // Random MAC per SSID
    uint8_t  bssid[6];           // Same as src_addr
    uint8_t  seq_ctrl[2];        // Sequence number (increment)

    // Beacon body
    uint8_t  timestamp[8];       // Fake timestamp
    uint8_t  interval[2];        // 0x64 0x00 (100 TU = 102.4ms)
    uint8_t  capability[2];      // 0x31 0x04 (ESS + privacy)

    // Tagged parameters (IEs)
    // SSID IE (tag 0)
    // Supported Rates IE (tag 1)
    // DS Parameter Set IE (tag 3) — channel number
    // RSN IE (tag 48) — fake WPA2 to appear as secure network
};
```

Each fake AP gets:
- Unique random MAC address (generated once, reused for duration)
- Incrementing sequence number (per AP)
- DS Parameter Set matching attack channel
- RSN IE making it appear as WPA2-protected

#### FR-05: Beacon Flood — Execution

| Property | Description |
|----------|-------------|
| **TX method** | `esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false)` |
| **Rate limiting** | Timer-based: send N beacons per second, evenly spaced |
| **Round-robin** | Cycle through all SSIDs per TX burst (SSID 1, SSID 2, ... SSID N, repeat) |
| **Channel** | Set once at attack start; does not hop during attack |
| **Promiscuous RX** | Remains active — scanner data continues updating in background |
| **Auto-stop** | Stops after configured duration (or infinite until manual stop) |

#### FR-06: Beacon Flood — Display (During Attack)

```
     ╭──────────────────────╮
    ╱   ⚡ BEACON FLOOD ⚡    ╲     ← Magenta border glow
   │  Channel 6                │
   │  ─────────────────────    │
   │  SSIDs:    20 active      │
   │  TX rate:  200 pkt/s      │
   │  Sent:     4,821          │
   │  Duration: 00:24 / 00:30  │
   │                           │
   │  ▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌▌░░░   │   ← Progress bar
   │                           │
   │     [hold = stop]         │
    ╲                        ╱
     ╰──────────────────────╯
```

---

### 3.2 Probe Sniffer

#### FR-07: Probe Sniffer — Purpose

Capture and display probe requests from nearby devices. These reveal what networks devices are searching for (device history leaks). Purely passive — captures frames that are already being broadcast.

#### FR-08: Probe Sniffer — Capture

| Property | Description |
|----------|-------------|
| **Frame type** | Management frames, subtype Probe Request (0x04) |
| **Source** | Existing promiscuous mode callback — add filter for probe requests |
| **Channel** | Can hop channels (use encoder) or lock on one channel |
| **Data extracted** | Source MAC, SSID being probed (may be empty/broadcast), RSSI, timestamp |

#### FR-09: Probe Sniffer — Data Structure

```cpp
struct ProbeRequest {
    uint8_t   src_mac[6];        // Device MAC (may be randomized)
    char      ssid_probed[33];   // Network the device is looking for ("" = broadcast)
    int8_t    rssi;              // Signal strength
    uint32_t  timestamp_ms;      // millis() when captured
    uint8_t   channel;           // Channel where captured
    bool      mac_randomized;    // True if MAC appears to be randomized (bit 1 of first byte set)
};

struct ProbeSnifferState {
    ProbeRequest  probes[100];    // Circular buffer
    uint16_t      write_index;    // Next write position
    uint16_t      total_count;    // Total probes captured (may exceed buffer)
    uint8_t       unique_macs;    // Distinct source MACs seen
    uint8_t       unique_ssids;   // Distinct SSIDs probed
    bool          running;        // Capture active
    bool          channel_hop;    // True = auto-hop, False = locked channel
};
```

#### FR-10: Probe Sniffer — Display

```
     ╭──────────────────────╮
    ╱     PROBE SNIFFER       ╲
   │  CH 6 | 47 probes | 12 dev│   ← Stats bar
   │  ─────────────────────    │
   │  A4:C3:F0:*  → "Telenor" │   ← Real device, real SSID
   │  A4:C3:F0:*  → "eduroam" │   ← Same device, different SSID
   │  DA:23:1F:*  → (broadcast)│   ← Randomized MAC, broadcast probe
   │  DA:23:1F:*  → "Starbucks│   ← Same device leaking history
   │  78:2A:CD:*  → "iPhone-H"│   ← Device looking for own hotspot
   │  ▶ B0:BE:76:* → "Get-5G" │   ← Selected (highlighted)
   │                           │
    ╲  CW/CCW=hop  tap=select ╱
     ╰──────────────────────╯
```

**Display features:**
- Real-time scrolling log (newest at top)
- Source MAC truncated (last 3 octets as `*` for readability)
- Randomized MACs indicated (italic or dimmed — bit 1 of byte 0)
- Encoder CW/CCW hops channel (same as WiFi scanner)
- Touch tap selects entry for detail view
- Grouped by source MAC when same device probes multiple SSIDs

#### FR-11: Probe Sniffer — Detail View

| Field | Content |
|-------|---------|
| Source MAC | Full `XX:XX:XX:XX:XX:XX` |
| MAC type | Real (OUI lookup) or Randomized |
| Vendor | OUI lookup result (if real MAC) |
| SSIDs probed | List of all SSIDs this MAC has probed |
| RSSI range | Weakest — strongest seen |
| First seen | Timestamp |
| Last seen | Timestamp |
| Probe count | Total probes from this MAC |

#### FR-12: Probe Sniffer — Channel Control

| Property | Description |
|----------|-------------|
| **Default** | Auto-hop (same rate as WiFi scanner: 350ms per channel) |
| **Manual** | Encoder CW/CCW locks to specific channel (same as scanner) |
| **Lock indicator** | "LOCKED CH 6" vs "HOPPING" in status bar |
| **Encoder mode** | `ENC_CHANNEL_HOP` (same as WiFi scanner) |

---

### 3.3 Deauthentication Attack

#### FR-13: Deauth — Purpose

Send deauthentication frames to disconnect a target device from its access point. The target device's WiFi connection drops until the attack stops (or the device reconnects and is deauthed again).

#### FR-14: Deauth — Technical Approach (Priority Order)

ESP32-S3 may block deauth frames via `esp_wifi_80211_tx()`. Three approaches, tested in order:

| Priority | Method | API | Risk |
|----------|--------|-----|------|
| 1 | **Rogue AP** | `esp_wifi_set_mac()` + SoftAP | Official API only. No patching. Slower. |
| 2 | **Direct injection** | `esp_wifi_80211_tx()` with deauth subtype | May be blocked by WiFi library |
| 3 | **WSL Bypass / libnet80211 patch** | Overwrite internal function at link-time | S3 compatibility unknown |

**Rogue AP method (preferred):**
1. Clone target AP's BSSID via `esp_wifi_set_mac(WIFI_IF_AP, target_bssid)`
2. Start SoftAP with cloned BSSID on target's channel
3. Target's clients see two APs with same BSSID — causes association confusion
4. Send disassociation frame (less filtered than deauth) or rely on client roaming behavior
5. Repeat rapidly

**Direct injection (if available):**
```cpp
// Deauth frame: 26 bytes
uint8_t deauth_frame[] = {
    0xC0, 0x00,                         // Frame control: deauth
    0x00, 0x00,                         // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Dest: broadcast (or specific client)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source: target AP BSSID
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID: target AP BSSID
    0x00, 0x00,                         // Sequence number
    0x07, 0x00                          // Reason code: Class 3 frame from non-associated
};
```

#### FR-15: Deauth — Target Selection

| Property | Description |
|----------|-------------|
| **Source** | AP list from WiFi scanner |
| **Pre-selection** | If an AP is selected in scanner, it carries over as deauth target |
| **Target type** | AP (deauth all clients) or specific client (if visible) |
| **Multi-target** | Not in Phase 3 — single target only |
| **Visual** | Selected target highlighted in AP list, then shown on attack screen |

#### FR-16: Deauth — Configuration

| Parameter | Input | Default | Range | Step |
|-----------|-------|---------|-------|------|
| Target | Pre-selected from scanner | — | — | — |
| Reason code | Choice | 7 (Class 3) | 1/4/5/6/7/8 | Encoder cycles |
| Packet rate | Value | 10 packets/sec | 1–100 | ±5 |
| Duration | Value | 30 seconds | 5–300 (0 = infinite) | ±5 |
| Scope | Choice | All clients | All clients / Specific client | Encoder cycles |

**Reason codes (802.11):**

| Code | Meaning | Use Case |
|------|---------|----------|
| 1 | Unspecified | Generic |
| 4 | Disassociated due to inactivity | Stealthy — looks like AP timeout |
| 5 | AP unable to handle all associated STAs | Capacity excuse |
| 6 | Class 2 frame from non-authenticated | Technical |
| 7 | Class 3 frame from non-associated | Most effective — default |
| 8 | Disassociated leaving BSS | Looks like client departure |

#### FR-17: Deauth — Execution

| Property | Description |
|----------|-------------|
| **Channel** | Locked to target AP's channel during attack |
| **TX rate** | Configurable packets per second |
| **Frame target** | Broadcast (FF:FF:FF:FF:FF:FF) for all-client, or specific client MAC |
| **Source MAC** | Spoofed to target AP's BSSID |
| **Promiscuous RX** | Remains active — monitors for re-association attempts |
| **Re-association detection** | If promiscuous captures association request to target AP, immediately resend deauth |
| **Auto-stop** | After configured duration |
| **Encoder** | LOCKED during attack (no accidental navigation) |

#### FR-18: Deauth — Display (During Attack)

```
     ╭──────────────────────╮
    ╱    ⚡ DEAUTH ⚡         ╲     ← Red border glow
   │  Target: Telenor-5G      │
   │  BSSID:  C8:2A:14:0B:3E  │
   │  Channel: 6               │
   │  ─────────────────────    │
   │  Packets sent:  312       │
   │  Reconnects:    7         │   ← Detected re-associations
   │  Rate: 10 pkt/s           │
   │  Duration: 00:31 / 00:30  │
   │                           │
   │  ████████████████████░░   │   ← Progress
   │     [hold = stop]         │
    ╲                        ╱
     ╰──────────────────────╯
```

---

### 3.4 Common Attack Infrastructure

#### FR-19: Attack State Machine

```
         ┌─────────┐
         │  IDLE   │
         └────┬────┘
              │ (enter attack screen)
              ▼
         ┌─────────┐
         │ CONFIG  │ ← Encoder adjusts params, display shows config
         └────┬────┘
              │ (touch hold 1s = confirm)
              ▼
         ┌─────────┐
         │ ARMED   │ ← Brief "3-2-1" countdown (1 second)
         └────┬────┘
              │
              ▼
         ┌─────────┐
    ┌───>│ RUNNING │ ← ENC_LOCKED, stats updating, border glow
    │    └────┬────┘
    │         │
    │         ├── timeout/count reached ──────────┐
    │         ├── touch hold 1s (manual stop) ────┤
    │         └── shake (emergency stop) ──── (→ main menu directly)
    │                                             │
    │                                             ▼
    │    ┌─────────────┐
    │    │  COMPLETE   │ ← Show results summary
    │    └─────┬───────┘
    │          │ (tap = dismiss)
    │          ▼
    └──── back to CONFIG (ready for next run)
```

#### FR-20: Attack Safety Features

| Feature | Implementation |
|---------|---------------|
| **Deliberate start** | Touch hold (1s) required — no accidental tap starts attack |
| **Countdown** | 1-second visual countdown after confirmation before TX begins |
| **Encoder lock** | `ENC_LOCKED` during RUNNING — no accidental navigation |
| **Auto-timeout** | All attacks have configurable max duration (default 30s) |
| **Emergency stop** | Shake gesture (Phase 2) immediately halts all TX, returns to main menu |
| **Manual stop** | Touch hold (1s) during RUNNING stops gracefully, shows results |
| **Visual indicator** | Screen border glows magenta (beacon) or red (deauth) during attack |
| **Haptic on start** | Triple click when attack begins |
| **Haptic on stop** | Pulsing when attack completes normally |
| **No auto-start** | Attacks never start on screen entry — always requires explicit config + confirm |

#### FR-21: Attack + Scanner Coexistence

| Property | Description |
|----------|-------------|
| **Goal** | WiFi scanner continues capturing beacons during attacks |
| **Best case** | Promiscuous RX interleaves with TX naturally (verified in pre-phase spike) |
| **Fallback** | Time-division: TX burst → RX window → TX burst (e.g., 80% TX, 20% RX) |
| **Display** | Scanner data shown on attack screen as secondary info (AP count, client count) |
| **BLE** | BLE scanner can remain active during WiFi attacks (independent radio) |

#### FR-22: Attack Persistence Across Navigation

| Property | Description |
|----------|-------------|
| **Leave attack screen** | Attack continues running in background |
| **Visual indicator** | Attack status shown in menu (icon next to WiFi group name) |
| **Return to attack screen** | Shows live attack stats |
| **Backspin during attack** | Opens menu, attack continues; only shake stops it |
| **Settings access** | Settings available during attack (except WiFi region change) |

---

### 3.5 Probe Sniffer Target Forwarding

#### FR-23: Scanner → Attack Target Transfer

| Property | Description |
|----------|-------------|
| **From scanner** | When AP is selected in WiFi Scanner and user navigates to Deauth screen |
| **Transfer** | Target AP's BSSID, SSID, channel, RSSI pre-populate the attack config |
| **No selection** | If no AP selected, Deauth screen shows "Select target" prompt; encoder scrolls internal AP list |
| **From probe sniffer** | Future: selected device MAC could be forwarded as specific deauth target |

---

## 4. User Interaction — Summary

```
┌─────────────────────────────────────────────────────────────────┐
│                    BEACON FLOOD                                   │
│                                                                   │
│  Entry: WiFi group → Beacon Flood                                │
│  Encoder CW/CCW ── Adjust selected parameter                    │
│  Touch tap ──────── Cycle to next parameter                     │
│  Touch hold (1s) ── Start attack (from CONFIG) / Stop (RUNNING) │
│  Shake ────────────── Emergency stop → main menu                │
│                                                                   │
│                    PROBE SNIFFER                                  │
│                                                                   │
│  Entry: WiFi group → Probe Sniffer                               │
│  Encoder CW/CCW ── Hop channel (or scroll if list full)         │
│  Touch tap ──────── Select probe entry                          │
│  Touch hold (1s) ── Show detail for selected entry              │
│  Tap in detail ──── Back to list                                │
│  Backspin ────────── Back to WiFi group menu                    │
│                                                                   │
│                    DEAUTH                                         │
│                                                                   │
│  Entry: WiFi group → Deauth (target from scanner or select)     │
│  Encoder CW/CCW ── Adjust selected parameter                    │
│  Touch tap ──────── Cycle to next parameter                     │
│  Touch hold (1s) ── Start attack (from CONFIG) / Stop (RUNNING) │
│  Shake ────────────── Emergency stop → main menu                │
│                                                                   │
│                    HAPTIC                                         │
│                                                                   │
│  Attack armed (countdown) ── Triple click                        │
│  Attack running (each 100 packets) ── Light tap                 │
│  Attack complete ── Pulsing pattern                              │
│  Emergency stop ── Strong double-pulse                           │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 5. Screen Designs

### 5.1 Beacon Flood — Configuration

```
     ╭──────────────────────╮
    ╱      BEACON FLOOD       ╲
   │  Channel: 6               │
   │  ─────────────────────    │
   │  ▶ SSID count:    [20]   │   ← Selected param (encoder adjusts)
   │    Source:      [Random]   │
   │    TX rate:    [10/s/AP]   │
   │    Duration:     [30 s]    │
   │                           │
   │                           │
   │  Total TX: ~200 pkt/s     │   ← Calculated preview
   │                           │
   │     [hold = START]        │
    ╲                        ╱
     ╰──────────────────────╯
```

### 5.2 Beacon Flood — Running

```
     ╭──────────────────────╮
    ╱   ⚡ BEACON FLOOD ⚡    ╲     ← Magenta border glow (animated)
   │  Channel 6                │
   │  ─────────────────────    │
   │  SSIDs:    20 active      │
   │  TX rate:  198 pkt/s      │
   │  Sent:     4,821          │
   │  Duration: 00:24 / 00:30  │
   │                           │
   │  ████████████████░░░░░    │   ← Progress bar (time-based)
   │                           │
   │  Scanner: 14 APs (+20)    │   ← Fake APs visible to scanner
   │     [hold = STOP]         │
    ╲                        ╱
     ╰──────────────────────╯
```

### 5.3 Beacon Flood — Complete

```
     ╭──────────────────────╮
    ╱      FLOOD COMPLETE     ╲
   │                           │
   │  ─────────────────────    │
   │  Duration:  30.0 s        │
   │  Total TX:  5,940 packets │
   │  Avg rate:  198 pkt/s     │
   │  SSIDs:     20            │
   │                           │
   │  Stopped: timeout         │
   │                           │
   │     [tap = dismiss]       │
   │     [hold = run again]    │
    ╲                        ╱
     ╰──────────────────────╯
```

### 5.4 Deauth — Configuration

```
     ╭──────────────────────╮
    ╱         DEAUTH          ╲
   │  Target: Telenor-5G      │
   │  BSSID:  C8:2A:14:0B:3E  │
   │  Channel: 6   RSSI: -42  │
   │  ─────────────────────    │
   │  ▶ Rate:      [10 pkt/s] │   ← Selected (encoder adjusts)
   │    Duration:  [30 s]      │
   │    Scope:     [All STA]   │
   │    Reason:    [7 - Class3]│
   │                           │
   │                           │
   │     [hold = START]        │
    ╲                        ╱
     ╰──────────────────────╯
```

### 5.5 Deauth — Running

```
     ╭──────────────────────╮
    ╱      ⚡ DEAUTH ⚡       ╲     ← Red border glow (pulsing)
   │  Target: Telenor-5G      │
   │  ─────────────────────    │
   │  Status:  DEAUTHING       │
   │  Packets: 312             │
   │  Reconnects: 7            │   ← Visible re-associations
   │  Rate: 10.0 pkt/s        │
   │  Time: 00:31 / 00:30     │
   │                           │
   │  ██████████████████████   │   ← Progress
   │                           │
   │     [hold = STOP]         │
    ╲                        ╱
     ╰──────────────────────╯
```

### 5.6 Probe Sniffer — Live View

```
     ╭──────────────────────╮
    ╱     PROBE SNIFFER       ╲
   │  CH: HOPPING | 67 probes  │   ← Status: channel mode + total
   │  Devices: 9 | SSIDs: 14   │
   │  ─────────────────────    │
   │  A4:C3:F0:* → Telenor    │   ← 1 sec ago
   │  A4:C3:F0:* → eduroam    │   ← Same device, 2nd SSID
   │  DA:23:1F:* → (broadcast)│   ← Randomized MAC
   │  78:2A:CD:* → iPhone-H   │
   │  ▶ B0:BE:76:* → Get-5G   │   ← Selected
   │  F8:1A:67:* → (broadcast)│
   │                           │
    ╲  dial=CH  tap=select   ╱
     ╰──────────────────────╯
```

### 5.7 Probe Sniffer — Device Detail

```
     ╭──────────────────────╮
    ╱     DEVICE DETAIL       ╲
   │                           │
   │  MAC: A4:C3:F0:9B:21:E7  │
   │  Type: Real (OUI match)  │
   │  Vendor: TP-Link          │
   │  ─────────────────────    │
   │  Probing for:             │
   │    • Telenor              │
   │    • eduroam              │
   │    • HomeNet-5G           │
   │  ─────────────────────    │
   │  Probes: 12 | RSSI: -58  │
   │  First: 00:02:14 ago     │
   │       [tap = back]        │
    ╲                        ╱
     ╰──────────────────────╯
```

---

## 6. Data Structures

### 6.1 Attack State

```cpp
enum AttackType {
    ATTACK_NONE,
    ATTACK_BEACON_FLOOD,
    ATTACK_PROBE_SNIFF,    // Not really an "attack" but uses same state machine
    ATTACK_DEAUTH
};

enum AttackPhase {
    ATTACK_IDLE,           // No attack configured
    ATTACK_CONFIG,         // User adjusting parameters
    ATTACK_ARMED,          // Countdown (1 second)
    ATTACK_RUNNING,        // Active TX
    ATTACK_COMPLETE        // Showing results
};

struct AttackStats {
    uint32_t  packets_sent;
    uint32_t  start_time_ms;
    uint32_t  end_time_ms;
    uint32_t  reconnects_detected;    // Deauth: re-association count
    float     avg_tx_rate;
};

struct AttackState {
    AttackType    type;
    AttackPhase   phase;
    AttackStats   stats;

    // Common params
    uint8_t       channel;
    uint16_t      duration_sec;        // 0 = infinite
    uint16_t      tx_rate;             // Packets per second

    // Beacon-specific
    uint8_t       ssid_count;
    uint8_t       ssid_source;         // 0=random, 1=wordlist, 2=clone
    char          ssids[50][33];       // Generated SSID list
    uint8_t       bssids[50][6];       // Random MAC per SSID

    // Deauth-specific
    uint8_t       target_bssid[6];
    char          target_ssid[33];
    uint8_t       target_channel;
    uint8_t       reason_code;
    uint8_t       scope;               // 0=all clients, 1=specific client
    uint8_t       client_mac[6];       // If scope=specific
};
```

### 6.2 Beacon Frame Template

```cpp
// Pre-built beacon frame (variable length depending on SSID)
struct BeaconTemplate {
    uint8_t   frame[128];         // Max beacon frame size
    uint8_t   frame_len;          // Actual length
    uint8_t   ssid_offset;        // Byte offset where SSID starts in frame
    uint8_t   ssid_len;           // SSID length
    uint8_t   seq_offset;         // Byte offset of sequence number
    uint16_t  seq_number;         // Current sequence (incremented per send)
};
```

### 6.3 Screen Enum Update

```cpp
enum Screen {
    SCREEN_WIFI_SCAN,        // Phase 1
    SCREEN_BLE_SCAN,         // Phase 2
    SCREEN_SETTINGS,         // Phase 2
    SCREEN_DEBUG,            // Phase 2
    SCREEN_BEACON_FLOOD,     // Phase 3
    SCREEN_PROBE_SNIFF,      // Phase 3
    SCREEN_DEAUTH,           // Phase 3
    // SCREEN_PMKID,         // Phase 3+
    // SCREEN_EVIL_PORTAL,   // Phase 3+
    // SCREEN_BLE_SPAM,      // Phase 3+
    // SCREEN_BT_SCAN,       // Phase 4
    // SCREEN_AUDIO_MON,     // Phase 5
    SCREEN_COUNT
};
```

### 6.4 Encoder Mode Update

```cpp
enum EncoderMode {
    ENC_CHANNEL_HOP,         // WiFi/probe: encoder hops channels
    ENC_BLE_LIST,            // BLE: encoder scrolls devices
    ENC_MENU,                // Navigation menus
    ENC_SAFE_LOCK,           // Lock screen
    ENC_SETTINGS,            // Settings edit
    ENC_TARGET_SELECT,       // Phase 3: select attack target from list
    ENC_ATTACK_PARAM,        // Phase 3: adjust attack parameter
    ENC_LOCKED               // Phase 3: during active attack
};
```

---

## 7. Non-Functional Requirements

### NFR-01: Performance

| Requirement | Value |
|-------------|-------|
| Beacon TX latency | <1ms per frame (at 40 MHz SPI, frame is <128 bytes) |
| Max beacon TX rate | 500 packets/sec total (hardware limit TBD) |
| Deauth TX rate | Up to 100 packets/sec configurable |
| Attack start latency | <100ms from confirmation to first TX frame |
| Attack stop latency | <50ms from shake/hold to TX cessation |
| Scanner continues during attack | RX callback fires within 500ms of last TX |
| Screen update during attack | Stats refresh every 500ms |

### NFR-02: Memory Budget (Phase 3 Addition)

| Component | Internal SRAM | Notes |
|-----------|--------------|-------|
| Attack state struct | ~5 KB | 50 SSIDs × 33 bytes + metadata |
| Beacon templates (50) | ~7 KB | Pre-built frames |
| Probe sniffer buffer (100) | ~5 KB | Circular buffer |
| Attack screen LVGL objects | ~8 KB | 3 screens × ~3 KB |
| **Phase 3 subtotal** | **~25 KB** | |
| **Total with Phase 1+2** | **~100 KB** | Still within 310 KB available |
| **Remaining** | **~210 KB** | Comfortable margin |

### NFR-03: Stability

| Requirement | Description |
|-------------|-------------|
| Beacon flood 5 min | 50 SSIDs at max rate for 5 minutes — no crash, no memory leak |
| Deauth 5 min | Max rate for 5 minutes — no crash, stable TX rate |
| Start/stop cycling | Start and stop beacon flood 50 times — no crash or memory leak |
| Emergency stop during TX | Shake while 500 pkt/s TX active — TX stops within 50ms |
| Navigation during attack | Backspin to menu and back while attack runs — attack uninterrupted |
| BLE + WiFi attack concurrent | BLE scanning while beacon flood active — both functional |

### NFR-04: Safety

| Requirement | Description |
|-------------|-------------|
| No accidental start | Touch hold (1s) + 1s countdown = 2 seconds of deliberate intent required |
| No orphaned attacks | If device reboots during attack, it does not auto-resume |
| Timeout default | All attacks default to 30s — infinite requires deliberate change to 0 |
| Encoder locked | Accidental rotation during attack cannot navigate away or change params |
| Visible indicator | Attack running state is unmistakable — colored border glow |

### NFR-05: Maintainability

| Requirement | Description |
|-------------|-------------|
| Attack engine independent of UI | `wifi_attack.cpp` has no LVGL dependency |
| Frame crafting testable | `craft_beacon()` and `craft_deauth()` are pure functions (input → output) |
| Common state machine | All attacks use same `AttackPhase` state machine |
| Tunable constants | TX rate, timeout, countdown duration as `#define` |
| New attacks follow template | Adding PMKID or Evil Portal follows same ScreenDef + AttackState pattern |

---

## 8. Known Limitations

| Limitation | Consequence | Accepted? |
|------------|-------------|-----------|
| ESP32-S3 may block deauth TX | Direct injection fails — must use rogue AP method | Yes — rogue AP is functional alternative |
| Single-target deauth only | Cannot deauth multiple APs simultaneously | Yes — Phase 6 (dual-chip) for multi-target |
| Beacon flood on one channel | Fake APs only visible on one channel | Yes — expected behavior |
| No PCAP export | Cannot save captured probes for offline analysis | Yes — future feature |
| Probe sniffer sees randomized MACs | Cannot uniquely identify most modern phones | Yes — inherent to MAC randomization |
| 100-probe circular buffer | Old probes overwritten in busy environments | Yes — real-time display, not archival |
| No PMF bypass | WPA3/PMF-enabled networks immune to deauth | Yes — hardware/protocol limitation |

---

## 9. Technical Pitfalls and Mitigations

| Pitfall | Source | Mitigation |
|---------|--------|------------|
| `esp_wifi_80211_tx()` blocks | ESP-IDF sends synchronously | Use timer/FreeRTOS task for TX scheduling; never call from main loop directly |
| TX disrupts promiscuous RX | Single radio contention | Test coexistence; implement time-division if needed |
| Deauth blocked on S3 | WiFi library internal filter | Priority: rogue AP method (official API only) |
| Beacon sequence numbers wrong | Each fake AP needs its own counter | Array of `uint16_t seq[50]` per SSID |
| Heap growth during long attacks | Dynamic allocation in TX path | All frames pre-built in CONFIG phase; no allocation during RUNNING |
| Frame buffer corruption | ISR/main loop race | Pre-built frames are read-only during RUNNING; only stats are written |
| Probe sniffer floods buffer | Busy environment fills 100 entries in seconds | Circular buffer; UI shows latest N; total count tracks overflow |
| Attack continues after screen exit | User navigates away, forgets attack is running | Visual indicator in menu (icon/color on WiFi group), periodic haptic reminder |
| Rogue AP deauth interferes with scanner | SoftAP mode changes radio behavior | Test: does promiscuous still work when SoftAP is active? Document result |
| MAC spoofing resets WiFi state | `esp_wifi_set_mac()` may require WiFi restart | Call before attack start, during CONFIG phase |

---

## 10. Acceptance Criteria

Phase 3 is accepted when all of the following are met:

### 10.1 Pre-Phase Validation

- [ ] **AC-01:** Test sketch confirms `esp_wifi_80211_tx()` successfully sends beacon frames (verified with external sniffer)
- [ ] **AC-02:** Test sketch confirms promiscuous RX callback fires during TX loop (coexistence verified)
- [ ] **AC-03:** Deauth delivery method determined and documented (direct / rogue AP / WSL)
- [ ] **AC-04:** Memory stable after 1000 TX frames (heap delta < 1 KB)

### 10.2 Beacon Flood

- [ ] **AC-05:** Beacon flood screen accessible via WiFi group menu
- [ ] **AC-06:** Encoder adjusts SSID count (1–50)
- [ ] **AC-07:** Three SSID source modes available (random, wordlist, clone)
- [ ] **AC-08:** Touch hold (1s) starts attack with 1-second countdown
- [ ] **AC-09:** During attack: border glow visible, stats updating, encoder locked
- [ ] **AC-10:** External device (phone) sees fake SSIDs in WiFi settings
- [ ] **AC-11:** Attack auto-stops after configured duration
- [ ] **AC-12:** Touch hold (1s) during attack stops it manually
- [ ] **AC-13:** Shake gesture immediately stops attack from any state
- [ ] **AC-14:** Results screen shows total packets, duration, average rate
- [ ] **AC-15:** WiFi scanner data continues updating during beacon flood

### 10.3 Probe Sniffer

- [ ] **AC-16:** Probe sniffer screen accessible via WiFi group menu
- [ ] **AC-17:** Captures probe requests from nearby devices in real-time
- [ ] **AC-18:** Displays source MAC (truncated) and probed SSID
- [ ] **AC-19:** Encoder CW/CCW hops channels (same behavior as WiFi scanner)
- [ ] **AC-20:** Touch tap selects entry, touch hold shows detail view
- [ ] **AC-21:** Detail view shows full MAC, vendor (OUI), all SSIDs probed by that device
- [ ] **AC-22:** Randomized MACs identified (bit 1 of first byte) and visually indicated
- [ ] **AC-23:** Circular buffer correctly wraps — oldest entries overwritten, newest shown
- [ ] **AC-24:** Stats bar shows total probes, unique devices, unique SSIDs

### 10.4 Deauthentication

- [ ] **AC-25:** Deauth screen accessible via WiFi group menu
- [ ] **AC-26:** Target pre-populated from WiFi scanner selection (or selectable if none)
- [ ] **AC-27:** Encoder adjusts attack parameters (rate, duration, reason code)
- [ ] **AC-28:** Touch hold (1s) starts attack with countdown
- [ ] **AC-29:** During attack: red border glow, packet count incrementing
- [ ] **AC-30:** Target device actually disconnects from AP (verified on test network)
- [ ] **AC-31:** Re-association detection: counter increments when target reconnects
- [ ] **AC-32:** Attack auto-stops after configured duration
- [ ] **AC-33:** Emergency stop (shake) halts attack within 50ms
- [ ] **AC-34:** No attack starts without explicit touch-hold confirmation

### 10.5 Safety and Stability

- [ ] **AC-35:** Encoder locked during all active attacks — rotation does not navigate
- [ ] **AC-36:** Attack does not auto-resume after device reboot
- [ ] **AC-37:** Beacon flood runs for 5 minutes at maximum rate without crash
- [ ] **AC-38:** Deauth runs for 5 minutes without crash
- [ ] **AC-39:** Start/stop beacon flood 50 times — no memory leak (heap stable)
- [ ] **AC-40:** Navigate away from attack screen and back — attack still running, stats correct
- [ ] **AC-41:** BLE scanner active during WiFi beacon flood — both functional
- [ ] **AC-42:** Heap monitoring shows no growth trend during 5-minute attack

### 10.6 Integration

- [ ] **AC-43:** All three Phase 3 screens appear in WiFi group menu
- [ ] **AC-44:** Navigation (backspin) works correctly to/from all attack screens
- [ ] **AC-45:** Scanner → Deauth target transfer works (selected AP carries over)
- [ ] **AC-46:** Settings still accessible during active attack
- [ ] **AC-47:** WiFi region setting affects channel range for all attack screens

---

## 11. Sequence Diagrams

### 11.1 Beacon Flood Lifecycle

```
User            Screen          AttackEngine    ESP-IDF         Display
  │                │                │              │               │
  │──nav to flood──>│                │              │               │
  │                │──show config───────────────────────────────────>│
  │                │                │              │               │
  │──encoder CW───>│                │              │               │
  │                │──ssid_count++ ─────────────────────────────────>│ update param
  │                │                │              │               │
  │──touch hold───>│                │              │               │
  │                │──attack_start──>│              │               │
  │                │                │──gen SSIDs   │               │
  │                │                │──craft frames│               │
  │                │                │──set channel──>│               │
  │                │                │              │               │
  │                │──countdown(1s)────────────────────────────────>│ "3-2-1"
  │                │  (haptic                      │               │
  │                │   triple)                     │               │
  │                │                │              │               │
  │                │                │──80211_tx────>│               │
  │                │                │──80211_tx────>│  (loop)       │
  │                │                │──80211_tx────>│               │
  │                │                │              │               │
  │                │──stats update──────────────────────────────────>│ every 500ms
  │                │                │              │               │
  │──touch hold───>│                │              │               │
  │  (stop)        │──attack_stop──>│              │               │
  │                │                │──stop TX     │               │
  │                │──show results─────────────────────────────────>│
  │                │  (haptic                      │               │
  │                │   pulse)                      │               │
  │<───────────────────────────────────────────────────────────────│
```

### 11.2 Deauth with Re-association Detection

```
User       AttackEngine     ESP-IDF       Target AP      Target Client
  │             │              │              │               │
  │──start─────>│              │              │               │
  │             │──deauth TX──>│──────────────────────────────>│ disconnect
  │             │              │              │               │
  │             │              │              │    ┌──reconnect─│
  │             │              │              │<───┘            │
  │             │              │              │──assoc resp────>│
  │             │              │              │               │
  │             │  promiscuous │              │               │
  │             │<──assoc req──│              │               │
  │             │  detected!   │              │               │
  │             │──stats.reconnects++         │               │
  │             │──deauth TX──>│──────────────────────────────>│ disconnect again
  │             │              │              │               │
```

### 11.3 Emergency Stop During Attack

```
User            Gesture        Navigation     AttackEngine    Display
  │                │              │                │              │
  │──shake────────>│              │                │              │
  │  (3 reversals) │──SHAKE!─────>│                │              │
  │                │              │──stop_all──────>│              │
  │                │              │                │──halt TX     │
  │                │              │                │──clear state │
  │                │              │                │              │
  │                │              │──show main menu────────────────>│
  │                │  (strong     │                │              │
  │                │   haptic)    │                │              │
  │<─────────────────────────────────────────────────────────────│
```

---

## 12. Tunable Constants

| Constant | File | Default | Range | Effect |
|----------|------|---------|-------|--------|
| `BEACON_MAX_SSIDS` | `wifi_attack.h` | 50 | 1–50 | Maximum concurrent fake SSIDs |
| `BEACON_DEFAULT_RATE` | `wifi_attack.h` | 10 | 1–100 | Beacons per second per SSID |
| `BEACON_MAX_RATE` | `wifi_attack.h` | 500 | — | Total TX packets per second ceiling |
| `DEAUTH_DEFAULT_RATE` | `wifi_attack.h` | 10 | 1–100 | Deauth packets per second |
| `DEAUTH_MAX_RATE` | `wifi_attack.h` | 100 | — | Deauth TX ceiling |
| `ATTACK_DEFAULT_DURATION_S` | `wifi_attack.h` | 30 | 5–300 | Default timeout |
| `ATTACK_COUNTDOWN_MS` | `wifi_attack.h` | 1000 | 500–3000 | Countdown before TX starts |
| `ATTACK_CONFIRM_HOLD_MS` | `wifi_attack.h` | 1000 | 500–2000 | Touch hold duration to start/stop |
| `PROBE_BUFFER_SIZE` | `wifi_probe_sniffer.h` | 100 | 50–500 | Circular buffer entries |
| `PROBE_MAC_RANDOMIZED_BIT` | `wifi_probe_sniffer.h` | 0x02 | — | Bit mask for randomized MAC detection |
| `TX_RX_DUTY_CYCLE_TX` | `attack_common.h` | 80 | 50–95 | TX% in time-division mode (if needed) |
| `ATTACK_HAPTIC_INTERVAL` | `attack_common.h` | 100 | 50–500 | Packets between haptic taps during attack |
| `RECONNECT_DETECT_WINDOW_MS` | `wifi_attack.h` | 5000 | 1000–10000 | Window for counting re-associations |

---

## 13. Memory Budget — Phase 3

### 13.1 Phase 3 Additions

| Component | Internal SRAM | Notes |
|-----------|--------------|-------|
| AttackState struct | ~5 KB | 50 SSIDs + params + stats |
| Beacon templates (50) | ~7 KB | Pre-built 128-byte frames |
| Probe sniffer buffer (100) | ~5 KB | Circular buffer of ProbeRequest structs |
| Deauth frame buffer | <1 KB | Single pre-built frame |
| Attack screens (3 × LVGL) | ~8 KB | Beacon, Probe, Deauth UIs |
| **Phase 3 subtotal** | **~26 KB** | |

### 13.2 Cumulative Budget

```
Total internal SRAM:             512 KB
System baseline:                ~200 KB
Phase 1+2 application:           ~76 KB
Phase 3 application:             ~26 KB
                                ────────
Total used:                     ~302 KB
Remaining:                      ~210 KB
Safety margin:                   ~41%
```

Still comfortable. Phase 4 (BT Classic via secondary ESP) adds no SRAM load on main chip.

---

## 14. Implementation Order

| Step | Deliverable | Validates | Blocks |
|------|-------------|-----------|--------|
| 0 | **Pre-phase spike:** TX+RX coexistence test sketch | Feasibility of entire phase | Everything |
| 1 | Attack common module (`attack_common.cpp`) | State machine, safety layer, timing | Steps 2-4 |
| 2 | Frame crafting functions (`craft_beacon()`, `craft_deauth()`) | Correct 802.11 frame construction | Steps 3, 5 |
| 3 | Beacon flood engine (`wifi_attack.cpp` — beacon portion) | TX at rate, round-robin SSIDs | Step 4 |
| 4 | Beacon flood screen (`scr_beacon_flood.cpp`) | Full UX flow: config → run → results | — |
| 5 | Probe sniffer module (`wifi_probe_sniffer.cpp`) | Capture + parse probe requests | Step 6 |
| 6 | Probe sniffer screen (`scr_probe_sniff.cpp`) | Live display, detail view | — |
| 7 | Deauth engine (method determined by step 0) | Target disconnect verified | Step 8 |
| 8 | Deauth screen (`scr_deauth.cpp`) | Full UX: target select → config → run | — |
| 9 | Scanner → attack target transfer | Target carries from scanner to deauth | — |
| 10 | Attack persistence across navigation | Attack runs while browsing menus | — |
| 11 | Integration + stability testing | All 47 acceptance criteria | — |

---

## 15. Future Extensions (Not Phase 3)

These features build on Phase 3 infrastructure but are out of scope:

| Feature | Depends On | Complexity |
|---------|-----------|------------|
| PMKID Capture | Promiscuous RX for EAPOL frames + deauth for trigger | Medium — frame parsing |
| Evil Portal | SoftAP + HTTP server + captive portal redirect | High — web stack on ESP32 |
| WPA Handshake Capture | Deauth + 4-way EAPOL capture timing | High — timing-critical |
| Multi-target deauth | Array of targets, round-robin TX | Low — extends existing engine |
| Attack profiles/presets | NVS storage of favorite attack configs | Low — settings pattern |
| PCAP export | Write captured frames to NVS or SD card | Medium — storage management |

---

## 16. Changelog

| Date | Version | Change |
|------|---------|--------|
| 2026-05-05 | 1.0 | Initial draft based on NETKNOB-CONCEPT, PHASE2-STARTUP, PHASE1-AAPNE-PUNKT |
