# NetKnob Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a WiFi channel scanner with dial control on the Waveshare ESP32-S3 Knob, rendering discovered APs on a 360x360 circular LVGL display.

**Architecture:** Single main loop with 3ms encoder timer. Six modules (display, encoder, touch, haptic, wifi_scanner, main) with clean data boundaries. WiFi promiscuous callback writes to a ring buffer; main loop parses and renders. No network connections — passive scanning only.

**Tech Stack:** PlatformIO, Arduino + ESP-IDF calls, LVGL 9.2, ESP32-S3 with QSPI display (ST77916), CST816T touch, DRV2605L haptic, `esp_wifi` promiscuous mode.

**References:**
- Design spec: `docs/superpowers/specs/2026-05-04-netknob-phase1-design.md`
- FSD: `PHASE1-FSD-EN.md`
- Hardware lessons: `TECHNICAL_REFERENCE.md`
- Reference init registers: `temp_volosr/KnobRGBControl/lcd_bsp.c`
- Reference encoder driver: `temp_volosr/KnobRGBControl/bidi_switch_knob.c`

**Testing approach:** This is embedded firmware — no desktop unit tests. Each task ends with "build, flash, verify on device" via serial monitor and visual inspection. Verification steps describe exactly what to look for.

---

## File Map

| File | Responsibility | Task |
|------|---------------|------|
| `platformio.ini` | Build config, board, libs, flags | 1 |
| `include/pins.h` | All GPIO pin definitions | 1 |
| `include/lv_conf.h` | LVGL 9.2 configuration | 1 |
| `src/main.cpp` | setup(), loop(), screen dispatch | 1, 3-9 |
| `src/display.h` | Display public interface | 2 |
| `src/display.cpp` | QSPI driver, LVGL init, screen rendering | 2, 7, 8 |
| `src/encoder.h` | Encoder public interface | 3 |
| `src/encoder.cpp` | Timer-polled bidi switch driver | 3 |
| `src/touch.h` | Touch public interface | 4 |
| `src/touch.cpp` | CST816T two-layer driver | 4 |
| `src/haptic.h` | Haptic public interface | 5 |
| `src/haptic.cpp` | DRV2605L LRA driver | 5 |
| `src/wifi_scanner.h` | Scanner types + public interface | 6 |
| `src/wifi_scanner.cpp` | Promiscuous scan, parser, OUI table | 6 |
| `src/interchip.h` | Reserved EspNowMessage struct | 9 |

---

## Task 1: Project Scaffold

**Files:**
- Create: `platformio.ini`
- Create: `include/pins.h`
- Create: `include/lv_conf.h`
- Create: `src/main.cpp`

- [ ] **Step 1: Create `platformio.ini`**

```ini
[env:knob]
platform = espressif32@6.6.0
board = esp32-s3-devkitc-1
framework = arduino
board_build.mcu = esp32s3
board_build.f_cpu = 240000000L
board_build.flash_mode = qio
board_build.flash_size = 16MB
board_build.partitions = default_16MB.csv
board_build.arduino.memory_type = qio_opi
upload_port = COM9
monitor_port = COM9
monitor_speed = 115200
lib_deps =
    lvgl/lvgl@^9.2
build_flags =
    -DLV_CONF_INCLUDE_SIMPLE
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_CDC_ON_BOOT=1
```

- [ ] **Step 2: Create `include/pins.h`**

```cpp
#pragma once

// Display — ST77916 QSPI
#define PIN_LCD_CLK   13
#define PIN_LCD_D0    15
#define PIN_LCD_D1    16
#define PIN_LCD_D2    17
#define PIN_LCD_D3    18
#define PIN_LCD_CS    14
#define PIN_LCD_RST   21
#define PIN_LCD_BL    47

// I2C — shared bus: touch (0x15) + haptic (0x5A)
#define PIN_I2C_SDA   11
#define PIN_I2C_SCL   12

// Touch — CST816T
#define PIN_TOUCH_INT 9
#define PIN_TOUCH_RST 10
#define TOUCH_I2C_ADDR 0x15

// Haptic — DRV2605L
#define HAPTIC_I2C_ADDR 0x5A

// Encoder — bidirectional switch
#define PIN_ENC_A     8
#define PIN_ENC_B     7

// Display constants
#define LCD_H_RES     360
#define LCD_V_RES     360
```

- [ ] **Step 3: Create `include/lv_conf.h`**

```cpp
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 1

#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (48U * 1024U)

#define LV_DEF_REFR_PERIOD 33

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_LOG 0
#define LV_USE_SYSMON 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS 0
#define LV_USE_DEMO_MUSIC 0

#endif
```

- [ ] **Step 4: Create `src/main.cpp`**

```cpp
#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("NetKnob Phase 1 — booting...");
}

void loop() {
}
```

- [ ] **Step 5: Build to verify scaffold compiles**

Run: `pio run -e knob`
Expected: Successful build with LVGL library downloaded and compiled. No errors.

- [ ] **Step 6: Flash and verify**

Run: `pio run -e knob --target upload && pio device monitor -p COM9 -b 115200`
Expected: Serial output shows `NetKnob Phase 1 — booting...`

- [ ] **Step 7: Commit**

```
feat: project scaffold with platformio, pins, LVGL config
```

---

## Task 2: Display — QSPI Driver + LVGL Init + Splash

**Files:**
- Create: `src/display.h`
- Create: `src/display.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: Create `src/display.h`**

```cpp
#pragma once

#include "wifi_scanner.h"

void display_init();
void display_splash();
void display_clear();
void display_mark_dirty();
void display_flush();
void display_wifi_scan(WifiScannerState *state);
void display_wifi_detail(AccessPoint *ap);
void display_scanning(uint8_t channel);
```

Note: `display_wifi_scan`, `display_wifi_detail`, and `display_scanning` are declared here but implemented in Task 7/8. For now, create a forward-declaration-safe minimal `wifi_scanner.h` stub (see step 2).

- [ ] **Step 2: Create temporary `src/wifi_scanner.h` stub**

This stub lets `display.h` compile. It will be replaced with the real scanner in Task 6.

```cpp
#pragma once

#include <stdint.h>

#define MAX_APS_PER_CHANNEL 32
#define CHANNEL_MIN 1
#define CHANNEL_MAX 13
#define DWELL_TIME_MS 350

struct AccessPoint {
    char     ssid[33];
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  encryption;  // 0=OPEN, 1=WEP, 2=WPA, 3=WPA2, 4=WPA3
    bool     hidden;
};

struct WifiScannerState {
    uint8_t       current_channel;
    AccessPoint   ap_list[MAX_APS_PER_CHANNEL];
    uint8_t       ap_count;
    uint8_t       selected_index;
    bool          scanning;
    bool          detail_view;
    uint32_t      scan_start_ms;
};
```

- [ ] **Step 3: Create `src/display.cpp`**

This is the largest file. It contains: QSPI hardware driver, LVGL 9.2 init, splash screen, and stubs for scan/detail screens (filled in Task 7/8).

```cpp
#include "display.h"
#include "pins.h"

#include <Arduino.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "soc/gpio_struct.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

// ─── QSPI Hardware Driver ───────────────────────────────────────────

#define LCD_SPI_HOST SPI2_HOST
#define LCD_FREQ_HZ  (40 * 1000 * 1000)

#define CS_LOW()  do { GPIO.out_w1tc.val = (1UL << PIN_LCD_CS); } while(0)
#define CS_HIGH() do { GPIO.out_w1ts.val = (1UL << PIN_LCD_CS); } while(0)

static spi_device_handle_t spi_dev;

// Init register table — exact values from Waveshare reference (lcd_bsp.c)
struct LcdInitCmd {
    uint8_t reg;
    uint8_t data[14];
    uint8_t len;
    uint16_t delay_ms;
};

