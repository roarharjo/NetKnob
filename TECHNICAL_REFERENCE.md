# Waveshare ESP32-S3-Knob-Touch-LCD-1.8 — Technical Reference

> Comprehensive guide to programming this device. Covers every hardware interface,
> what works, what doesn't, and the patterns that solved each problem.
> Written 2026-05-02 after two sessions of hands-on development.

---

## Device Overview

Round smart knob with 360x360 IPS display, rotary encoder dial, capacitive touch,
haptic feedback motor, and WiFi. Based on ESP32-S3 with 16MB flash and 8MB PSRAM.

**Two ESP chips on board.** The USB-C plug orientation selects which chip is connected:
- One side: ESP32-S3 (main, COM9, VID 303A) — display, touch, encoder, WiFi
- Flipped: ESP32 (secondary, COM7, CH340) — BT, audio

If your device doesn't respond, flip the USB-C cable.

---

## Pin Mapping

| Function | GPIO | Notes |
|---|---|---|
| LCD CLK | 13 | QSPI clock |
| LCD D0-D3 | 15, 16, 17, 18 | QSPI data lines |
| LCD CS | 14 | Manual GPIO toggle (not SPI peripheral) |
| LCD RST | 21 | Active low, 10ms pulse at init |
| LCD Backlight | 47 | PWM via ledcWrite |
| I2C SDA | 11 | Shared: touch (0x15) + haptic (0x5A) |
| I2C SCL | 12 | **NOT GPIO 10** (common mistake) |
| Touch INT | 9 | Pulsing LOW on data ready (not level!) |
| Touch RST | 10 | Active low |
| Encoder A | 8 | CW rotation channel |
| Encoder B | 7 | CCW rotation channel |

---

## 1. Display — ST77916 over QSPI

### What You Need to Know

The display uses QSPI (4 data lines, no DC pin). No standard library supports this
correctly out of the box. You must use a custom SPI driver.

### What Does NOT Work

| Approach | Result |
|---|---|
| Arduino_GFX `Panel_ST77916` | Garbled — wrong QSPI opcode framing |
| LovyanGFX `Panel_ST77916` | No QSPI support |
| `command_bits=8, address_bits=24` | Garbled |
| `command_bits=32` | Fails — ESP-IDF 4.4 cmd is uint16_t |

### What Works — ESPHome QSPI Approach

SPI device: `command_bits=0, address_bits=0`, flags `SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY`.

Each transaction uses `SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY`.

**Register write** (single command byte to display):
```
cmd_bits=8, cmd=0x02, addr_bits=24, addr=(register << 8)
data = register value(s), single-line SPI
```

**Pixel write** (bulk image data):
```
cmd_bits=8, cmd=0x32, addr_bits=24, addr=(0x2C << 8)
data = pixel buffer, SPI_TRANS_MODE_QIO (quad data lines)
```

CS is toggled manually via `GPIO.out_w1tc` / `GPIO.out_w1ts` (not the SPI peripheral).

### Init Sequence — Waveshare Specific

**Critical:** The init register values are unique to this board. Using generic ST77916
defaults from Arduino_GFX or Espressif produces wrong colors, garbled output, or blank screen.

The correct init is in `temp_volosr/KnobRGBControl/lcd_bsp.c` (`lcd_init_cmds` array).
Key differences from generic defaults:
- `0xF0=0x28` (not `0x08`)
- `0x73=0xF0`, `0x7C=0xD1`
- Completely different page-0x10 timing registers
- Different gamma tables
- `0xB8=0x86`, `0xDD/0xDE=0x4F`

The demo uses `esp_lcd_sh8601` (SH8601-compatible), NOT `esp_lcd_st77916`.

### LVGL Integration

