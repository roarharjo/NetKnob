#pragma once

#include <stdint.h>

// Reserved for Phase 4: ESP-NOW communication between ESP32-S3 and ESP32
struct EspNowMessage {
    uint8_t type;
    uint8_t data[32];
};
