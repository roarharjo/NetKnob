# Phase 1 Handover Guide

> For anyone picking up this codebase after Phase 1.
> Covers build, architecture, known gotchas, and where to start for Phase 2.

---

## How to Build and Flash

**Prerequisites**
- PlatformIO CLI or VS Code with the PlatformIO extension
- USB-C cable connected to the primary face of the device (ESP32-S3 side)
- Device appears as COM9 (change `upload_port` in `platformio.ini` if different)

```bash
# Build only
pio run -e knob

# Build and flash
pio run -e knob --target upload

# Monitor serial output
pio device monitor --port COM9 --baud 115200

# Build, flash, and monitor in one command
pio run -e knob --target upload && pio device monitor --port COM9 --baud 115200
```

If the flash fails with "no serial data received", flip the USB-C cable. The board
has two ESP chips and USB-C orientation selects which one is connected. See the
hardware gotchas section below.

---

## File Structure and Responsibilities

```
src/
├── main.cpp            Entry point. Setup sequence, main loop, screen/encoder
│                       state machine. EncoderMode and Screen enums live here.
├── display.cpp/.h      Everything visual. QSPI driver init, LVGL setup, frame
│                       buffer management, all screen rendering functions.
├── encoder.cpp/.h      Interrupt-driven rotary encoder. Call encoder_get_delta()
│                       each loop iteration to get accumulated tick count.
├── touch.cpp/.h        CST816 capacitive touch over I2C. Returns tap coordinates.
├── haptic.cpp/.h       DRV2605L LRA haptic driver over I2C. Call haptic_buzz()
│                       with an effect ID (1-123 per DRV2605L datasheet).
├── wifi_scanner.cpp/.h Promiscuous mode scanner. Manages AP list, channel state,
│                       aging, RSSI updates, OUI lookup.
├── interchip.h         Message struct definitions for ESP-NOW comms with the
│                       secondary ESP32 (COM7). Not used in Phase 1.
└── pins.h              Single source of truth for all GPIO numbers.
```

---

## Known Hardware Gotchas

These are not bugs in the code — they are hardware behaviours that will surprise you
if you do not know about them. Full details in `TECHNICAL_REFERENCE.md`.

### Touch INT Pin Pulses, Does Not Hold

GPIO 9 (Touch INT) pulses LOW when touch data is ready. It does not stay LOW.
Do not use `digitalRead` polled in a tight loop — you will miss events.
Either use an interrupt (`attachInterrupt`) or read the I2C data register
unconditionally each loop and check the touch count byte.

### Haptic Motor is LRA, Not ERM

The DRV2605L on this board is wired for an LRA (Linear Resonant Actuator) motor,
not a standard ERM (Eccentric Rotating Mass). If you call `drv.setMode(DRV2605_MODE_INTTRIG)`
without first calling `drv.useLRA()`, the motor will not respond or will respond
weakly. The mode byte in the DRV2605L register 0x1D must have bit 7 set for LRA.
`haptic_init()` handles this — do not reinitialise the DRV2605L without checking.

### Board-Specific Display Init Registers

The ST77916 initialisation sequence includes several undocumented vendor registers
(addresses above 0xB0). Do not simplify or remove these — they are required for
this specific panel. Removing them produces a white screen or colour corruption.
The full sequence is in `display.cpp` and mirrors the `temp_volosr/` reference.

### USB-C Orientation Selects the Active Chip

- Primary face (COM9, VID 303A): ESP32-S3 — this is the chip you flash for Phase 1
- Flipped (COM7, CH340): ESP32 — Bluetooth and audio, reserved for Phase 4+

If your port does not appear or the device does not respond to flash, flip the cable.
The board has no indicator for which chip is active.

### I2C SCL is GPIO 12, Not GPIO 10

GPIO 10 is `Touch RST`. The I2C clock is GPIO 12. This is contrary to the labelling
on some community pinout diagrams. Verify against `pins.h` before changing any I2C
configuration.

---

## Architecture

### Main Loop Flow

```
setup()
  display_init()
  display_splash() + display_animate_splash()
  encoder_init()
  touch_init()
  haptic_init()
  scanner_init()
  display_clear()
  display_mark_dirty()

loop()
  lv_timer_handler()          -- LVGL tick (call every ~5 ms)
  delta = encoder_get_delta()
  if delta != 0:
    update channel / scroll selection
    haptic_buzz()
    display_mark_dirty()
  touch = touch_get_event()
  if touch:
    handle tap (list item -> detail view, back button -> list view)
    haptic_buzz()
    display_mark_dirty()
  scanner_tick()              -- process pending WiFi events, age APs
  if dirty:
    rebuild or update current screen
    dirty = false
  delay(5)
```