static const LcdInitCmd lcd_init_cmds[] = {
    {0xF0, {0x28}, 1, 0},
    {0xF2, {0x28}, 1, 0},
    {0x73, {0xF0}, 1, 0},
    {0x7C, {0xD1}, 1, 0},
    {0x83, {0xE0}, 1, 0},
    {0x84, {0x61}, 1, 0},
    {0xF2, {0x82}, 1, 0},
    {0xF0, {0x00}, 1, 0},
    {0xF0, {0x01}, 1, 0},
    {0xF1, {0x01}, 1, 0},
    {0xB0, {0x56}, 1, 0},
    {0xB1, {0x4D}, 1, 0},
    {0xB2, {0x24}, 1, 0},
    {0xB4, {0x87}, 1, 0},
    {0xB5, {0x44}, 1, 0},
    {0xB6, {0x8B}, 1, 0},
    {0xB7, {0x40}, 1, 0},
    {0xB8, {0x86}, 1, 0},
    {0xBA, {0x00}, 1, 0},
    {0xBB, {0x08}, 1, 0},
    {0xBC, {0x08}, 1, 0},
    {0xBD, {0x00}, 1, 0},
    {0xC0, {0x80}, 1, 0},
    {0xC1, {0x10}, 1, 0},
    {0xC2, {0x37}, 1, 0},
    {0xC3, {0x80}, 1, 0},
    {0xC4, {0x10}, 1, 0},
    {0xC5, {0x37}, 1, 0},
    {0xC6, {0xA9}, 1, 0},
    {0xC7, {0x41}, 1, 0},
    {0xC8, {0x01}, 1, 0},
    {0xC9, {0xA9}, 1, 0},
    {0xCA, {0x41}, 1, 0},
    {0xCB, {0x01}, 1, 0},
    {0xD0, {0x91}, 1, 0},
    {0xD1, {0x68}, 1, 0},
    {0xD2, {0x68}, 1, 0},
    {0xF5, {0x00, 0xA5}, 2, 0},
    {0xDD, {0x4F}, 1, 0},
    {0xDE, {0x4F}, 1, 0},
    {0xF1, {0x10}, 1, 0},
    {0xF0, {0x00}, 1, 0},
    {0xF0, {0x02}, 1, 0},
    {0xE0, {0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, {0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, {0x10}, 1, 0},
    {0xF3, {0x10}, 1, 0},
    {0xE0, {0x07}, 1, 0},
    {0xE1, {0x00}, 1, 0},
    {0xE2, {0x00}, 1, 0},
    {0xE3, {0x00}, 1, 0},
    {0xE4, {0xE0}, 1, 0},
    {0xE5, {0x06}, 1, 0},
    {0xE6, {0x21}, 1, 0},
    {0xE7, {0x01}, 1, 0},
    {0xE8, {0x05}, 1, 0},
    {0xE9, {0x02}, 1, 0},
    {0xEA, {0xDA}, 1, 0},
    {0xEB, {0x00}, 1, 0},
    {0xEC, {0x00}, 1, 0},
    {0xED, {0x0F}, 1, 0},
    {0xEE, {0x00}, 1, 0},
    {0xEF, {0x00}, 1, 0},
    {0xF8, {0x00}, 1, 0},
    {0xF9, {0x00}, 1, 0},
    {0xFA, {0x00}, 1, 0},
    {0xFB, {0x00}, 1, 0},
    {0xFC, {0x00}, 1, 0},
    {0xFD, {0x00}, 1, 0},
    {0xFE, {0x00}, 1, 0},
    {0xFF, {0x00}, 1, 0},
    {0x60, {0x40}, 1, 0},
    {0x61, {0x04}, 1, 0},
    {0x62, {0x00}, 1, 0},
    {0x63, {0x42}, 1, 0},
    {0x64, {0xD9}, 1, 0},
    {0x65, {0x00}, 1, 0},
    {0x66, {0x00}, 1, 0},
    {0x67, {0x00}, 1, 0},
    {0x68, {0x00}, 1, 0},
    {0x69, {0x00}, 1, 0},
    {0x6A, {0x00}, 1, 0},
    {0x6B, {0x00}, 1, 0},
    {0x70, {0x40}, 1, 0},
    {0x71, {0x03}, 1, 0},
    {0x72, {0x00}, 1, 0},
    {0x73, {0x42}, 1, 0},
    {0x74, {0xD8}, 1, 0},
    {0x75, {0x00}, 1, 0},
    {0x76, {0x00}, 1, 0},
    {0x77, {0x00}, 1, 0},
    {0x78, {0x00}, 1, 0},
    {0x79, {0x00}, 1, 0},
    {0x7A, {0x00}, 1, 0},
    {0x7B, {0x00}, 1, 0},
    {0x80, {0x48}, 1, 0},
    {0x81, {0x00}, 1, 0},
    {0x82, {0x06}, 1, 0},
    {0x83, {0x02}, 1, 0},
    {0x84, {0xD6}, 1, 0},
    {0x85, {0x04}, 1, 0},
    {0x86, {0x00}, 1, 0},
    {0x87, {0x00}, 1, 0},
    {0x88, {0x48}, 1, 0},
    {0x89, {0x00}, 1, 0},
    {0x8A, {0x08}, 1, 0},
    {0x8B, {0x02}, 1, 0},
    {0x8C, {0xD8}, 1, 0},
    {0x8D, {0x04}, 1, 0},
    {0x8E, {0x00}, 1, 0},
    {0x8F, {0x00}, 1, 0},
    {0x90, {0x48}, 1, 0},
    {0x91, {0x00}, 1, 0},
    {0x92, {0x0A}, 1, 0},
    {0x93, {0x02}, 1, 0},
    {0x94, {0xDA}, 1, 0},
    {0x95, {0x04}, 1, 0},
    {0x96, {0x00}, 1, 0},
    {0x97, {0x00}, 1, 0},
    {0x98, {0x48}, 1, 0},
    {0x99, {0x00}, 1, 0},
    {0x9A, {0x0C}, 1, 0},
    {0x9B, {0x02}, 1, 0},
    {0x9C, {0xDC}, 1, 0},
    {0x9D, {0x04}, 1, 0},
    {0x9E, {0x00}, 1, 0},
    {0x9F, {0x00}, 1, 0},
    {0xA0, {0x48}, 1, 0},
    {0xA1, {0x00}, 1, 0},
    {0xA2, {0x05}, 1, 0},
    {0xA3, {0x02}, 1, 0},
    {0xA4, {0xD5}, 1, 0},
    {0xA5, {0x04}, 1, 0},
    {0xA6, {0x00}, 1, 0},
    {0xA7, {0x00}, 1, 0},
    {0xA8, {0x48}, 1, 0},
    {0xA9, {0x00}, 1, 0},
    {0xAA, {0x07}, 1, 0},
    {0xAB, {0x02}, 1, 0},
    {0xAC, {0xD7}, 1, 0},
    {0xAD, {0x04}, 1, 0},
    {0xAE, {0x00}, 1, 0},
    {0xAF, {0x00}, 1, 0},
    {0xB0, {0x48}, 1, 0},
    {0xB1, {0x00}, 1, 0},
    {0xB2, {0x09}, 1, 0},
    {0xB3, {0x02}, 1, 0},
    {0xB4, {0xD9}, 1, 0},
    {0xB5, {0x04}, 1, 0},
    {0xB6, {0x00}, 1, 0},
    {0xB7, {0x00}, 1, 0},
    {0xB8, {0x48}, 1, 0},
    {0xB9, {0x00}, 1, 0},
    {0xBA, {0x0B}, 1, 0},
    {0xBB, {0x02}, 1, 0},
    {0xBC, {0xDB}, 1, 0},
    {0xBD, {0x04}, 1, 0},
    {0xBE, {0x00}, 1, 0},
    {0xBF, {0x00}, 1, 0},
    {0xC0, {0x10}, 1, 0},
    {0xC1, {0x47}, 1, 0},
    {0xC2, {0x56}, 1, 0},
    {0xC3, {0x65}, 1, 0},
    {0xC4, {0x74}, 1, 0},
    {0xC5, {0x88}, 1, 0},
    {0xC6, {0x99}, 1, 0},
    {0xC7, {0x01}, 1, 0},
    {0xC8, {0xBB}, 1, 0},
    {0xC9, {0xAA}, 1, 0},
    {0xD0, {0x10}, 1, 0},
    {0xD1, {0x47}, 1, 0},
    {0xD2, {0x56}, 1, 0},
    {0xD3, {0x65}, 1, 0},
    {0xD4, {0x74}, 1, 0},
    {0xD5, {0x88}, 1, 0},
    {0xD6, {0x99}, 1, 0},
    {0xD7, {0x01}, 1, 0},
    {0xD8, {0xBB}, 1, 0},
    {0xD9, {0xAA}, 1, 0},
    {0xF3, {0x01}, 1, 0},
    {0xF0, {0x00}, 1, 0},
    {0x21, {0x00}, 1, 0},
    {0x11, {0x00}, 1, 120},
    {0x29, {0x00}, 1, 0},
    {0x36, {0x00}, 1, 0},
};

static void lcd_cmd(uint8_t reg, const uint8_t *data, size_t len) {
    spi_transaction_ext_t t = {};
    t.base.flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
    t.base.cmd = 0x02;
    t.base.addr = ((uint32_t)reg) << 8;
    t.command_bits = 8;
    t.address_bits = 24;
    t.dummy_bits = 0;
    if (data && len > 0) {
        t.base.tx_buffer = data;
        t.base.length = len * 8;
    }
    CS_LOW();
    spi_device_polling_transmit(spi_dev, (spi_transaction_t *)&t);
    CS_HIGH();
}

static void lcd_color(const uint8_t *data, size_t len) {
    spi_transaction_ext_t t = {};
    t.base.flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY | SPI_TRANS_MODE_QIO;
    t.base.cmd = 0x32;
    t.base.addr = ((uint32_t)0x2C) << 8;
    t.command_bits = 8;
    t.address_bits = 24;
    t.dummy_bits = 0;
    t.base.tx_buffer = data;
    t.base.length = len * 8;
    CS_LOW();
    spi_device_polling_transmit(spi_dev, (spi_transaction_t *)&t);
    CS_HIGH();
}

static void lcd_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
    uint8_t caset[] = {(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2};
    uint8_t raset[] = {(uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2};
    lcd_cmd(0x2A, caset, 4);
    lcd_cmd(0x2B, raset, 4);
}

static void lcd_hw_init() {
    // CS pin — manual GPIO
    gpio_config_t cs_cfg = {
        .pin_bit_mask = (1ULL << PIN_LCD_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs_cfg);
    CS_HIGH();

    // Hardware reset
    pinMode(PIN_LCD_RST, OUTPUT);
    digitalWrite(PIN_LCD_RST, LOW);
    delay(10);
    digitalWrite(PIN_LCD_RST, HIGH);
    delay(150);

    // SPI bus
    spi_bus_config_t bus_cfg = {
        .data0_io_num = PIN_LCD_D0,
        .data1_io_num = PIN_LCD_D1,
        .sclk_io_num = PIN_LCD_CLK,
        .data2_io_num = PIN_LCD_D2,
        .data3_io_num = PIN_LCD_D3,
        .max_transfer_sz = LCD_H_RES * 36 * 2,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD,
    };
    spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);

    // SPI device
    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.command_bits = 0;
    dev_cfg.address_bits = 0;
    dev_cfg.dummy_bits = 0;
    dev_cfg.mode = 0;
    dev_cfg.clock_speed_hz = LCD_FREQ_HZ;
    dev_cfg.spics_io_num = -1;  // Manual CS
    dev_cfg.queue_size = 10;
    dev_cfg.flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY;
    spi_bus_add_device(LCD_SPI_HOST, &dev_cfg, &spi_dev);

    // Send init sequence
    for (size_t i = 0; i < sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]); i++) {
        lcd_cmd(lcd_init_cmds[i].reg, lcd_init_cmds[i].data, lcd_init_cmds[i].len);
        if (lcd_init_cmds[i].delay_ms > 0) {
            delay(lcd_init_cmds[i].delay_ms);
        }
    }

    // Backlight on
    ledcAttach(PIN_LCD_BL, 5000, 8);
    ledcWrite(PIN_LCD_BL, 200);  // ~78% brightness
}

// ─── LVGL 9.2 Setup ─────────────────────────────────────────────────

static lv_display_t *lvgl_disp = NULL;
static uint8_t *draw_buf = NULL;

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint16_t x1 = area->x1, y1 = area->y1;
    uint16_t x2 = area->x2, y2 = area->y2;

    lcd_set_window(x1, y1, x2, y2);

    size_t len = (x2 - x1 + 1) * (y2 - y1 + 1) * 2;
    lcd_color(px_map, len);

    lv_display_flush_ready(disp);
}

