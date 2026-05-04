# NetKnob

WiFi channel scanner with dial control on Waveshare ESP32-S3 Knob.

---

## Hardware

**Waveshare ESP32-S3-Knob-Touch-LCD-1.8**

| Spec | Value |
|---|---|
| Display | 360x360 IPS, QSPI interface, ST77916 controller |
| MCU | ESP32-S3, 16 MB flash, 8 MB OPI PSRAM |
| Input | Rotary encoder dial |
| Touch | Capacitive touchscreen (CST816) |
| Haptics | LRA motor via DRV2605L (I2C) |
| Connectivity | WiFi 802.11 b/g/n (single chip) |

The board has two ESP chips. USB-C orientation selects which is connected:
- Primary face: ESP32-S3 (COM9) — display, WiFi, encoder, touch, haptics
- Flipped: ESP32 (COM7) — Bluetooth, audio (reserved for later phases)

---

## Features (Phase 1)

- Passive WiFi scanning via promiscuous mode — no packets transmitted
- Channel hopping (1-13) controlled by rotary encoder
- AP list view with live RSSI bars and colour-coded signal strength
- Detail view per AP: channel, RSSI, BSSID, estimated distance, OUI vendor lookup
- AP aging — networks fade and expire when no longer heard
- Haptic feedback on encoder rotation and view transitions
- Smooth arc animation reflecting current channel position
- Neon/cyber UI aesthetic built on LVGL 9.2

---

## Phase 1 Scope

Phase 1 is passive observation only.

Out of scope for Phase 1 (reserved for later phases):
- Deauthentication or any active attacks (Phase 2)
- BLE/Bluetooth scanning (Phase 3-4)
- Secondary ESP32 communication (Phase 4)
- Audio monitoring (Phase 5)
- Persistent storage / SD card logging

---

## Build

**Requirements:** PlatformIO (VS Code extension or CLI), USB-C to the primary face of the device.

```bash
# Build and flash
pio run -e knob --target upload

# Monitor serial output
pio device monitor --port COM9 --baud 115200
```

Default upload port: `COM9`. Change in `platformio.ini` if your port differs.

---

## Tech Stack

| Layer | Choice |
|---|---|
| Build system | PlatformIO, espressif32 platform 6.6.0 |
| Framework | Arduino + ESP-IDF |
| UI library | LVGL 9.2.x |
| Display driver | Custom QSPI driver (direct SPI peripheral, no esp_lcd component) |
| WiFi | ESP-IDF promiscuous mode API |

LVGL is configured entirely via `build_flags` in `platformio.ini` (no `lv_conf.h` file needed).

---

## Project Structure

```
NetKnob/
├── platformio.ini          # Build configuration, LVGL flags, upload settings
├── TECHNICAL_REFERENCE.md  # Hardware deep-dive: pins, gotchas, what does/doesn't work
├── PHASE1-FSD-EN.md        # Phase 1 functional specification
├── src/
│   ├── main.cpp            # Setup, main loop, screen state machine
│   ├── display.cpp/.h      # QSPI driver, LVGL integration, all screen rendering
│   ├── encoder.cpp/.h      # Rotary encoder with interrupt-driven delta accumulation
│   ├── touch.cpp/.h        # CST816 capacitive touch over I2C
│   ├── haptic.cpp/.h       # DRV2605L LRA haptic driver over I2C
│   ├── wifi_scanner.cpp/.h # Promiscuous mode scanner, AP list, channel management
│   ├── interchip.h         # ESP-NOW message types for secondary ESP32 (Phase 4+)
│   └── pins.h              # All GPIO pin definitions in one place
├── include/
│   └── pins.h              # (see src/pins.h)
├── docs/
│   ├── PHASE1-DEVELOPMENT.md  # Development log: decisions, bugs, solutions
│   ├── PHASE1-HANDOVER.md     # Handover guide for future contributors
│   └── SESSION-REVIEW.md      # Session retrospective
└── temp_volosr/            # Reference implementation from Waveshare community repo
```

---

## Legal

NetKnob is built for educational purposes and authorised security research.

Passive WiFi scanning (promiscuous mode) captures management frames broadcast publicly
by access points. No data is stored. No packets are injected or transmitted.

Use only on networks and in environments you are authorised to test.
The author accepts no liability for misuse.
