#pragma once

#include <stdint.h>

struct Settings {
    bool     lock_enabled;
    uint8_t  lock_timeout_min;     // 0 = never
    uint8_t  wifi_region;          // 0=ETSI(1-13), 1=FCC(1-11), 2=Japan(1-14)
    uint8_t  display_brightness;   // 10-100
    bool     haptic_enabled;
    uint8_t  haptic_strength;      // 0=Weak, 1=Medium, 2=Strong
    bool     auto_scan_on_boot;
    uint8_t  splash_duration_sec;  // 0-5
};

void settings_init();              // Load from NVS or use defaults
void settings_save();              // Write all to NVS
const Settings* settings_get();
Settings* settings_get_mut();      // For editing

// Individual setters (save immediately)
void settings_set_lock_enabled(bool val);
void settings_set_lock_timeout(uint8_t min);
void settings_set_wifi_region(uint8_t region);
void settings_set_brightness(uint8_t pct);
void settings_set_haptic_enabled(bool val);
void settings_set_haptic_strength(uint8_t str);
void settings_set_auto_scan(bool val);
void settings_set_splash_duration(uint8_t sec);

// Lock code management
bool settings_verify_lock_code(uint8_t d1, uint8_t d2, uint8_t d3);
void settings_set_lock_code(uint8_t d1, uint8_t d2, uint8_t d3);
bool settings_has_lock_code();

// WiFi region helpers
uint8_t settings_max_channel();    // Returns 11, 13, or 14 based on region