### Dirty Flag Pattern

`display_mark_dirty()` sets a boolean that is checked at the bottom of each loop
iteration. This prevents redundant screen redraws when nothing has changed, which
matters because a full LVGL screen rebuild takes ~15 ms on this hardware.

### Two Levels of Display Update

1. **Full rebuild** (`display_show_wifi_scan()`, `display_show_detail()`): clears all
   objects on the active screen and recreates them. Used when switching views or when
   AP count/sort order changes.

2. **Live update** (`display_update_ap_list()`, `display_update_rssi()`): updates
   only the changed properties (label text, bar width, colour) on existing objects.
   Used every scan cycle to refresh RSSI without visual flicker.

The scan loop calls the live update path unless `scanner_list_changed()` returns true,
in which case it falls through to a full rebuild.

---

## How to Add a New Screen (Phase 2+)

1. Add an entry to the `Screen` enum in `main.cpp`:
   ```c
   enum Screen {
       SCREEN_WIFI_SCAN,
       SCREEN_DEAUTH,   // <-- add here
       SCREEN_COUNT
   };
   ```

2. Write a render function in `display.cpp` / `display.h`:
   ```c
   void display_show_deauth(const char *target_bssid);
   ```
   Start the function with `lv_obj_clean(lv_screen_active())` to clear the previous
   screen before building the new one.

3. Wire it into the main loop in `main.cpp`:
   ```c
   if (currentScreen == SCREEN_DEAUTH) {
       display_show_deauth(selected_bssid);
   }
   ```

4. Add a transition trigger — typically a long-press, a touch zone, or an encoder
   button press — that sets `currentScreen` and calls `display_mark_dirty()`.

---

## Constants to Tune

All timing and visual constants are `#define` at the top of their respective files.

| Constant | File | Default | Effect |
|---|---|---|---|
| `CHANNEL_DWELL_MS` | `wifi_scanner.h` | 200 | Time spent on each channel before hopping (ms) |
| `AP_AGING_TIMEOUT_MS` | `wifi_scanner.h` | 30000 | Time before an AP is removed after last beacon (ms) |
| `AP_STALE_WARN_MS` | `wifi_scanner.h` | 15000 | Time before an AP is dimmed as stale (ms) |
| `ARC_STEP_DEG` | `display.cpp` | 6 | Degrees the channel arc moves per loop iteration |
| `SPLASH_DURATION_MS` | `main.cpp` | 1500 | How long the splash screen shows (ms) |
| `RSSI_STRONG` | `wifi_scanner.h` | -60 | RSSI threshold for green colour coding (dBm) |
| `RSSI_MEDIUM` | `wifi_scanner.h` | -75 | RSSI threshold for yellow colour coding (dBm) |

---

## What is Reserved for Future Phases

### interchip.h

Defines `EspNowMessage` struct and message type constants for ESP-NOW communication
with the secondary ESP32 (COM7). The `on_secondary_esp_message()` callback stub is
in `main.cpp`. Not wired up in Phase 1.

### Screen Enum

`SCREEN_DEAUTH`, `SCREEN_BEACON_FLOOD`, `SCREEN_BLE_SCAN`, `SCREEN_BT_SCAN`,
`SCREEN_AUDIO_MON`, `SCREEN_DUAL_STATUS`, `SCREEN_BOOT_MENU` are commented out in
the enum. Uncomment as each phase is implemented.

### EncoderMode Enum

`ENC_TARGET_SELECT`, `ENC_SCREEN_SWITCH`, `ENC_LOCKED` are defined but not used.
Phase 1 uses only `ENC_CHANNEL_HOP`. The enum is in place to avoid a structural
refactor in Phase 2.

---

## Dependencies

```ini
platform = espressif32@6.6.0
framework = arduino
lib_deps =
    lvgl/lvgl@~9.2.0
```

Pin this platform version. `espressif32@6.7+` changes the PSRAM initialisation
sequence in a way that causes a boot loop on this board unless additional flags are
set. If you must upgrade, test on the device before committing.

LVGL `~9.2.0` resolves to the latest 9.2.x patch. Avoid upgrading to 9.3+ without
checking the LVGL migration guide — arc widget and colour API changed in 9.3.
