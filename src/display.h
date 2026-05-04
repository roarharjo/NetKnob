#pragma once

#include "wifi_scanner.h"

void display_init();
void display_splash();
void display_animate_splash(uint16_t duration_ms);
void display_clear();
void display_mark_dirty();
bool display_is_dirty();
void display_flush();
void display_wifi_scan(WifiScannerState *state);
void display_wifi_detail(AccessPoint *ap);
void display_scanning(uint8_t channel);
void display_update_live(WifiScannerState *state);
void display_update_arc_pulse();