static void lvgl_init_display() {
    lv_init();
    lv_tick_set_cb(millis);

    lvgl_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(lvgl_disp, lvgl_flush_cb);

    size_t buf_size = LCD_H_RES * 36 * 2;  // 1/10th screen, ~25KB
    draw_buf = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    lv_display_set_buffers(lvgl_disp, draw_buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
}

// ─── Screen Rendering ────────────────────────────────────────────────

static bool dirty = false;

// Static LVGL object pointers — ALL nulled in display_clear()
static lv_obj_t *scr = NULL;
static lv_obj_t *lbl_splash = NULL;
static lv_obj_t *lbl_channel = NULL;
static lv_obj_t *lbl_freq = NULL;
static lv_obj_t *lbl_ap_count = NULL;
static lv_obj_t *arc_activity = NULL;
static lv_obj_t *lbl_scanning = NULL;
static lv_obj_t *lbl_ap_lines[7] = {};
static lv_obj_t *lbl_rssi_lines[7] = {};
static lv_obj_t *lbl_detail_ssid = NULL;
static lv_obj_t *lbl_detail_mac = NULL;
static lv_obj_t *lbl_detail_rssi = NULL;
static lv_obj_t *lbl_detail_ch = NULL;
static lv_obj_t *lbl_detail_enc = NULL;
static lv_obj_t *lbl_detail_vendor = NULL;
static lv_obj_t *lbl_no_aps = NULL;

void display_init() {
    lcd_hw_init();
    lvgl_init_display();
    Serial.println("[display] init complete");
}

void display_clear() {
    if (scr) {
        lv_obj_clean(scr);
    }
    lbl_splash = NULL;
    lbl_channel = NULL;
    lbl_freq = NULL;
    lbl_ap_count = NULL;
    arc_activity = NULL;
    lbl_scanning = NULL;
    for (int i = 0; i < 7; i++) {
        lbl_ap_lines[i] = NULL;
        lbl_rssi_lines[i] = NULL;
    }
    lbl_detail_ssid = NULL;
    lbl_detail_mac = NULL;
    lbl_detail_rssi = NULL;
    lbl_detail_ch = NULL;
    lbl_detail_enc = NULL;
    lbl_detail_vendor = NULL;
    lbl_no_aps = NULL;
    dirty = true;
}

void display_mark_dirty() {
    dirty = true;
}

void display_flush() {
    if (!dirty) return;
    lv_refr_now(lvgl_disp);
    dirty = false;
}

void display_splash() {
    scr = lv_display_get_screen_active(lvgl_disp);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lbl_splash = lv_label_create(scr);
    lv_label_set_text(lbl_splash, "NetKnob");
    lv_obj_set_style_text_font(lbl_splash, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_splash, lv_color_white(), 0);
    lv_obj_center(lbl_splash);

    dirty = true;
    display_flush();
    Serial.println("[display] splash shown");
}

// Stub implementations — filled in Task 7 and Task 8
void display_wifi_scan(WifiScannerState *state) {
    // Implemented in Task 7
}

void display_wifi_detail(AccessPoint *ap) {
    // Implemented in Task 8
}

void display_scanning(uint8_t channel) {
    // Implemented in Task 7
}
```

- [ ] **Step 4: Update `src/main.cpp` to use display**

```cpp
#include <Arduino.h>
#include "display.h"

#define SPLASH_DURATION_MS 1500

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("NetKnob Phase 1 — booting...");

    display_init();
    display_splash();

    delay(SPLASH_DURATION_MS);
    display_clear();
    display_flush();

    Serial.println("[main] setup complete");
}

void loop() {
    lv_timer_handler();
}
```

- [ ] **Step 5: Build and verify**

Run: `pio run -e knob`
Expected: Compiles successfully. If LVGL 9 API differences cause errors, adjust the API calls (check `lv_display_create` signature, `lv_display_set_buffers` params).

- [ ] **Step 6: Flash and verify**

Run: `pio run -e knob --target upload`
Expected: Display shows "NetKnob" centered on black background for 1.5 seconds, then clears to black. Serial shows init messages.

**Troubleshooting if display is garbled:** The init register table or QSPI framing may need adjustment. Check: (1) CS toggling works, (2) SPI clock is not too fast (try 20MHz), (3) init sequence matches reference exactly.

- [ ] **Step 7: Commit**

```
feat: display QSPI driver, LVGL 9.2 init, splash screen
```

---

## Task 3: Encoder Driver

**Files:**
- Create: `src/encoder.h`
- Create: `src/encoder.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: Create `src/encoder.h`**

```cpp
#pragma once

#include <stdint.h>

void encoder_init(uint8_t pin_a, uint8_t pin_b);
int8_t encoder_get_delta();
```

- [ ] **Step 2: Create `src/encoder.cpp`**

```cpp
#include "encoder.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define ENCODER_TICK_US 3000  // 3ms polling interval
#define ENCODER_DEBOUNCE 2    // 2 ticks = 6ms effective debounce

static uint8_t pin_a, pin_b;
static uint8_t level_a, level_b;
static uint8_t debounce_a, debounce_b;
static volatile int8_t delta;
static esp_timer_handle_t timer_handle;

static void process_channel(uint8_t current, uint8_t *prev, uint8_t *cnt, int8_t step) {
    if (current == 0) {
        if (current != *prev)
            *cnt = 0;
        else
            (*cnt)++;
    } else {
        if (current != *prev && ++(*cnt) >= ENCODER_DEBOUNCE) {
            *cnt = 0;
            delta += step;
        } else if (current == *prev) {
            *cnt = 0;
        }
    }
    *prev = current;
}

static void encoder_timer_cb(void *arg) {
    uint8_t a = gpio_get_level((gpio_num_t)pin_a);
    uint8_t b = gpio_get_level((gpio_num_t)pin_b);
    process_channel(a, &level_a, &debounce_a, +1);
    process_channel(b, &level_b, &debounce_b, -1);
}

void encoder_init(uint8_t a, uint8_t b) {
    pin_a = a;
    pin_b = b;
    delta = 0;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << a) | (1ULL << b),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    level_a = gpio_get_level((gpio_num_t)a);
    level_b = gpio_get_level((gpio_num_t)b);
    debounce_a = 0;
    debounce_b = 0;

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = encoder_timer_cb;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "encoder";
    esp_timer_create(&timer_args, &timer_handle);
    esp_timer_start_periodic(timer_handle, ENCODER_TICK_US);

    Serial.printf("[encoder] init on GPIO %d/%d, timer %dus\n", a, b, ENCODER_TICK_US);
}

int8_t encoder_get_delta() {
    int8_t d = delta;
    delta = 0;
    return d;
}
```

