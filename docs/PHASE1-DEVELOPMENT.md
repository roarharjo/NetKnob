# Phase 1 Development Log

> Full development history for the NetKnob Phase 1 build.
> Covers every major decision, every hardware fight, and how each was resolved.

---

## Session Overview

Built Phase 1 from scratch in a single extended session:

1. Hardware research and pin mapping from datasheets and community repos
2. PlatformIO project scaffold with correct board config
3. QSPI display driver (custom, direct SPI peripheral)
4. LVGL 9.2 integration with frame buffer in PSRAM
5. Rotary encoder driver (interrupt-based)
6. Capacitive touch driver (CST816 over I2C)
7. Haptic driver (DRV2605L LRA over I2C)
8. WiFi promiscuous mode scanner with channel hopping
9. AP list view with RSSI bars and colour coding
10. AP detail view with OUI vendor lookup
11. Arc animation and splash screen
12. Code review pass — 4 critical bugs identified and fixed
13. Visual polish: neon/cyber aesthetic, matrix rain splash, live updates

---

## Architecture Decisions

### Direct QSPI vs esp_lcd Component

The ESP-IDF `esp_lcd` component supports QSPI in theory but requires exact register
sequences at driver level that Espressif has not documented for ST77916. Community
attempts to use `esp_lcd_st77916` produce garbled or blank screens.

Decision: write a direct SPI peripheral driver using `spi_device_handle_t` and manual
CS toggling on GPIO 14. This mirrors the approach in the only confirmed-working community
reference (`temp_volosr/`).

### Single Main Loop vs Tasks

Phase 1 uses a single-threaded Arduino loop. LVGL is not thread-safe without mutex
guards, and the sensor polling cadence (encoder: per-loop, WiFi events: via callback,
display: every 5 ms via `lv_timer_handler`) does not require parallelism.

FreeRTOS tasks are reserved for Phase 2+ if latency becomes an issue.

### LVGL 9.2

LVGL 9.x is the current stable branch. Version 9.2 specifically is used because 9.5
(latest at time of writing) introduced API changes to arc widget constructors and
colour handling that are not yet reflected in community examples. Using `~9.2.0` in
`lib_deps` pins to the last 9.2.x patch.

---

## Hardware Bring-Up Challenges

### PSRAM Configuration Causing Boot Loops

The ESP32-S3-Knob uses OPI PSRAM (Octal SPI, not standard QPI). Three `platformio.ini`
flags must all be set together or the chip boot-loops with no serial output:

```ini
board_build.psram_type = opi
board_build.arduino.memory_type = qio_opi
build_flags = -DBOARD_HAS_PSRAM
```

Missing any one of these produces an MMU fault at startup. The symptom with no serial
output made this extremely hard to diagnose — the backlight blink test (flash the
backlight in a minimal sketch before any PSRAM access) was used to confirm whether the
chip was booting at all.

### QSPI Framing

Standard SPI sends one bit per clock. QSPI sends four. The ST77916 expects:
- A 1-byte command phase over single SPI (CMD bit = 1 in the control byte)
- A data phase over 4-wire SPI (DAT bit = 1)

The SPI bus must be configured with `flags = SPI_DEVICE_HALFDUPLEX` and the
transaction structures must set `.flags = SPI_TRANS_MODE_QIO` for data phases.
Getting this wrong produces a screen that receives power but shows solid colour or
random noise.

### COLMOD Register

The ST77916 defaults to 18-bit colour. LVGL is configured for 16-bit (RGB565).
The COLMOD register (0x3A) must be written with value `0x55` at display init or
every pixel is misinterpreted and the display shows corrupted colour bands.

### lv_conf.h vs Build Flags

LVGL 9.x looks for `lv_conf.h` on the include path by default. If not found and
`LV_CONF_SKIP=1` is not defined, it uses internal defaults that differ from what the
project needs (wrong colour depth, missing fonts, monitor overlays enabled).

