#pragma once

#include "navigation.h"

extern const ScreenDef scr_beacon_flood_def;

void scr_beacon_flood_on_encoder(int8_t delta);
void scr_beacon_flood_on_tap();
void scr_beacon_flood_on_hold();