- [ ] **Step 3: Update `src/main.cpp` to test encoder**

Add encoder init and delta readout to verify on device:

```cpp
#include <Arduino.h>
#include "display.h"
#include "encoder.h"
#include "pins.h"

#define SPLASH_DURATION_MS 1500

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("NetKnob Phase 1 — booting...");

    display_init();
    display_splash();
    encoder_init(PIN_ENC_A, PIN_ENC_B);

    delay(SPLASH_DURATION_MS);
    display_clear();
    display_flush();

    Serial.println("[main] setup complete");
}

void loop() {
    int8_t d = encoder_get_delta();
    if (d != 0) {
        Serial.printf("[encoder] delta=%d\n", d);
    }

    lv_timer_handler();
}
```

- [ ] **Step 4: Build and flash**

Run: `pio run -e knob --target upload && pio device monitor -p COM9 -b 115200`
Expected: Turn the dial CW → serial shows `[encoder] delta=1`. Turn CCW → shows `[encoder] delta=-1`. Each detent should produce exactly one step.

- [ ] **Step 5: Commit**

```
feat: encoder driver — timer-polled bidi switch at 3ms
```

---

## Task 4: Touch Driver

**Files:**
- Create: `src/touch.h`
- Create: `src/touch.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: Create `src/touch.h`**

```cpp
#pragma once

#include <stdint.h>

void touch_init();
bool touch_read();
void touch_update();
bool touch_tapped();
bool touch_held();

extern bool touching;
extern uint16_t touch_x, touch_y;
```

- [ ] **Step 2: Create `src/touch.cpp`**

```cpp
#include "touch.h"
#include "pins.h"
#include <Arduino.h>
#include <Wire.h>

#define TOUCH_LATCH_MS   150
#define TOUCH_TAP_MIN_MS 30
#define TOUCH_TAP_MAX_MS 1000
#define TOUCH_HOLD_MS    1000

// Raw layer
bool touching = false;
uint16_t touch_x = 0, touch_y = 0;

// Latch layer
static bool latch = false;
static uint32_t last_touch_ms = 0;
static uint32_t contact_start_ms = 0;
static bool was_latched = false;
static bool hold_fired = false;
static bool tap_event = false;
static bool hold_event = false;

void touch_init() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);

    // Hardware reset
    pinMode(PIN_TOUCH_RST, OUTPUT);
    digitalWrite(PIN_TOUCH_RST, LOW);
    delay(10);
    digitalWrite(PIN_TOUCH_RST, HIGH);
    delay(50);

    // Set normal mode
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(0x00);
    Wire.write(0x00);
    Wire.endTransmission();

    pinMode(PIN_TOUCH_INT, INPUT);

    Serial.println("[touch] init complete");
}

bool touch_read() {
    touching = false;

    if (digitalRead(PIN_TOUCH_INT) != LOW) return false;

    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(0x01);
    Wire.endTransmission();
    Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, (uint8_t)6);

    if (Wire.available() < 6) return false;

    uint8_t data[6];
    for (int i = 0; i < 6; i++) data[i] = Wire.read();

    uint8_t fingers = data[0];
    if (fingers == 0) return false;

    touch_x = ((data[1] & 0x0F) << 8) | data[2];
    touch_y = ((data[3] & 0x0F) << 8) | data[4];
    touching = true;
    return true;
}

void touch_update() {
    uint32_t now = millis();

    // Update latch
    if (touching) {
        if (!latch) {
            contact_start_ms = now;
            hold_fired = false;
        }
        latch = true;
        last_touch_ms = now;
    }

    was_latched = latch;

    // Latch expires after gap
    if (latch && !touching && (now - last_touch_ms > TOUCH_LATCH_MS)) {
        latch = false;

        // Tap detection: was touching, now released, duration in range
        uint32_t duration = last_touch_ms - contact_start_ms;
        if (duration >= TOUCH_TAP_MIN_MS && duration <= TOUCH_TAP_MAX_MS) {
            tap_event = true;
        }
    }

    // Hold detection: still latched and duration exceeded threshold
    if (latch && !hold_fired && (now - contact_start_ms > TOUCH_HOLD_MS)) {
        hold_event = true;
        hold_fired = true;
    }
}

bool touch_tapped() {
    if (tap_event) {
        tap_event = false;
        return true;
    }
    return false;
}

bool touch_held() {
    if (hold_event) {
        hold_event = false;
        return true;
    }
    return false;
}
```

- [ ] **Step 3: Update `src/main.cpp` to test touch**

```cpp
#include <Arduino.h>
#include "display.h"
#include "encoder.h"
#include "touch.h"
#include "pins.h"

#define SPLASH_DURATION_MS 1500

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("NetKnob Phase 1 — booting...");

    display_init();
    display_splash();
    encoder_init(PIN_ENC_A, PIN_ENC_B);
    touch_init();

    delay(SPLASH_DURATION_MS);
    display_clear();
    display_flush();

    Serial.println("[main] setup complete");
}

void loop() {
    touch_read();
    touch_update();

    int8_t d = encoder_get_delta();
    if (d != 0) {
        Serial.printf("[encoder] delta=%d\n", d);
    }

    if (touch_tapped()) {
        Serial.printf("[touch] TAP at (%d, %d)\n", touch_x, touch_y);
    }
    if (touch_held()) {
        Serial.printf("[touch] HOLD at (%d, %d)\n", touch_x, touch_y);
    }

    lv_timer_handler();
}
```

- [ ] **Step 4: Build and flash**

Run: `pio run -e knob --target upload && pio device monitor -p COM9 -b 115200`
Expected: Quick tap on screen → `[touch] TAP at (x, y)`. Hold finger >1s → `[touch] HOLD at (x, y)`. No false triggers from INT pulsing.

- [ ] **Step 5: Commit**

```
feat: touch driver — CST816T with latch debounce, tap/hold detection
```

---

## Task 5: Haptic Driver

**Files:**
- Create: `src/haptic.h`
- Create: `src/haptic.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: Create `src/haptic.h`**

```cpp
#pragma once

#include <stdint.h>

void haptic_init();
void haptic_play(uint8_t effect);
void haptic_click();
void haptic_double_click();
```

- [ ] **Step 2: Create `src/haptic.cpp`**

```cpp
#include "haptic.h"
#include "pins.h"
#include <Arduino.h>
#include <Wire.h>

#define DRV_REG_STATUS    0x00
#define DRV_REG_MODE      0x01
#define DRV_REG_LIBRARY   0x03
#define DRV_REG_WAVESEQ1  0x04
#define DRV_REG_GO        0x0C
#define DRV_REG_FEEDBACK  0x1A

static void drv_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(HAPTIC_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static uint8_t drv_read(uint8_t reg) {
    Wire.beginTransmission(HAPTIC_I2C_ADDR);
    Wire.write(reg);
    Wire.endTransmission();
    Wire.requestFrom((uint8_t)HAPTIC_I2C_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
}

void haptic_init() {
    // Probe device
    uint8_t status = drv_read(DRV_REG_STATUS);
    Serial.printf("[haptic] DRV2605 status: 0x%02X\n", status);

    // Mode: internal trigger
    drv_write(DRV_REG_MODE, 0x00);

    // Library: 6 (LRA-optimized)
    drv_write(DRV_REG_LIBRARY, 0x06);

    // Feedback control: set bit 7 for LRA mode
    uint8_t fb = drv_read(DRV_REG_FEEDBACK);
    drv_write(DRV_REG_FEEDBACK, fb | 0x80);

    Serial.println("[haptic] init complete — LRA mode, library 6");
}

void haptic_play(uint8_t effect) {
    drv_write(DRV_REG_GO, 0x00);        // Stop current
    drv_write(DRV_REG_WAVESEQ1, effect); // Effect slot 1
    drv_write(DRV_REG_WAVESEQ1 + 1, 0); // End marker slot 2
    drv_write(DRV_REG_GO, 0x01);        // Fire
}

void haptic_click() {
    haptic_play(1);  // Strong Click 100%
}

void haptic_double_click() {
    haptic_play(10); // Double Click 100%
}
```

- [ ] **Step 3: Update `src/main.cpp` — haptic on encoder turn**

```cpp
#include <Arduino.h>
#include "display.h"
#include "encoder.h"
#include "touch.h"
#include "haptic.h"
#include "pins.h"

#define SPLASH_DURATION_MS 1500

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("NetKnob Phase 1 — booting...");

    display_init();
    display_splash();
    encoder_init(PIN_ENC_A, PIN_ENC_B);
    touch_init();
    haptic_init();

    delay(SPLASH_DURATION_MS);
    display_clear();
    display_flush();

    Serial.println("[main] setup complete");
}

void loop() {
    touch_read();
    touch_update();

    int8_t d = encoder_get_delta();
    if (d != 0) {
        Serial.printf("[encoder] delta=%d\n", d);
        haptic_click();
    }

    if (touch_tapped()) {
        Serial.printf("[touch] TAP at (%d, %d)\n", touch_x, touch_y);
    }
    if (touch_held()) {
        Serial.printf("[touch] HOLD at (%d, %d)\n", touch_x, touch_y);
        haptic_double_click();
    }

    lv_timer_handler();
}
```