- LVGL 9.2, 16-bit color, byte-swapped (`LV_COLOR_16_SWAP=1`)
- Partial rendering with PSRAM buffer (21600 bytes)
- Explicit `lv_refr_now()` to push frames (only when data changes!)
- `lv_timer_handler()` every loop iteration for internal timers
- Montserrat font bundled with LVGL only includes ASCII — no ae/oe/aa without custom font build

### LVGL Animations

`lv_anim_*` API compiles and creates animation objects, but animations do not render
on this setup. Suspected cause: `lv_timer_handler()` processes animations but does not
trigger display refresh for animation-modified objects. `lv_refr_now()` forces only a
single static frame. Workaround: drive visual changes manually from the main loop
(e.g., the volume knob arc is updated each frame by `display_draw_knob()`).

### Gotcha: Dangling LVGL Pointers

When switching screens, `rebuild_screen()` calls `lv_obj_del(scr)` which destroys all
children. **Every** static `lv_obj_t*` pointer to objects on that screen must be nulled.
Missing one causes `LoadProhibited` crash on the next label/object update.

```cpp
// WRONG — lbl_debug is not reset, becomes dangling pointer
lbl_time = lbl_date = lbl_weather = lbl_info = NULL;

// RIGHT
lbl_time = lbl_date = lbl_weather = lbl_info = lbl_debug = NULL;
```

### Gotcha: Render Blocking Kills Touch

`lv_refr_now()` takes several milliseconds. If called every loop iteration, the main
loop runs too slowly to catch CST816T INT pulses, making touch unresponsive.
**Only call `lv_refr_now()` when screen data actually changes.**

---

## 2. Rotary Encoder — Bidirectional Switch

### What You Need to Know

This is **NOT** a quadrature encoder. It is a bidirectional switch:
- CW rotation: only channel A (GPIO 8) pulses
- CCW rotation: only channel B (GPIO 7) pulses
- Channels are independent — no phase relationship between A and B

### What Does NOT Work

| Approach | Result |
|---|---|
| GPIO interrupts (ISR) | Unreliable — timing conflicts with LVGL/SPI/I2C |
| Falling edge detection | Wrong polarity — must use rising edge |
| 50ms time-based debounce | Too slow, misses fast turns |
| `ESP32Encoder` library | Quadrature library, wrong for bidi switch |
| PCNT hardware | Quadrature design, overkill |

### What Works — Timer-Polled (Official Waveshare Pattern)

Direct port of `temp_demo/.../bidi_switch_knob.c`:

**`esp_timer`** fires every **3ms** (`ESP_TIMER_TASK` dispatch).

**State machine per channel** (`process_channel()`):
1. Read GPIO level
2. LOW and changed → reset debounce counter
3. LOW and unchanged → increment counter (track LOW duration)
4. HIGH and changed → if counter >= 2: confirmed rising edge → +1 or -1
5. HIGH and changed but counter < 2 → bounce, ignore
6. HIGH and unchanged → reset counter

Constants: `TICKS_INTERVAL = 3ms`, `DEBOUNCE_TICKS = 2` (6ms effective).

**Why timer, not loop polling?** The timer runs independently. LVGL render, WiFi HTTP
requests, and I2C transactions can block the main loop for 10-100ms. The 3ms timer
fires regardless, so no encoder steps are missed.

### Screen Switching

Delta-based: `enc_count - last_switch`. Each step switches one screen.
Wraps with `(idx + 1) % SCREEN_COUNT` and `(idx - 1 + SCREEN_COUNT) % SCREEN_COUNT`.

**Gotcha:** If code changes `currentScreen` directly (e.g., a "back" button), you must
also call `encoder_set_screen(target)` to sync the encoder's internal index. Otherwise
the stale encoder index overwrites `currentScreen` on the next loop.

### Reference Code
- `temp_demo/.../bidi_switch_knob.c` — Official Waveshare driver
- `temp_krx3d/.../rotary_encoder_custom.cpp` — ESPHome variant (loop polling)

---

## 3. Touch — CST816T Capacitive Controller

