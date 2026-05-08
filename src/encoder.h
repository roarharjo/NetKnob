#pragma once

#include <stdint.h>

void encoder_init(uint8_t pin_a, uint8_t pin_b);
int8_t encoder_get_delta();

struct EncoderEvent {
    int8_t   direction;     // +1 CW, -1 CCW
    uint32_t timestamp_us;  // microseconds at detection
};

#define ENCODER_EVENT_SLOTS 16

// New: drain event buffer for gesture processing
uint8_t encoder_get_events(EncoderEvent* out, uint8_t max_count);