- [ ] **Step 4: Build and flash**

Run: `pio run -e knob --target upload`
Expected: Turn the dial → feel a click per detent. Hold finger on screen >1s → feel double click. Serial shows `[haptic] init complete — LRA mode, library 6`.

- [ ] **Step 5: Commit**

```
feat: haptic driver — DRV2605L LRA, click and double-click effects
```

---

## Task 6: WiFi Scanner

**Files:**
- Modify: `src/wifi_scanner.h` (replace stub with full header)
- Create: `src/wifi_scanner.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: Replace `src/wifi_scanner.h` with full header**

```cpp
#pragma once

#include <stdint.h>

#define MAX_APS_PER_CHANNEL 32
#define CHANNEL_MIN 1
#define CHANNEL_MAX 13
#define DWELL_TIME_MS 350

struct AccessPoint {
    char     ssid[33];
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  encryption;  // 0=OPEN, 1=WEP, 2=WPA, 3=WPA2, 4=WPA3
    bool     hidden;
};

struct WifiScannerState {
    uint8_t       current_channel;
    AccessPoint   ap_list[MAX_APS_PER_CHANNEL];
    uint8_t       ap_count;
    uint8_t       selected_index;
    bool          scanning;
    bool          detail_view;
    uint32_t      scan_start_ms;
};

void scanner_init();
void scanner_set_channel(uint8_t ch);
void scanner_update();
WifiScannerState* scanner_get_state();
const char* oui_lookup(const uint8_t bssid[6]);
const char* encryption_str(uint8_t enc);
uint16_t channel_to_freq(uint8_t ch);
```

- [ ] **Step 2: Create `src/wifi_scanner.cpp`**

```cpp
#include "wifi_scanner.h"
#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include <string.h>

// ─── Ring Buffer (ISR-safe) ──────────────────────────────────────────

#define RING_BUFFER_SLOTS 8
#define MAX_FRAME_LEN 256

struct RawFrame {
    uint8_t data[MAX_FRAME_LEN];
    uint16_t len;
    int8_t rssi;
    bool valid;
};

static RawFrame ring_buf[RING_BUFFER_SLOTS];
static volatile uint8_t ring_write = 0;
static volatile uint8_t ring_read = 0;
static portMUX_TYPE ring_mux = portMUX_INITIALIZER_UNLOCKED;

// ─── Scanner State ───────────────────────────────────────────────────

static WifiScannerState state;
static uint8_t prev_ap_count = 0;

// ─── OUI Table ───────────────────────────────────────────────────────

struct OuiEntry {
    uint8_t oui[3];
    const char *vendor;
};

static const OuiEntry oui_table[] = {
    {{0x00, 0x1A, 0x2B}, "Apple"},
    {{0xC8, 0x2A, 0x14}, "Samsung"},
    {{0x8C, 0xAA, 0xB5}, "Samsung"},
    {{0xB0, 0xBE, 0x76}, "TP-Link"},
    {{0xF8, 0x1A, 0x67}, "TP-Link"},
    {{0xAC, 0x84, 0xC6}, "TP-Link"},
    {{0x20, 0xA6, 0xCD}, "Netgear"},
    {{0xB4, 0xFB, 0xE4}, "Ubiquiti"},
    {{0xFC, 0xEC, 0xDA}, "Ubiquiti"},
    {{0xDC, 0x9F, 0xDB}, "Ubiquiti"},
    {{0x78, 0x8A, 0x20}, "Ubiquiti"},
    {{0x00, 0x24, 0xD7}, "Intel"},
    {{0x34, 0x02, 0x86}, "Intel"},
    {{0xE8, 0x6F, 0x38}, "Cisco/Meraki"},
    {{0x00, 0x18, 0x74}, "Cisco"},
    {{0xD8, 0xB3, 0x70}, "Asus"},
    {{0xAC, 0x9E, 0x17}, "Asus"},
    {{0xDC, 0xA6, 0x32}, "Raspberry Pi"},
    {{0x28, 0x6C, 0x07}, "Xiaomi"},
    {{0x64, 0xCE, 0x73}, "Xiaomi"},
};

const char* oui_lookup(const uint8_t bssid[6]) {
    for (size_t i = 0; i < sizeof(oui_table) / sizeof(oui_table[0]); i++) {
        if (memcmp(bssid, oui_table[i].oui, 3) == 0) {
            return oui_table[i].vendor;
        }
    }
    return "Unknown";
}

const char* encryption_str(uint8_t enc) {
    switch (enc) {
        case 0: return "OPEN";
        case 1: return "WEP";
        case 2: return "WPA";
        case 3: return "WPA2";
        case 4: return "WPA3";
        default: return "?";
    }
}

uint16_t channel_to_freq(uint8_t ch) {
    if (ch >= 1 && ch <= 13) return 2407 + ch * 5;
    if (ch == 14) return 2484;
    return 0;
}

// ─── Beacon Parsing ──────────────────────────────────────────────────

static void parse_beacon(const uint8_t *frame, uint16_t len, int8_t rssi) {
    // Minimum beacon frame: 24 (header) + 12 (fixed fields) = 36 bytes
    if (len < 36) return;

    // BSSID is at bytes 16-21 of the 802.11 header
    const uint8_t *bssid = &frame[16];

    // Capability info at bytes 34-35 (after 8-byte timestamp + 2-byte interval)
    uint16_t capability = frame[34] | (frame[35] << 8);
    bool privacy = (capability >> 4) & 1;

    // Walk tagged IEs starting at byte 36
    char ssid[33] = {};
    bool hidden = true;
    uint8_t channel = state.current_channel;  // fallback
    uint8_t encryption = privacy ? 1 : 0;     // default: WEP if privacy, else OPEN
    bool has_rsn = false;
    bool has_wpa = false;

    size_t pos = 36;
    while (pos + 2 <= len) {
        uint8_t tag = frame[pos];
        uint8_t tag_len = frame[pos + 1];
        if (pos + 2 + tag_len > len) break;

        const uint8_t *tag_data = &frame[pos + 2];

        switch (tag) {
            case 0:  // SSID
                if (tag_len > 0 && tag_len <= 32) {
                    memcpy(ssid, tag_data, tag_len);
                    ssid[tag_len] = '\0';
                    hidden = false;
                }
                break;

            case 3:  // DS Parameter Set (channel)
                if (tag_len >= 1) {
                    channel = tag_data[0];
                }
                break;

            case 48:  // RSN IE → WPA2 (or WPA3)
                has_rsn = true;
                encryption = 3;  // WPA2

                // Check for WPA3 SAE: AKM suite type 8
                // RSN IE: version(2) + group cipher(4) + pairwise count(2) + pairwise suites
                // + AKM count(2) + AKM suites
                if (tag_len >= 8) {
                    uint16_t pw_count = tag_data[4] | (tag_data[5] << 8);
                    size_t akm_offset = 6 + pw_count * 4;
                    if (akm_offset + 2 <= tag_len) {
                        uint16_t akm_count = tag_data[akm_offset] | (tag_data[akm_offset + 1] << 8);
                        for (uint16_t i = 0; i < akm_count && akm_offset + 2 + (i + 1) * 4 <= tag_len; i++) {
                            uint8_t akm_type = tag_data[akm_offset + 2 + i * 4 + 3];
                            if (akm_type == 8) {
                                encryption = 4;  // WPA3-SAE
                                break;
                            }
                        }
                    }
                }
                break;

            case 221:  // Vendor-specific
                // Check for WPA IE: OUI 00:50:F2, type 1
                if (tag_len >= 4 && tag_data[0] == 0x00 && tag_data[1] == 0x50 &&
                    tag_data[2] == 0xF2 && tag_data[3] == 0x01) {
                    has_wpa = true;
                    if (!has_rsn) encryption = 2;  // WPA only if no RSN
                }
                break;
        }

        pos += 2 + tag_len;
    }

    // Deduplication: check if BSSID already in list
    for (uint8_t i = 0; i < state.ap_count; i++) {
        if (memcmp(state.ap_list[i].bssid, bssid, 6) == 0) {
            // Keep strongest RSSI
            if (rssi > state.ap_list[i].rssi) {
                state.ap_list[i].rssi = rssi;
            }
            return;
        }
    }

    // Add new AP
    if (state.ap_count >= MAX_APS_PER_CHANNEL) {
        // Find weakest, replace if new AP is stronger
        int8_t weakest_rssi = state.ap_list[0].rssi;
        uint8_t weakest_idx = 0;
        for (uint8_t i = 1; i < state.ap_count; i++) {
            if (state.ap_list[i].rssi < weakest_rssi) {
                weakest_rssi = state.ap_list[i].rssi;
                weakest_idx = i;
            }
        }
        if (rssi <= weakest_rssi) return;  // Too weak, discard
        // Replace weakest
        AccessPoint *ap = &state.ap_list[weakest_idx];
        memcpy(ap->ssid, ssid, 33);
        memcpy(ap->bssid, bssid, 6);
        ap->rssi = rssi;
        ap->channel = channel;
        ap->encryption = encryption;
        ap->hidden = hidden;
        return;
    }

    AccessPoint *ap = &state.ap_list[state.ap_count];
    memcpy(ap->ssid, ssid, 33);
    memcpy(ap->bssid, bssid, 6);
    ap->rssi = rssi;
    ap->channel = channel;
    ap->encryption = encryption;
    ap->hidden = hidden;
    state.ap_count++;
}