### What You Need to Know

I2C address 0x15, 400kHz. Shares bus with haptic motor (0x5A).

**Critical:** The INT pin (GPIO 9) **pulses** LOW when touch data is ready. It does NOT
stay LOW for the duration of the touch. During finger contact, INT pulses LOW for
~5-30ms then goes HIGH, repeating with gaps of ~10-50ms.

### Reading Data

```cpp
if (digitalRead(PIN_TOUCH_INT) != LOW) return false;  // gate
Wire.beginTransmission(0x15);
Wire.write(0x01);
Wire.endTransmission();
Wire.requestFrom(0x15, 6);
// data[1] = finger count (0 = not touching)
// X = ((data[2] & 0x0F) << 8) | data[3]
// Y = ((data[4] & 0x0F) << 8) | data[5]
```

Coordinates: 0-359 both axes on the 360x360 display.

### The INT Pulsing Problem

Because INT pulses, `touch_read()` returns `true` intermittently during a continuous
touch. Raw code that checks `if (touch_read()) { ... }` will see rapid true/false
flickering — NOT a clean "finger down" / "finger up" transition.

This means:
- You cannot use a single `bool isTouching` for gesture detection
- "Tap" detection based on touching→not-touching transition fires on INT gaps, not real lifts
- Duration measurement is unreliable with raw touch_read()

### Solution: Two-Layer Touch System

**Layer 1 — Latch (for gestures: tap, hold)**

Smooths INT pulses with 150ms gap detection:
- `latch_touching`: set true on any `touch_read()==true`, stays true for 150ms after
  last successful read
- `latch_x/latch_y`: updated only when `touch_read()` returns true
- Finger-lift detection: `!touching && latch_touching && (now - last_touch > 150ms)`
- Tap: finger-lift after 30ms-1000ms contact
- Hold: detected WHILE finger is down after 1000ms (fires once via flag)

**Layer 2 — Raw (for responsive buttons)**

Use `touching` + `tx/ty` directly from `touch_read()` for UI buttons. This gives
the same responsiveness as the touch test screen. Button action fires once per touch
via an `acted` flag, reset when `!touching`.

```cpp
// Responsive button check — use raw touch, not latch
if (touching && ty > 220) {
    btn_zone = (tx >= 180) ? BACK : MUTE;
    if (!btn_acted) {
        btn_acted = true;
        // handle button press
    }
}
if (!touching) btn_acted = false;
```

**Why two layers?** The latch adds 150ms latency (worst case) which is acceptable for
"tap to refresh" but makes buttons feel sluggish. Raw touch gives instant response but
can't detect gestures (hold, tap duration) because of INT flickering.

### Performance Rules

1. **Never call `lv_refr_now()` every loop** — blocks for ms, misses INT pulses
2. **Haptic I2C writes can disrupt touch reads** — skip touch-down haptic on screens
   that have button-specific haptic
3. **Only redraw on data changes** — loop must run fast to catch INT pulses
4. **Button cooldowns** — if needed, use an `acted` flag reset on `!touching`, not
   a timer (timers interact badly with INT pulsing)

---

## 4. Haptic — DRV2605L Motor Driver

### What You Need to Know

I2C address 0x5A. The device has an **LRA** (Linear Resonance Actuator) motor,
not ERM. Using the wrong motor mode makes most effects imperceptible.

### Configuration

```
Mode:    0x00 (internal trigger — play on GO command)
Library: 6   (LRA-optimized waveforms)
Motor:   LRA (feedback register bit 7 = 1)
```

This matches the KrX3D ESPHome configuration (`temp_krx3d/.../drv2605.yaml`).

### What Does NOT Work

| Config | Result |
|---|---|
| ERM mode, library 1 | Only effect 1 (Strong Click) felt. Ticks, buzzes imperceptible |
| Playing without stopping first | New effect may not start |
| Library 1-5 with LRA | Some effects work, many don't |

