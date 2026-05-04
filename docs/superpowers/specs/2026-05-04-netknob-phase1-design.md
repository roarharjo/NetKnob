# NetKnob Phase 1 — Implementation Design

**Date:** 2026-05-04
**Source FSD:** PHASE1-FSD-EN.md
**Source tech ref:** TECHNICAL_REFERENCE.md

---

## Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Code origin | Written from scratch | Lean, efficient, no dead code from reference project |
| Display driver | Direct QSPI (no esp_lcd_sh8601 component) | ~80 lines vs ~350 lines of abstraction. Same init registers. |
| LVGL version | 9.2 | FSD spec. Reference code is LVGL 8 — all API calls rewritten. |
| Architecture | Single main loop + 3ms encoder timer | Lean for Phase 1. Clean module boundaries allow task extraction in Phase 2+. |
| Touch handling | Two-layer (raw + latch), not LVGL input driver | Pulsing INT pin makes LVGL's input driver unreliable. Custom latch is proven. |
| Haptic library | Library 6, LRA mode | Device has LRA motor, not ERM. Confirmed in tech ref. |

---

## Project Structure

```
netknob/
├── platformio.ini
├── include/
│   ├── pins.h                    ← All GPIO definitions
│   └── lv_conf.h                 ← LVGL 9.2 config
├── src/
│   ├── main.cpp                  ← setup() + loop(), screen dispatch
│   ├── display.cpp / display.h   ← QSPI driver, LVGL init, screen renders
│   ├── encoder.cpp / encoder.h   ← Timer-polled bidi switch
│   ├── touch.cpp / touch.h       ← CST816T with latch + raw layers
│   ├── haptic.cpp / haptic.h     ← DRV2605L LRA driver
│   ├── wifi_scanner.cpp / .h     ← Promiscuous scan, beacon parser, AP list
│   └── interchip.h               ← Reserved EspNowMessage struct (Phase 4)
└── temp_volosr/                  ← Reference code (not compiled)
```

## Build Configuration (`platformio.ini`)

- `board = esp32-s3-devkitc-1`
- `framework = arduino`
- `platform = espressif32@6.6.0`
- PSRAM: OPI mode enabled
- `lib_deps = lvgl/lvgl@^9.2`
- `lv_conf.h` in `include/`: 16-bit color, `LV_COLOR_16_SWAP=1`, Montserrat 14 + 20
- Upload/monitor: COM9, 115200 baud

---

## Module Design

### 1. Display (`display.cpp` / `display.h`)

Three responsibilities:

**1a. QSPI hardware driver (~60 lines)**

- `spi_bus_initialize()` on SPI2_HOST with QSPI config (GPIO 13 clk, 15-18 d0-d3)
- `spi_device_add()`: `command_bits=0, address_bits=0`, flags `SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY`
- Manual CS toggle on GPIO 14 via `GPIO.out_w1tc/w1ts`
- `lcd_cmd(reg, data, len)`: opcode `0x02`, `SPI_TRANS_VARIABLE_CMD|ADDR|DUMMY`, single-line data
- `lcd_color(data, len)`: opcode `0x32`, `SPI_TRANS_MODE_QIO` (quad data lines)
- Init sequence: exact register table from reference `lcd_bsp.c` as static const array (106 entries)
- Hardware reset: GPIO 21, active low, 10ms pulse
- Backlight: GPIO 47 via `ledcWrite()`

**1b. LVGL 9.2 setup (~40 lines)**

- `lv_init()`, `lv_tick_set_cb(millis)`
- `lv_display_create(360, 360)` with flush callback calling `lcd_color()`
- Draw buffer: single PSRAM allocation, `360 * 36` pixels (~25KB)
- Rounder callback: align x/y to even boundaries (required by panel)
- No LVGL input device registered (touch handled by custom latch system)

**1c. Screen rendering (~150 lines)**