// ─── Sort (insertion sort by RSSI, descending) ───────────────────────

static void sort_ap_list() {
    for (uint8_t i = 1; i < state.ap_count; i++) {
        AccessPoint temp = state.ap_list[i];
        int8_t j = i - 1;
        while (j >= 0 && state.ap_list[j].rssi < temp.rssi) {
            state.ap_list[j + 1] = state.ap_list[j];
            j--;
        }
        state.ap_list[j + 1] = temp;
    }
}

// ─── Promiscuous Callback ────────────────────────────────────────────

static void IRAM_ATTR promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;

    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *frame = pkt->payload;
    uint16_t frame_len = pkt->rx_ctrl.sig_len;

    // Check subtype: beacon (0x80) or probe response (0x50)
    if (frame[0] != 0x80 && frame[0] != 0x50) return;

    uint16_t copy_len = frame_len;
    if (copy_len > MAX_FRAME_LEN) copy_len = MAX_FRAME_LEN;

    portENTER_CRITICAL(&ring_mux);
    uint8_t next = (ring_write + 1) % RING_BUFFER_SLOTS;
    if (next != ring_read) {  // Not full
        memcpy((void *)ring_buf[ring_write].data, frame, copy_len);
        ring_buf[ring_write].len = copy_len;
        ring_buf[ring_write].rssi = pkt->rx_ctrl.rssi;
        ring_buf[ring_write].valid = true;
        ring_write = next;
    }
    portEXIT_CRITICAL(&ring_mux);
}

// ─── Public Interface ────────────────────────────────────────────────

void scanner_init() {
    memset(&state, 0, sizeof(state));
    state.current_channel = 1;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);

    // Set initial channel
    esp_wifi_set_channel(state.current_channel, WIFI_SECOND_CHAN_NONE);
    state.scanning = true;
    state.scan_start_ms = millis();

    Serial.println("[scanner] init — promiscuous mode on channel 1");
}

void scanner_set_channel(uint8_t ch) {
    if (ch < CHANNEL_MIN) ch = CHANNEL_MIN;
    if (ch > CHANNEL_MAX) ch = CHANNEL_MAX;

    state.current_channel = ch;
    state.ap_count = 0;
    state.selected_index = 0;
    state.scanning = true;
    state.scan_start_ms = millis();
    prev_ap_count = 0;

    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

void scanner_update() {
    // Drain ring buffer
    while (ring_read != ring_write) {
        portENTER_CRITICAL(&ring_mux);
        RawFrame frame = ring_buf[ring_read];
        ring_read = (ring_read + 1) % RING_BUFFER_SLOTS;
        portEXIT_CRITICAL(&ring_mux);

        if (frame.valid) {
            parse_beacon(frame.data, frame.len, frame.rssi);
        }
    }

    // Check dwell time
    if (state.scanning && (millis() - state.scan_start_ms >= DWELL_TIME_MS)) {
        state.scanning = false;
        sort_ap_list();
        Serial.printf("[scanner] ch%d scan done — %d APs\n", state.current_channel, state.ap_count);
    }
}

WifiScannerState* scanner_get_state() {
    return &state;
}
```

- [ ] **Step 3: Update `src/main.cpp` to test scanner**

```cpp
#include <Arduino.h>
#include "display.h"
#include "encoder.h"
#include "touch.h"
#include "haptic.h"
#include "wifi_scanner.h"
#include "pins.h"

#define SPLASH_DURATION_MS 1500

static uint8_t current_channel = 1;

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("NetKnob Phase 1 — booting...");

    display_init();
    display_splash();
    encoder_init(PIN_ENC_A, PIN_ENC_B);
    touch_init();
    haptic_init();
    scanner_init();

    delay(SPLASH_DURATION_MS);
    display_clear();
    display_flush();

    Serial.println("[main] setup complete");
}

void loop() {
    touch_read();
    touch_update();

    // Encoder → channel hop
    int8_t d = encoder_get_delta();
    if (d != 0) {
        current_channel = ((current_channel - 1 + d + CHANNEL_MAX) % CHANNEL_MAX) + 1;
        scanner_set_channel(current_channel);
        haptic_click();
        Serial.printf("[main] channel -> %d (%d MHz)\n", current_channel, channel_to_freq(current_channel));
    }

    // Touch test
    if (touch_tapped()) {
        Serial.printf("[touch] TAP\n");
    }
    if (touch_held()) {
        Serial.printf("[touch] HOLD\n");
        haptic_double_click();
    }

    // Scanner update
    scanner_update();

    // Print APs once after scan completes
    WifiScannerState *s = scanner_get_state();
    static bool printed = false;
    if (!s->scanning && !printed && s->ap_count > 0) {
        for (uint8_t i = 0; i < s->ap_count; i++) {
            AccessPoint *ap = &s->ap_list[i];
            Serial.printf("  [%d] %-20s %4d dBm  %s  %s\n",
                i, ap->hidden ? "[Hidden]" : ap->ssid,
                ap->rssi, encryption_str(ap->encryption),
                oui_lookup(ap->bssid));
        }
        printed = true;
    }
    if (s->scanning) printed = false;

    lv_timer_handler();
}
```

- [ ] **Step 4: Build and flash**

Run: `pio run -e knob --target upload && pio device monitor -p COM9 -b 115200`
Expected: After boot, serial shows `[scanner] ch1 scan done — N APs` followed by a list of discovered APs with SSID, RSSI, encryption, and vendor. Turn dial → channel changes, new scan starts. APs should appear within ~500ms of channel change.

- [ ] **Step 5: Commit**

```
feat: WiFi scanner — promiscuous mode, beacon parser, OUI lookup
```

---

## Task 7: Scan Screen Rendering

**Files:**
- Modify: `src/display.cpp` — implement `display_wifi_scan()` and `display_scanning()`
- Modify: `src/main.cpp` — wire up rendering

- [ ] **Step 1: Add RSSI color helper and screen builders to `src/display.cpp`**

Replace the stub implementations of `display_wifi_scan()` and `display_scanning()` with the real code. Add these functions before the stubs in `display.cpp`:

```cpp
// ─── Helpers ─────────────────────────────────────────────────────────

static lv_color_t rssi_color(int8_t rssi) {
    if (rssi > -50) return lv_color_make(0x00, 0xE0, 0x00);  // Green
    if (rssi > -70) return lv_color_make(0xE0, 0xA0, 0x00);  // Orange
    return lv_color_make(0xE0, 0x20, 0x20);                   // Red
}

static uint16_t clamp_arc(uint8_t ap_count) {
    // 0 APs = 2%, 20+ APs = 98%, linear in between
    uint16_t val = 2 + (uint16_t)ap_count * 96 / 20;
    if (val < 2) val = 2;
    if (val > 98) val = 98;
    return val;
}

// ─── Scan Screen ─────────────────────────────────────────────────────

static bool scan_screen_built = false;