Decision: set `-DLV_CONF_SKIP=1` in `build_flags` and define every required LVGL
constant as a build flag. This avoids maintaining a separate config file and makes the
configuration explicit and auditable in `platformio.ini`.

---

## Display Driver Development

### The Garbled Screen Issue

First attempt at the QSPI driver produced a screen that displayed content but:
- Colours were wrong (green channel and blue channel swapped)
- Pixels were shifted horizontally by ~8 columns
- Right edge content appeared on the left

Root cause: two separate problems.

1. Colour swap: LVGL `LV_COLOR_16_SWAP=1` must be set because the SPI peripheral sends
   bytes MSB-first but LVGL packs RGB565 LSB-first. Without this flag every pixel has
   its high and low bytes swapped.

2. Pixel shift: the SPI bus clock speed was set too high (80 MHz). The QSPI traces on
   this board cannot sustain 80 MHz reliably. Dropping to 40 MHz eliminated the shift.
   The reference repo (`temp_volosr/`) uses 40 MHz — this was the fix found by
   comparing the two configs line by line.

### Fixing SPI Bus Config to Match Working Repo

The `temp_volosr/` reference was fetched and diffed against the initial driver.
Key differences:

| Parameter | Initial | Fixed |
|---|---|---|
| `clock_speed_hz` | 80 000 000 | 40 000 000 |
| `max_transfer_sz` | 4096 | 360*360*2 (full frame) |
| CS handling | SPI peripheral | Manual GPIO |
| Command bits | 8 | 32 (full QSPI command word) |

After applying all four changes the display rendered correctly.

---

## Touch Driver Bug

The CST816 touch controller returns touch coordinates starting at I2C register `0x03`.
The initial driver read from `0x00`, which is the chip ID register. Reading 4 bytes
from `0x00` returns: chip ID, firmware version, and 2 bytes of the first coordinate —
but the coordinate bytes are offset by 2 from their correct positions.

Result: X coordinates were read as Y and vice versa, and both were scaled incorrectly.

Fix: change the read register from `0x00` to `0x03`. X and Y then read correctly
without any swap or scaling correction needed.

---

## Code Review Findings

A code review pass before device testing found 4 critical bugs:

### Bug 1 — Detail View Memory Leak

`display_show_detail()` called `lv_obj_create(lv_screen_active())` on every invocation
without first deleting the previous detail screen objects. After visiting 5-10 APs and
returning to list view repeatedly, LVGL heap was exhausted and the display froze.

Fix: call `lv_obj_clean(lv_screen_active())` at the start of `display_show_detail()`
before creating any new objects.

### Bug 2 — Dangling Pointer on AP List Rebuild

`display_update_ap_list()` stored raw pointers to `APInfo` structs in LVGL label user
data. When the scanner rebuilt the AP list (e.g., a new AP was inserted in sorted
order), those pointers became invalid. Tapping a label after a list rebuild caused a
hard fault.

Fix: store the AP's BSSID string (stack-copied) as user data instead of a pointer.
On tap, look up the AP by BSSID through the scanner API.

### Bug 3 — Encoder Delta Atomicity

`encoder_get_delta()` read and cleared the delta accumulator as two separate operations
without disabling interrupts. A rotation interrupt arriving between read and clear
silently dropped counts.

Fix: wrap read-and-clear in `portENTER_CRITICAL` / `portEXIT_CRITICAL`.

### Bug 4 — Missing LVGL Include in wifi_scanner.cpp

`wifi_scanner.cpp` used `lv_color_t` for RSSI colour computation but did not include
`<lvgl.h>`. The file compiled on a build where another translation unit happened to
include LVGL first (due to link order), but would fail on a clean build on another
machine.

Fix: add `#include <lvgl.h>` explicitly to `wifi_scanner.cpp`.

---

## Visual Design Evolution

### Initial Layout

First pass: plain white text on black background. Functional but unreadable on a round
360x360 display — square text blocks clip against the circular bezel.

### Neon/Cyber Aesthetic