### What Works

```cpp
// Always stop → set effect → fire
drv_write(REG_GO, 0);           // stop current
drv_write(REG_WAVESEQ1, effect); // effect 1-123
drv_write(REG_WAVESEQ1+1, 0);   // end marker
drv_write(REG_GO, 1);           // fire
```

**Reliable effects (library 6, LRA):**
| Effect | # | Use |
|---|---|---|
| Strong Click 100% | 1 | General feedback, encoder clicks |
| Strong Click 60% | 2 | Lighter feedback |
| Strong Click 30% | 3 | Subtle feedback |
| Double Click 100% | 10 | Confirmations, back navigation |
| Strong Buzz 100% | 14 | Alerts |

### I2C Bus Sharing with Touch

Touch and haptic share the bus. Each `haptic_play()` does 4 I2C write transactions
(stop + 3 writes). This can cause the next `touch_read()` to miss data. Mitigations:
- Skip touch-down haptic on screens with button-specific haptic
- 300ms cooldown between touch-down haptics on other screens
- Button haptic fires once per touch (acted flag), not repeatedly

---

## 5. I2C Bus Architecture

Single bus, two devices:

| Device | Address | Role |
|---|---|---|
| CST816T | 0x15 | Touch controller |
| DRV2605L | 0x5A | Haptic motor driver |

Initialized once: `Wire.begin(11, 12)` at 400kHz in `touch_init()`.
Haptic uses the same `Wire` instance. No bus arbitration issues in single-threaded
Arduino context. Both are accessed only from the main loop (encoder uses timer, not I2C).

---

## 6. Main Loop Architecture

### Init Order (matters!)
```
display_init()   → SPI bus, LVGL, panel init sequence
encoder_init()   → GPIO config, esp_timer 3ms start
touch_init()     → Wire.begin(11,12), CST816T hardware reset
haptic_init()    → DRV2605 probe + LRA config (needs Wire from touch_init)
wifi_init()      → WiFi.begin() non-blocking
ntp_init()       → configTzTime()
weather_init()   → no-op
```

### Loop Structure
```
 1. wifi_check()           — reconnect if dropped
 2. touch_read()           — I2C read (only if INT LOW)
 3. touch_update()         — legacy tap/hold (used on non-interactive screens)
 4. latch debounce         — 150ms smoothing of INT pulses
 5. hold detection         — fires after 1000ms while finger down
 6. encoder_poll()         — no-op (timer handles it)
 7. screen-specific logic  — volume buttons OR encoder screen switching
 8. weather_fetch()        — HTTP GET every 10 min
 9. serial heartbeat       — status every 5s
10. screen draw            — interval-gated, switch/case per screen
11. display_flush()        — lv_refr_now() (skipped for animation screens)
12. lv_timer_handler()     — LVGL tick
```

### Draw Intervals
| Screen | Interval | Why |
|---|---|---|
| Klokke | 1000ms | Time updates per second |
| Vaer | 1000ms | Data from API every 10 min |
| Diagnostikk | 1000ms | System stats |
| Touch Test | 50ms | Responsive coordinate display |
| Volum | 50ms | Responsive button feedback |

---

## 7. Build Environment

```
Platform:    espressif32@6.6.0 (ESP-IDF 4.4 based)
Framework:   Arduino
Board:       esp32-s3-devkitc-1
Flash:       16MB QIO
PSRAM:       8MB OPI
LVGL:        9.2.x
ArduinoJson: 7.x
```

```bash
pio run -e knob                        # Build
pio run -e knob --target upload        # Build + flash (COM9)
pio device monitor -p COM9 -b 115200   # Serial monitor
```

RAM usage: ~34%, Flash: ~37%

---

## 8. Current Application State (v2, 2026-05-02)