static void build_scan_screen() {
    scr = lv_display_get_screen_active(lvgl_disp);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // Channel label — large, centered top
    lbl_channel = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_channel, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_channel, lv_color_white(), 0);
    lv_obj_align(lbl_channel, LV_ALIGN_TOP_MID, 0, 20);

    // Frequency label
    lbl_freq = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_freq, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_align(lbl_freq, LV_ALIGN_TOP_MID, 0, 48);

    // AP count label
    lbl_ap_count = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_ap_count, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_align(lbl_ap_count, LV_ALIGN_TOP_MID, 0, 66);

    // Activity arc
    arc_activity = lv_arc_create(scr);
    lv_obj_set_size(arc_activity, 340, 340);
    lv_obj_center(arc_activity);
    lv_arc_set_rotation(arc_activity, 270);
    lv_arc_set_range(arc_activity, 0, 100);
    lv_arc_set_value(arc_activity, 2);
    lv_arc_set_bg_angles(arc_activity, 0, 360);
    lv_obj_remove_style(arc_activity, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc_activity, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(arc_activity, lv_color_make(0x20, 0x20, 0x20), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_activity, lv_color_make(0x00, 0x80, 0xE0), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_activity, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_activity, 4, LV_PART_INDICATOR);

    // Scanning label (shown/hidden as needed)
    lbl_scanning = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_scanning, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_align(lbl_scanning, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_add_flag(lbl_scanning, LV_OBJ_FLAG_HIDDEN);

    // No APs label
    lbl_no_aps = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_no_aps, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_align(lbl_no_aps, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_flag(lbl_no_aps, LV_OBJ_FLAG_HIDDEN);

    // AP list lines — 7 rows
    int16_t y_start = 95;
    int16_t row_h = 32;
    for (int i = 0; i < 7; i++) {
        lbl_ap_lines[i] = lv_label_create(scr);
        lv_obj_set_style_text_color(lbl_ap_lines[i], lv_color_white(), 0);
        lv_obj_set_width(lbl_ap_lines[i], 220);
        lv_label_set_long_mode(lbl_ap_lines[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_pos(lbl_ap_lines[i], 40, y_start + i * row_h);
        lv_obj_add_flag(lbl_ap_lines[i], LV_OBJ_FLAG_HIDDEN);

        lbl_rssi_lines[i] = lv_label_create(scr);
        lv_obj_set_style_text_color(lbl_rssi_lines[i], lv_color_white(), 0);
        lv_obj_set_pos(lbl_rssi_lines[i], 270, y_start + i * row_h);
        lv_obj_add_flag(lbl_rssi_lines[i], LV_OBJ_FLAG_HIDDEN);
    }

    scan_screen_built = true;
}
```

Now replace the `display_wifi_scan()` and `display_scanning()` stubs:

```cpp
void display_scanning(uint8_t channel) {
    if (!scan_screen_built) build_scan_screen();

    lv_label_set_text_fmt(lbl_channel, "Channel %d", channel);
    lv_label_set_text_fmt(lbl_freq, "%d MHz", channel_to_freq(channel));
    lv_label_set_text(lbl_ap_count, "");
    lv_arc_set_value(arc_activity, 2);

    // Show scanning label
    static uint8_t dot_count = 0;
    dot_count = (dot_count + 1) % 4;
    const char *dots[] = {"Scanning", "Scanning.", "Scanning..", "Scanning..."};
    lv_label_set_text(lbl_scanning, dots[dot_count]);
    lv_obj_clear_flag(lbl_scanning, LV_OBJ_FLAG_HIDDEN);

    // Hide AP list and no-aps label
    lv_obj_add_flag(lbl_no_aps, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 7; i++) {
        lv_obj_add_flag(lbl_ap_lines[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_rssi_lines[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void display_wifi_scan(WifiScannerState *state) {
    if (!scan_screen_built) build_scan_screen();

    // Header
    lv_label_set_text_fmt(lbl_channel, "Channel %d", state->current_channel);
    lv_label_set_text_fmt(lbl_freq, "%d MHz", channel_to_freq(state->current_channel));
    lv_label_set_text_fmt(lbl_ap_count, "%d APs", state->ap_count);

    // Activity arc
    lv_arc_set_value(arc_activity, clamp_arc(state->ap_count));

    // Scanning indicator
    if (state->scanning) {
        lv_obj_clear_flag(lbl_scanning, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_scanning, "Scanning...");
    } else {
        lv_obj_add_flag(lbl_scanning, LV_OBJ_FLAG_HIDDEN);
    }

    // AP list
    if (state->ap_count == 0) {
        lv_label_set_text_fmt(lbl_no_aps, "No APs found on channel %d", state->current_channel);
        lv_obj_clear_flag(lbl_no_aps, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 7; i++) {
            lv_obj_add_flag(lbl_ap_lines[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_rssi_lines[i], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    lv_obj_add_flag(lbl_no_aps, LV_OBJ_FLAG_HIDDEN);

    uint8_t visible = state->ap_count < 7 ? state->ap_count : 7;
    for (int i = 0; i < 7; i++) {
        if (i < visible) {
            AccessPoint *ap = &state->ap_list[i];
            const char *name = ap->hidden ? "[Hidden]" : ap->ssid;

            // Prefix selected AP with indicator
            if (i == state->selected_index) {
                lv_label_set_text_fmt(lbl_ap_lines[i], LV_SYMBOL_RIGHT " %s", name);
                lv_obj_set_style_text_color(lbl_ap_lines[i], lv_color_white(), 0);
            } else {
                lv_label_set_text(lbl_ap_lines[i], name);
                lv_obj_set_style_text_color(lbl_ap_lines[i], lv_color_make(0xC0, 0xC0, 0xC0), 0);
            }

            // RSSI value with color
            lv_label_set_text_fmt(lbl_rssi_lines[i], "%d", ap->rssi);
            lv_obj_set_style_text_color(lbl_rssi_lines[i], rssi_color(ap->rssi), 0);

            lv_obj_clear_flag(lbl_ap_lines[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_rssi_lines[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_ap_lines[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_rssi_lines[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}
```

- [ ] **Step 2: Wire rendering in `src/main.cpp`**

Replace the loop body with rendering logic:

```cpp
#include <Arduino.h>
#include "display.h"
#include "encoder.h"
#include "touch.h"
#include "haptic.h"
#include "wifi_scanner.h"
#include "pins.h"

#define SPLASH_DURATION_MS 1500

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("NetKnob Phase 1 — booting...");

    display_init();
    display_splash();
    encoder_init(PIN_ENC_A, PIN_ENC_B);
    touch_init();
    haptic_init();
    scanner_init();

    delay(SPLASH_DURATION_MS);
    display_clear();
    display_mark_dirty();

    Serial.println("[main] setup complete");
}

static uint8_t prev_ap_count = 0xFF;
static bool prev_scanning = false;

void loop() {
    // 1. Touch
    touch_read();
    touch_update();

    WifiScannerState *s = scanner_get_state();

    // 2. Encoder → channel hop
    int8_t d = encoder_get_delta();
    if (!s->detail_view && d != 0) {
        uint8_t ch = s->current_channel;
        ch = ((ch - 1 + d + CHANNEL_MAX) % CHANNEL_MAX) + 1;
        scanner_set_channel(ch);
        haptic_click();
        display_mark_dirty();
    }

    // 3. Touch interaction
    if (!s->detail_view) {
        if (touch_tapped() && s->ap_count > 0) {
            s->selected_index = (s->selected_index + 1) % s->ap_count;
            display_mark_dirty();
        }
        if (touch_held() && s->ap_count > 0) {
            s->detail_view = true;
            haptic_double_click();
            display_clear();
            display_mark_dirty();
        }
    } else {
        if (touch_tapped()) {
            s->detail_view = false;
            display_clear();
            display_mark_dirty();
        }
    }

    // 4. Scanner update
    scanner_update();

    // 5. Dirty check on scanner data changes
    if (s->ap_count != prev_ap_count || s->scanning != prev_scanning) {
        prev_ap_count = s->ap_count;
        prev_scanning = s->scanning;
        display_mark_dirty();
    }

    // 6. Render
    if (!s->detail_view) {
        if (s->scanning && s->ap_count == 0) {
            display_scanning(s->current_channel);
        } else {
            display_wifi_scan(s);
        }
    }
    display_flush();

    // 7. LVGL tick
    lv_timer_handler();
}
```

- [ ] **Step 3: Build and flash**

Run: `pio run -e knob --target upload`
Expected: After splash, the scan screen appears showing channel number, frequency, and discovered APs with RSSI values color-coded (green/orange/red). Turn dial → channel changes, new scan. "Scanning..." indicator shows during dwell. Selected AP indicator (arrow) on first AP.

- [ ] **Step 4: Commit**

```
feat: scan screen rendering — AP list, channel header, activity arc
```

---

## Task 8: Detail View

**Files:**
- Modify: `src/display.cpp` — implement `display_wifi_detail()`

- [ ] **Step 1: Implement `display_wifi_detail()` in `src/display.cpp`**

Replace the stub:

```cpp
void display_wifi_detail(AccessPoint *ap) {
    scr = lv_display_get_screen_active(lvgl_disp);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // SSID — large centered
    lbl_detail_ssid = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_detail_ssid, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_detail_ssid, lv_color_white(), 0);
    lv_obj_set_width(lbl_detail_ssid, 300);
    lv_label_set_long_mode(lbl_detail_ssid, LV_LABEL_LONG_CLIP);
    lv_obj_align(lbl_detail_ssid, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_text_align(lbl_detail_ssid, LV_TEXT_ALIGN_CENTER, 0);
    if (ap->hidden) {
        lv_label_set_text_fmt(lbl_detail_ssid, "[Hidden]\n%02X:%02X:%02X:%02X:%02X:%02X",
            ap->bssid[0], ap->bssid[1], ap->bssid[2],
            ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    } else {
        lv_label_set_text(lbl_detail_ssid, ap->ssid);
    }

    // MAC
    lbl_detail_mac = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_detail_mac, lv_color_make(0xA0, 0xA0, 0xA0), 0);
    lv_obj_align(lbl_detail_mac, LV_ALIGN_TOP_MID, 0, 120);
    lv_label_set_text_fmt(lbl_detail_mac, "MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        ap->bssid[0], ap->bssid[1], ap->bssid[2],
        ap->bssid[3], ap->bssid[4], ap->bssid[5]);

    // RSSI
    lbl_detail_rssi = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_detail_rssi, rssi_color(ap->rssi), 0);
    lv_obj_align(lbl_detail_rssi, LV_ALIGN_TOP_MID, 0, 150);
    lv_label_set_text_fmt(lbl_detail_rssi, "RSSI  %d dBm", ap->rssi);

    // Channel
    lbl_detail_ch = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_detail_ch, lv_color_make(0xA0, 0xA0, 0xA0), 0);
    lv_obj_align(lbl_detail_ch, LV_ALIGN_TOP_MID, 0, 180);
    lv_label_set_text_fmt(lbl_detail_ch, "CH    %d", ap->channel);

    // Encryption
    lbl_detail_enc = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_detail_enc, lv_color_make(0xA0, 0xA0, 0xA0), 0);
    lv_obj_align(lbl_detail_enc, LV_ALIGN_TOP_MID, 0, 210);
    lv_label_set_text_fmt(lbl_detail_enc, "Enc   %s", encryption_str(ap->encryption));

    // Vendor
    lbl_detail_vendor = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_detail_vendor, lv_color_make(0xA0, 0xA0, 0xA0), 0);
    lv_obj_align(lbl_detail_vendor, LV_ALIGN_TOP_MID, 0, 240);
    lv_label_set_text_fmt(lbl_detail_vendor, "Vendor  %s", oui_lookup(ap->bssid));

    // "tap = back" hint
    lv_obj_t *lbl_hint = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_hint, lv_color_make(0x60, 0x60, 0x60), 0);
    lv_obj_align(lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_label_set_text(lbl_hint, "[ tap = back ]");

    scan_screen_built = false;  // Force rebuild of scan screen on return
}
```

- [ ] **Step 2: Update detail view rendering in `src/main.cpp` loop**

Add the detail view render call inside the render block. Change the render section (step 6 in loop) to:

```cpp
    // 6. Render
    if (s->detail_view) {
        display_wifi_detail(&s->ap_list[s->selected_index]);
    } else {
        if (s->scanning && s->ap_count == 0) {
            display_scanning(s->current_channel);
        } else {
            display_wifi_scan(s);
        }
    }
    display_flush();
```

- [ ] **Step 3: Build and flash**

Run: `pio run -e knob --target upload`
Expected: Tap screen → selected AP cycles through list (arrow indicator moves). Hold >1s → detail view shows SSID, MAC, RSSI (colored), channel, encryption, vendor. Tap in detail → returns to AP list. Double-click haptic on entering detail view.

- [ ] **Step 4: Commit**

```
feat: detail view — full AP info with tap-to-return
```

---

## Task 9: Final Integration — Stubs, Enums, Serial Debug

**Files:**
- Create: `src/interchip.h`
- Modify: `src/main.cpp` — add enums, stubs, serial heartbeat

- [ ] **Step 1: Create `src/interchip.h`**

```cpp
#pragma once

#include <stdint.h>

// Reserved for Phase 4: ESP-NOW communication between ESP32-S3 and ESP32
struct EspNowMessage {
    uint8_t type;
    uint8_t data[32];
};
```

- [ ] **Step 2: Final `src/main.cpp`**

Complete main.cpp with all enums, stubs, and serial debug:

```cpp
#include <Arduino.h>
#include "display.h"
#include "encoder.h"
#include "touch.h"
#include "haptic.h"
#include "wifi_scanner.h"
#include "interchip.h"
#include "pins.h"

// ─── Enums (reserved slots for future phases) ───────────────────────

enum EncoderMode {
    ENC_CHANNEL_HOP,      // Phase 1: encoder controls channel selection
    ENC_TARGET_SELECT,    // Phase 2+: encoder selects target
    ENC_SCREEN_SWITCH,    // Phase 2+: encoder switches screen
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

// ─── State ───────────────────────────────────────────────────────────

static Screen currentScreen = SCREEN_WIFI_SCAN;
static EncoderMode encoderMode = ENC_CHANNEL_HOP;

#define SPLASH_DURATION_MS  1500
#define SERIAL_HEARTBEAT_MS 5000

// ─── Reserved stubs ──────────────────────────────────────────────────

void on_secondary_esp_message(const EspNowMessage *msg) {
    // Phase 4: handle messages from secondary ESP32
}

// ─── Setup ───────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("NetKnob Phase 1 — booting...");

    display_init();
    display_splash();
    encoder_init(PIN_ENC_A, PIN_ENC_B);
    touch_init();
    haptic_init();
    scanner_init();

    delay(SPLASH_DURATION_MS);
    display_clear();
    display_mark_dirty();

    Serial.println("[main] setup complete — entering scan mode");
}

// ─── Loop ────────────────────────────────────────────────────────────

static uint8_t prev_ap_count = 0xFF;
static bool prev_scanning = false;
static uint32_t last_heartbeat = 0;

void loop() {
    // 1. Touch — always first for INT pulse responsiveness
    touch_read();
    touch_update();

    WifiScannerState *s = scanner_get_state();

    // 2. Encoder → channel hop (only in scanner mode, not detail view)
    int8_t d = encoder_get_delta();
    if (!s->detail_view && d != 0) {
        uint8_t ch = s->current_channel;
        ch = ((ch - 1 + d + CHANNEL_MAX) % CHANNEL_MAX) + 1;
        scanner_set_channel(ch);
        haptic_click();
        display_mark_dirty();
    }

    // 3. Touch interaction
    if (!s->detail_view) {
        if (touch_tapped() && s->ap_count > 0) {
            s->selected_index = (s->selected_index + 1) % s->ap_count;
            display_mark_dirty();
        }
        if (touch_held() && s->ap_count > 0) {
            s->detail_view = true;
            haptic_double_click();
            display_clear();
            display_mark_dirty();
        }
    } else {
        if (touch_tapped()) {
            s->detail_view = false;
            display_clear();
            display_mark_dirty();
        }
    }

    // 4. Scanner update — drain ring buffer, parse beacons
    scanner_update();

    // 5. Dirty check on scanner state changes
    if (s->ap_count != prev_ap_count || s->scanning != prev_scanning) {
        prev_ap_count = s->ap_count;
        prev_scanning = s->scanning;
        display_mark_dirty();
    }

    // 6. Render
    if (s->detail_view) {
        display_wifi_detail(&s->ap_list[s->selected_index]);
    } else {
        if (s->scanning && s->ap_count == 0) {
            display_scanning(s->current_channel);
        } else {
            display_wifi_scan(s);
        }
    }
    display_flush();

    // 7. LVGL tick
    lv_timer_handler();

    // 8. Serial debug heartbeat
    uint32_t now = millis();
    if (now - last_heartbeat >= SERIAL_HEARTBEAT_MS) {
        last_heartbeat = now;
        Serial.printf("[heartbeat] ch=%d aps=%d scanning=%d detail=%d heap=%d\n",
            s->current_channel, s->ap_count, s->scanning, s->detail_view,
            ESP.getFreeHeap());
    }
}
```

- [ ] **Step 3: Build and flash**

Run: `pio run -e knob --target upload && pio device monitor -p COM9 -b 115200`
Expected: Full application running. Verify all acceptance criteria:
- AC-01: Scanner screen shown at startup (after splash)
- AC-02/03: Encoder CW/CCW changes channel with wrapping
- AC-04: Each channel change triggers 350ms dwell
- AC-05: APs appear within 500ms
- AC-06: RSSI color coding correct
- AC-07: AP list sorted by RSSI
- AC-08: Touch tap cycles selected AP
- AC-09: Touch hold opens detail view
- AC-10: Tap in detail returns to list
- AC-11: Rapid channel hopping (1→13→1 in <5s) — no crash
- AC-12: Open/close detail >20 times — no crash
- AC-13: Arc values clamped (check serial for no LVGL errors)
- AC-14: Serial heartbeat shows correct info every 5s
- AC-15: `interchip.h` exists with `EspNowMessage`
- AC-16: Screen enum has Phase 2-7 slots
- AC-17: `on_secondary_esp_message()` stub exists

- [ ] **Step 4: Commit**

```
feat: final integration — enums, stubs, serial heartbeat, acceptance complete
```

---

## Post-Implementation Notes

**If display is blank or garbled after Task 2:**
1. Check USB-C orientation (ESP32-S3 = VID 303A on COM9)
2. Try reducing SPI clock to `20 * 1000 * 1000` in `lcd_hw_init()`
3. Verify CS pin toggles (add serial prints around CS_LOW/CS_HIGH)
4. Compare init register sequence against `temp_volosr/KnobRGBControl/lcd_bsp.c` byte-for-byte

**If LVGL 9.2 API errors during build:**
- `lv_display_create` / `lv_display_set_flush_cb` / `lv_display_set_buffers` — these are LVGL 9 APIs. If errors, check LVGL version in `lib_deps` resolves to 9.2.x.
- If `LV_COLOR_16_SWAP` is not recognized, byte-swap manually in `lvgl_flush_cb`: `for each pixel: px = (px >> 8) | (px << 8)`
- If `LV_SYMBOL_RIGHT` doesn't exist, replace with `">"` string literal.

**If touch is unresponsive:**
- Verify `lv_refr_now()` is only called when dirty (not every loop)
- Check that `touch_read()` is called before any rendering in the loop
- Add `Serial.println()` inside `touch_read()` to confirm INT pulses are being caught

**If encoder misses steps:**
- Verify GPIO 8 and 7 are not swapped (CW = pin A = GPIO 8)
- Check timer is running: `Serial.println()` inside timer callback (remove after verification)