Public functions:
- `display_init()` — full hardware + LVGL init
- `display_splash()` — centered "NetKnob" text, held during setup
- `display_wifi_scan(WifiScannerState*)` — main AP list screen
- `display_wifi_detail(AccessPoint*)` — detail overlay
- `display_scanning(uint8_t channel)` — "Scanning..." indicator shown as full screen only when `ap_count == 0 && scanning`. Once APs arrive during the dwell, the AP list screen takes over with a small "Scanning..." label in the header area.
- `display_mark_dirty()` — sets dirty flag
- `display_flush()` — calls `lv_refr_now()` only if dirty, clears flag
- `display_clear()` — nulls ALL static LVGL object pointers

Design rules:
- Static `lv_obj_t*` pointers for all LVGL objects
- ALL pointers nulled in `display_clear()` before rebuilding (prevents dangling pointer crash)
- Arc values clamped to 2-98 (LVGL 9.2 crash at 0 and 100)
- `lv_refr_now()` only called when dirty flag is set (prevents touch INT misses)

**AP list screen layout:**
- Top: channel number (large), frequency in MHz, AP count
- Activity arc: fills proportionally with AP count (0 APs = 2%, 20+ = 98%), clamped 2-98
- List: up to 7 visible rows, sorted by RSSI (strongest top)
- Per row: truncated SSID (left), RSSI bar, dBm value (right)
- Color coding: green > -50 dBm, orange -50 to -70, red < -70
- Hidden networks: `[Hidden]` with BSSID
- Selected AP: highlighted with inverted background or border

**Detail view layout:**
- SSID (large font, centered)
- MAC address, RSSI with color, channel, encryption type, vendor name
- Tap anywhere returns to AP list

---

### 2. Encoder (`encoder.cpp` / `encoder.h`)

Rewrite of reference bidi switch pattern, simplified for single-knob use.

- `encoder_init(uint8_t pin_a, uint8_t pin_b)` — GPIO config with pull-up, starts 3ms `esp_timer`
- Timer callback: state machine per channel — track level, counter-based debounce (2 ticks = 6ms), rising edge = confirmed step
- Single `volatile int8_t encoder_delta` accumulates +1 (CW on pin A) / -1 (CCW on pin B)
- `encoder_get_delta()` — atomically reads and resets delta, called once per loop

~60 lines. No heap, no event groups, no callback registration, no linked list.

---

### 3. Touch (`touch.cpp` / `touch.h`)

Two-layer system:

- `touch_init()` — `Wire.begin(11, 12)` at 400kHz, hardware reset on GPIO 10, CST816T normal mode
- `touch_read()` — gate on INT pin (GPIO 9) LOW, I2C read 6 bytes, updates raw state

**Raw layer (for future button use):**
- `bool touching` — true when `touch_read()` succeeds this frame
- `uint16_t touch_x, touch_y` — coordinates 0-359

**Latch layer (for gestures):**
- `touch_update()` — called each loop after `touch_read()`
- `touch_latch`: stays true for 150ms after last successful read (smooths INT pulsing)
- Tap detection: contact 30ms-1000ms then release (latch drops)
- Hold detection: >1000ms continuous contact, fires once via flag
- `touch_tapped()` — returns true once per tap event
- `touch_held()` — returns true once per hold event

~80 lines.

---

### 4. Haptic (`haptic.cpp` / `haptic.h`)

Minimal DRV2605L driver:

- `haptic_init()` — probe I2C 0x5A on existing Wire bus, mode 0x00 (internal trigger), library 6 (LRA), feedback bit 7 = 1
- `haptic_play(uint8_t effect)` — stop current (GO=0) -> set waveform slot 1 -> set end marker slot 2 -> fire (GO=1). 4 I2C writes.
- `haptic_click()` — effect 1 (Strong Click 100%). Used on channel change.
- `haptic_double_click()` — effect 10 (Double Click 100%). Used on detail view enter.

~40 lines. No cooldown logic — caller decides when to fire.

---

### 5. WiFi Scanner (`wifi_scanner.cpp` / `wifi_scanner.h`)

Pure data producer. No LVGL dependency.

**Data structures (from FSD):**

