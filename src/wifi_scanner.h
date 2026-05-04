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
    uint32_t last_seen;   // millis() timestamp for aging
};

struct WifiScannerState {
    uint8_t       current_channel;
    AccessPoint   ap_list[MAX_APS_PER_CHANNEL];
    uint8_t       ap_count;
    uint8_t       selected_index;
    uint8_t       selected_bssid[6];  // Track selection by BSSID, not index
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
