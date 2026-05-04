#pragma once

#include <stdint.h>

void encoder_init(uint8_t pin_a, uint8_t pin_b);
int8_t encoder_get_delta();