### 5 Screens (Norwegian UI)
1. **KLOKKE** — NTP tid (24t), norsk dato, vaersammendrag
2. **VAER** — Temperatur og tilstand fra Open-Meteo, norske vaerbeskrivelser
3. **DIAGNOSTIKK** — WiFi signal, minne, oppetid, sensorverdier
4. **TOUCH TEST** — Live touch-koordinater, trykk/hold-hendelser, bevegelig prikk
5. **VOLUM** — Interaktiv arc-knapp styrt av encoder, mute/tilbake-knapper

### Working
- Encoder: reliable CW/CCW, 1 step per detent
- Touch: coordinates, tap, hold, responsive buttons
- Haptic: click on encoder, click on touch buttons
- Display: LVGL 9 via custom QSPI driver
- WiFi: auto-connect, NTP time sync
- Weather: Open-Meteo API with Norwegian translations

### Known Limitations
- LVGL animations don't auto-render (manual updates needed)
- Montserrat font: ASCII only (no ae/oe/aa)
- Touch button hit rate depends on loop speed
- Factory firmware was not backed up

---

## 9. Reference Files

| File | What It Contains |
|---|---|
| `temp_volosr/KnobRGBControl/lcd_bsp.c` | Correct display init sequence |
| `temp_demo/.../bidi_switch_knob.c` | Official encoder driver source |
| `temp_demo/.../bidi_switch_knob.h` | Encoder API definition |
| `temp_krx3d/.../rotary_encoder_custom.cpp` | ESPHome encoder (alt approach) |
| `temp_krx3d/.../drv2605.yaml` | Haptic config (LRA, library 6) |
| `temp_demo/.../03_DRV2605_Test/` | Official haptic demo |
| `temp_demo/.../04_Encoder_Test/` | Official encoder demo |
| `temp_esphome/.../spi_esp_idf.cpp` | ESPHome QSPI write function |

---

## 10. Lessons Learned

### 1. Encoder: Never Use Interrupts for Bidi Switches
Timer polling at 3ms with counter-based debounce is the only reliable approach.
ISR timing conflicts with LVGL, SPI, and I2C make interrupt-based detection erratic.
Port the Waveshare `bidi_switch_knob.c` directly.

### 2. Touch INT Pin Pulses, Not Stays Low
Any code assuming "INT LOW = finger touching" will fail. You need a debounce latch
with ~150ms gap detection for gestures, and raw touch for responsive buttons.

### 3. Render Blocking Kills Touch Responsiveness
`lv_refr_now()` every loop blocks for milliseconds. Touch INT pulses are missed.
Only render when data changes. This was the #1 cause of "unresponsive" touch.

### 4. This Device Uses LRA, Not ERM
Wrong motor mode makes haptic effects imperceptible. Use library 6, LRA mode.
Confirmed by KrX3D ESPHome config.

### 5. Null ALL LVGL Pointers on Screen Rebuild
`lv_obj_del()` frees children but doesn't null your static pointers. One missed
pointer = `LoadProhibited` crash on next update.

### 6. Sync Encoder When Changing Screens Programmatically
If you set `currentScreen` from a button handler, also call `encoder_set_screen()`.
Otherwise the encoder's stale index overwrites your change next loop.

### 7. Haptic I2C Writes Disrupt Touch Reads
They share one bus. Each `haptic_play()` is 4 I2C writes. Skip touch-down haptic
on screens that have their own button haptic. Use cooldowns elsewhere.

### 8. Raw Touch for Buttons, Latch for Gestures
The 150ms latch debounce makes buttons feel sluggish. Raw `touching` + coordinates
gives instant response. Reserve the latch for tap/hold gesture detection.

### 9. Display Init is Board-Specific
Generic ST77916 init tables from ANY library produce garbled output. Only the
Waveshare demo's init sequence works. Copy it exactly.

### 10. USB-C Orientation Selects Chip
If COM port doesn't appear or wrong chip responds, flip the USB-C cable.
ESP32-S3 = VID 303A. ESP32 = CH340.