```cpp
struct AccessPoint {
    char     ssid[33];       // 32 chars + null
    uint8_t  bssid[6];       // MAC address
    int8_t   rssi;           // dBm
    uint8_t  channel;        // 1-13
    uint8_t  encryption;     // 0=OPEN, 1=WEP, 2=WPA, 3=WPA2, 4=WPA3
    bool     hidden;         // true if SSID empty
};

struct WifiScannerState {
    uint8_t       current_channel;
    AccessPoint   ap_list[32];
    uint8_t       ap_count;
    uint8_t       selected_index;
    bool          scanning;
    bool          detail_view;
    uint32_t      scan_start_ms;
};
```

**Interface:**
- `scanner_init()` — `WiFi.mode(WIFI_STA)`, `esp_wifi_set_promiscuous(true)`, register callback, start channel 1
- `scanner_set_channel(uint8_t ch)` — clear AP list, `esp_wifi_set_channel(ch)`, set `scanning=true`, record `scan_start_ms`
- `scanner_update()` — called each loop: drain ring buffer, parse beacons, sort by RSSI, check dwell elapsed (350ms) -> set `scanning=false`
- `scanner_get_state()` — returns `WifiScannerState*` for display to read

**Promiscuous callback (ISR context):**
- Filter: `WIFI_PKT_MGMT` only
- Subtype: beacon (0x80) or probe response (0x50)
- Action: copy raw frame + RSSI into ring buffer (8 slots, ~256 bytes each)
- No parsing in callback — ISR safety

**Beacon parsing (main loop via `scanner_update()`):**
- Drains ring buffer each call
- Extracts from 802.11 header: BSSID at bytes 16-21
- Walks tagged IEs in beacon body:
  - Tag 0: SSID (up to 32 chars), flag hidden if empty
  - Tag 3: DS Parameter Set (channel)
  - Tag 48: RSN IE (WPA2/WPA3)
  - Tag 221 + Microsoft OUI: WPA
  - No RSN + no WPA: check capability field bit 4 for WEP vs OPEN

**Deduplication:**
- Key: BSSID (6-byte memcmp)
- Same BSSID in same dwell: keep strongest RSSI
- List full (32): discard if weaker than weakest

**OUI lookup:**
- `const char* oui_lookup(const uint8_t bssid[6])` — static const table (~20 entries from FSD), linear scan, returns "Unknown" on miss

~200 lines total.

---

### 6. Main Loop (`main.cpp`)

**`setup()` sequence:**
1. `display_init()` — SPI bus, LVGL, panel init, backlight on
2. `display_splash()` — "NetKnob" centered text, flush
3. `encoder_init(8, 7)` — GPIO config, 3ms timer starts
4. `touch_init()` — Wire.begin, CST816T hardware reset
5. `haptic_init()` — DRV2605L probe + LRA config
6. `scanner_init()` — WiFi STA, promiscuous mode, channel 1
7. `delay(1500)` — hold splash
8. `display_clear()` — null pointers, prepare for scanner screen

**`loop()` — free-running:**
1. `touch_read()` — always first (catch INT pulses)
2. `touch_update()` — latch debounce, tap/hold
3. `delta = encoder_get_delta()`
4. State logic:
   - **Scanner mode**: encoder delta changes channel (wrap 1-13), tap cycles selected AP, hold opens detail view
   - **Detail mode**: tap returns to scanner mode
   - Haptic: click on channel change, double-click on detail enter
5. `scanner_update()` — drain ring buffer, parse, sort
6. Dirty check on scanner state changes
7. Render if dirty: call appropriate display function, then `display_flush()`
8. `lv_timer_handler()`
9. Serial debug heartbeat every 5s

**Reserved stubs:**
- `interchip.h`: `struct EspNowMessage { uint8_t type; uint8_t data[32]; };`
- `void on_secondary_esp_message()` — empty function in main.cpp
- `enum Screen` with `SCREEN_WIFI_SCAN` and commented Phase 2-7 slots

