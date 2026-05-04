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
