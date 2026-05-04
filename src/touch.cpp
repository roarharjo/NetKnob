#include "touch.h"
#include "pins.h"
#include <Arduino.h>
#include <Wire.h>

#define TOUCH_LATCH_MS   300
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
    Wire.write(0x00);
    Wire.endTransmission();
    Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, (uint8_t)7);

    if (Wire.available() < 7) return false;

    uint8_t data[7];
    for (int i = 0; i < 7; i++) data[i] = Wire.read();

    uint8_t fingers = data[2];
    if (fingers == 0) return false;

    touch_x = ((data[3] & 0x0F) << 8) | data[4];
    touch_y = ((data[5] & 0x0F) << 8) | data[6];
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
        // Suppress tap if hold already fired (finger was held, not tapped)
        uint32_t duration = last_touch_ms - contact_start_ms;
        if (!hold_fired && duration >= TOUCH_TAP_MIN_MS && duration <= TOUCH_TAP_MAX_MS) {
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