Switched to a dark grey background (`#1A1A2E`) with accent colours:
- Cyan (`#00FFFF`) — channel indicator, selected AP
- Green (`#00FF41`) — strong signal (RSSI > -60)
- Yellow (`#FFD700`) — medium signal (-60 to -75)
- Red (`#FF4136`) — weak signal (< -75)
- Dim grey (`#404040`) — inactive list items

All text uses Montserrat (LVGL built-in). 28pt for the channel number, 16pt for AP
names, 14pt for detail fields.

### Arc Animation

A full-circle arc widget spans the outer bezel. The arc endpoint angle maps to the
current channel (channel 1 = 0 degrees, channel 13 = 360 degrees). Rotation triggers
smooth interpolation over 200 ms rather than jumping to the new angle.

### Matrix Rain Splash Screen

The splash screen renders a short matrix-style rain effect using LVGL labels with
random green characters falling from the top. Duration: 1500 ms. Replaced with the
main scan view on completion.

### Live RSSI Updates

Rather than rebuilding the full AP list on every scan cycle, the list view uses a
dirty-flag pattern. RSSI bar widths and signal colour labels update in-place if the AP
is already visible. Full rebuild only occurs when the AP count or sort order changes.

---

## Data Flow Bugs Found and Fixed

### selected_index vs Sort Order

When the encoder scrolled the AP list, `selected_index` was an index into the
unsorted internal array. After a scan cycle that changed sort order, the highlighted
row no longer matched the intended AP.

Fix: `selected_index` now indexes into the sorted display list, which is recomputed
on each scan cycle. Selection is preserved by BSSID across sorts.

### Arc Base Value Not Applied

The arc widget's base angle was set at creation but not stored. On the next LVGL
screen refresh, LVGL reset the arc to its default angle. The arc appeared to jump on
first encoder movement.

Fix: store the base angle in a static variable and reapply it in the arc's event
callback.

### SSID / RSSI Mismatch in Detail View

The detail view received the AP index at tap time. If a scan cycle completed between
tap and render, the index pointed to a different AP (list had been re-sorted). SSID
showed correctly (the label text was already set) but RSSI and channel showed the
new AP at that index.

Fix: detail view looks up the AP by BSSID, not by index.

---

## AP Aging Mechanism

APs that stop transmitting do not immediately disappear — they age out over a
configurable timeout (`AP_AGING_TIMEOUT_MS`, default 30 000 ms). Each AP record
stores `last_seen_ms` (updated on each received beacon/probe response). The scan loop
marks APs as `stale` when `millis() - last_seen_ms > AP_AGING_TIMEOUT_MS` and removes
them on the next list rebuild.

Stale APs are shown in the list with dimmed colour before removal, giving the user
visual feedback that signal has been lost.

---

## Arc Interpolation

`display_update_arc()` is called on every loop iteration. It compares the current arc
angle to the target angle (from the current channel) and steps toward it by
`ARC_STEP_DEG` per call (default 6 degrees, ~200 ms to traverse full range at 30 Hz).
This avoids a jarring jump while keeping the display responsive.

---

## Current State

All 17 Phase 1 acceptance criteria are met:

1. Device boots reliably to splash screen
2. Display renders at correct 360x360 resolution with no artefacts
3. Encoder rotation hops WiFi channel 1-13
4. Channel indicator updates immediately on rotation
5. Haptic pulse fires on each encoder detent
6. WiFi scanner captures beacon and probe-response frames
7. AP list populates with real APs within 3 seconds of boot
8. AP list sorts by RSSI descending
9. RSSI bars render with correct colour coding
10. Tapping an AP opens the detail view
11. Detail view shows SSID, BSSID, channel, RSSI, estimated distance, OUI vendor
12. Back navigation from detail returns to list at the same scroll position
13. Haptic pulse fires on view transition
14. APs age out and are removed after 30 seconds of no signal
15. Stale APs are visually dimmed before removal
16. Arc animation tracks channel position smoothly
17. All of the above survive a 5-minute continuous run without crash or memory leak