**Enums:**
```cpp
enum EncoderMode {
    ENC_CHANNEL_HOP,      // Phase 1
    ENC_TARGET_SELECT,    // Phase 2+
    ENC_SCREEN_SWITCH,    // Phase 2+
    ENC_LOCKED            // During active attack
};

enum Screen {
    SCREEN_WIFI_SCAN,
    // SCREEN_DEAUTH,        // Phase 2
    // SCREEN_BEACON_FLOOD,  // Phase 2
    // SCREEN_BLE_SCAN,      // Phase 3
    // SCREEN_BT_SCAN,       // Phase 4
    // SCREEN_AUDIO_MON,     // Phase 5
    // SCREEN_DUAL_STATUS,   // Phase 6
    // SCREEN_BOOT_MENU,     // Phase 7
    SCREEN_COUNT
};
```

---

## Configurable Constants

All `#define` in one place (either `pins.h` or top of relevant module):

| Constant | Value | Location |
|----------|-------|----------|
| `CHANNEL_MIN` | 1 | wifi_scanner.h |
| `CHANNEL_MAX` | 13 | wifi_scanner.h |
| `DWELL_TIME_MS` | 350 | wifi_scanner.h |
| `MAX_APS_PER_CHANNEL` | 32 | wifi_scanner.h |
| `RING_BUFFER_SLOTS` | 8 | wifi_scanner.cpp |
| `TOUCH_LATCH_MS` | 150 | touch.cpp |
| `TOUCH_TAP_MIN_MS` | 30 | touch.cpp |
| `TOUCH_TAP_MAX_MS` | 1000 | touch.cpp |
| `TOUCH_HOLD_MS` | 1000 | touch.cpp |
| `ENCODER_TICK_US` | 3000 | encoder.cpp |
| `ENCODER_DEBOUNCE` | 2 | encoder.cpp |
| `SERIAL_HEARTBEAT_MS` | 5000 | main.cpp |
| `SPLASH_DURATION_MS` | 1500 | main.cpp |

---

## Data Flow

```
Encoder (3ms timer)
      |
      v
encoder_delta (+1/-1)
      |
      v (main loop reads)
scanner_set_channel(ch) ---------> esp_wifi_set_channel()
                                          |
                                          v
                                   promiscuous callback (ISR)
                                          |
                                          v
                                   ring buffer (8 frames)
                                          |
                                          v (main loop drains)
                                   scanner_update() -> parse -> sort
                                          |
                                          v
                                   WifiScannerState
                                          |
                                          v (main loop reads)
Touch (raw+latch) ---------> main loop state logic
                                          |
                                          v
                              display_wifi_scan() / _detail() / _scanning()
                                          |
                                          v
                              display_flush() -> lv_refr_now() -> lcd_color() -> QSPI
```

---

## Init Order Dependencies

```
display_init()    ← SPI bus (must be first — longest init)
display_splash()  ← needs display
encoder_init()    ← independent (GPIO + timer)
touch_init()      ← Wire.begin (must precede haptic)
haptic_init()     ← needs Wire from touch_init
scanner_init()    ← WiFi (independent of display/input)
```

---

## Known Pitfalls and Mitigations

| Pitfall | Mitigation |
|---------|------------|
| LVGL 9.2 arc crash at 0/100 | Clamp all arc values to 2-98 |
| Dangling LVGL pointers on screen rebuild | `display_clear()` nulls ALL static pointers |
| Touch INT pulses, not level | Two-layer system: 150ms latch for gestures, raw for buttons |
| Render blocking kills touch | Only `lv_refr_now()` when dirty flag set |
| Promiscuous callback in ISR context | Callback only copies to ring buffer; parsing in main loop |
| Haptic I2C disrupts touch | No touch-down haptic; only fire on confirmed events (channel change, detail enter) |
| Display uses board-specific init registers | Exact register table from reference `lcd_bsp.c`, not generic values |
| LVGL animations don't auto-render | No animations used; all visual updates driven manually |

---

## Acceptance Criteria Coverage

All 17 acceptance criteria from the FSD are covered:
- AC-01 to AC-10: Core functionality via main loop state logic + scanner + display
- AC-11 to AC-14: Stability via clamped arcs, nulled pointers, dirty-flag rendering, serial debug
- AC-15 to AC-17: Future prep via interchip.h, Screen enum, on_secondary_esp_message stub
