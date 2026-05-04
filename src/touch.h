#pragma once

#include <stdint.h>

void touch_init();
bool touch_read();
void touch_update();
bool touch_tapped();
bool touch_held();

extern bool touching;
extern uint16_t touch_x, touch_y;
