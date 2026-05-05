# NetKnob

Multi-radio scanner and security tool on the Waveshare ESP32-S3 Knob.

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
| Connectivity | WiFi 802.11 b/g/n, BLE 5.0 (single chip) |

The board has two ESP chips. USB-C orientation selects which is connected:
- Primary face: ESP32-S3 (COM9) — display, WiFi, BLE, encoder, touch, haptics
- Flipped: ESP32 (COM7) — Bluetooth Classic, audio (reserved for Phase 4+)

---

## Features

### Phase 1 — WiFi Scanner
- Passive WiFi scanning via promiscuous mode — no packets transmitted
- Channel hopping (1-14) controlled by rotary encoder with region setting
- AP list view with live RSSI bars and colour-coded signal strength
- Detail view per AP: channel, RSSI, BSSID, encryption, OUI vendor lookup
- AP aging — networks fade and expire when no longer heard

### Phase 2 — Navigation, BLE, Settings, Lock
- **Navigation system**: gesture-driven multi-screen architecture
  - Backspin (fast CCW flick) opens main menu from any screen
  - Shake (3+ rapid reversals) triggers emergency stop
  - 3-level hierarchy: Main Menu → Group Menu → Screen
- **BLE Scanner**: NimBLE passive scanning with device classification
  - Name-based and protocol-based manufacturer identification
  - Apple subtype parsing (AirPods, iBeacon, Find My, AirDrop)
  - Device type detection via appearance, service UUIDs, and name patterns
- **Settings**: 9 NVS-persisted settings (lock, WiFi region, brightness, haptic, etc.)
- **Safe-Lock**: 3-digit combination lock (0-39 dial) with SHA-256 hash storage
  - Escalating lockout (1s, 5s, 30s) on failed attempts
  - Auto-lock on inactivity timeout
- **Debug screen**: real-time heap, PSRAM, WiFi/BLE status, uptime
- **Heap monitoring**: 10s periodic serial logging with threshold alerts

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
| Display driver | Custom QSPI driver (direct SPI peripheral) |
| WiFi | ESP-IDF promiscuous mode API |
| BLE | NimBLE-Arduino 1.4.x (passive scanning) |
| Storage | ESP-IDF NVS (Non-Volatile Storage) |
| Crypto | mbedtls SHA-256 (lock code hashing) |

---

## Project Structure

```
NetKnob/
├── platformio.ini              # Build config, LVGL + NimBLE flags
├── TECHNICAL_REFERENCE.md      # Hardware deep-dive: pins, gotchas
├── src/
│   ├── main.cpp                # Setup, main loop, gesture→navigation dispatch
│   ├── display.cpp/.h          # QSPI driver, LVGL setup, shared colour palette
│   ├── encoder.cpp/.h          # Timer-polled encoder with event stream
│   ├── touch.cpp/.h            # CST816 capacitive touch (tap + hold)
│   ├── haptic.cpp/.h           # DRV2605L LRA haptic driver
│   ├── wifi_scanner.cpp/.h     # Promiscuous WiFi scanner + beacon parser
│   ├── ble_scanner.cpp/.h      # NimBLE passive BLE scanner
│   ├── gesture.cpp/.h          # Encoder velocity, backspin, shake detection
│   ├── navigation.cpp/.h       # Screen lifecycle + state machine
│   ├── settings.cpp/.h         # NVS settings + lock code management
│   ├── safe_lock.cpp/.h        # Combination lock logic
│   ├── heap_monitor.cpp/.h     # Periodic heap logging + alerts
│   ├── interchip.h             # ESP-NOW message types (Phase 4+)
│   ├── pins.h                  # GPIO pin definitions
│   └── screens/
│       ├── scr_main_menu.cpp/.h    # Main menu (WiFi/BLE/System)
│       ├── scr_group_menu.cpp/.h   # Group menu (screens within group)
│       ├── scr_wifi_scan.cpp/.h    # WiFi scanner screen
│       ├── scr_ble_scan.cpp/.h     # BLE scanner screen
│       ├── scr_settings.cpp/.h     # Settings editor
│       ├── scr_safe_lock.cpp/.h    # Safe-lock dial screen
│       └── scr_debug.cpp/.h        # System debug/heap screen
├── include/
│   └── pins.h
└── docs/
    ├── PHASE1-DEVELOPMENT.md
    ├── PHASE1-HANDOVER.md
    ├── PHASE2-FSD-EN.md
    ├── PHASE2-HANDOVER.md
    └── SESSION-REVIEW-P2.md
```

---

## Memory Budget (Phase 2)

```
Internal SRAM:  37.9% used (124 KB / 328 KB)
Flash:          36.7% used (1.2 MB / 3.3 MB)
```

Comfortable headroom for Phase 3 features (WiFi attacks, active BLE).

---

## Legal

NetKnob is built for educational purposes and authorised security research.

Passive WiFi and BLE scanning captures publicly broadcast frames and advertisements.
No data is stored persistently. No packets are injected or transmitted (Phases 1-2).

Use only on networks and in environments you are authorised to test.
The author accepts no liability for misuse.
