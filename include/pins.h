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
