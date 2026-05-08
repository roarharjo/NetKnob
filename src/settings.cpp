#include "settings.h"
#include <Arduino.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <mbedtls/sha256.h>
#include <esp_mac.h>
#include <string.h>

static Settings cfg;
static nvs_handle_t nvs;
static uint8_t lock_hash[32] = {};
static bool lock_hash_valid = false;

static const Settings defaults = {
    .lock_enabled = false,
    .lock_timeout_min = 5,
    .wifi_region = 0,       // ETSI
    .display_brightness = 80,
    .haptic_enabled = true,
    .haptic_strength = 1,   // Medium
    .auto_scan_on_boot = true,
    .splash_duration_sec = 2
};

static uint8_t clamp(uint8_t val, uint8_t lo, uint8_t hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

void settings_init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_open("settings", NVS_READWRITE, &nvs);

    uint8_t u8;
    cfg = defaults;

    if (nvs_get_u8(nvs, "lock_en", &u8) == ESP_OK) cfg.lock_enabled = u8;
    if (nvs_get_u8(nvs, "lock_tout", &u8) == ESP_OK) cfg.lock_timeout_min = clamp(u8, 0, 60);
    if (nvs_get_u8(nvs, "wifi_rgn", &u8) == ESP_OK) cfg.wifi_region = clamp(u8, 0, 2);
    if (nvs_get_u8(nvs, "disp_brt", &u8) == ESP_OK) cfg.display_brightness = clamp(u8, 10, 100);
    if (nvs_get_u8(nvs, "haptic_en", &u8) == ESP_OK) cfg.haptic_enabled = u8;
    if (nvs_get_u8(nvs, "haptic_str", &u8) == ESP_OK) cfg.haptic_strength = clamp(u8, 0, 2);
    if (nvs_get_u8(nvs, "auto_scan", &u8) == ESP_OK) cfg.auto_scan_on_boot = u8;
    if (nvs_get_u8(nvs, "splash_dur", &u8) == ESP_OK) cfg.splash_duration_sec = clamp(u8, 0, 5);

    // Load lock hash
    size_t len = 32;
    if (nvs_get_blob(nvs, "lock_hash", lock_hash, &len) == ESP_OK && len == 32) {
        lock_hash_valid = true;
    }

    Serial.printf("[settings] loaded (lock=%d, region=%d, brightness=%d)\n",
                  cfg.lock_enabled, cfg.wifi_region, cfg.display_brightness);
}

void settings_save() {
    nvs_set_u8(nvs, "lock_en", cfg.lock_enabled);
    nvs_set_u8(nvs, "lock_tout", cfg.lock_timeout_min);
    nvs_set_u8(nvs, "wifi_rgn", cfg.wifi_region);
    nvs_set_u8(nvs, "disp_brt", cfg.display_brightness);
    nvs_set_u8(nvs, "haptic_en", cfg.haptic_enabled);
    nvs_set_u8(nvs, "haptic_str", cfg.haptic_strength);
    nvs_set_u8(nvs, "auto_scan", cfg.auto_scan_on_boot);
    nvs_set_u8(nvs, "splash_dur", cfg.splash_duration_sec);
    nvs_commit(nvs);
}

const Settings* settings_get() { return &cfg; }
Settings* settings_get_mut() { return &cfg; }

void settings_set_lock_enabled(bool val) {
    cfg.lock_enabled = val;
    settings_save();
}
void settings_set_lock_timeout(uint8_t min) { cfg.lock_timeout_min = clamp(min, 0, 60); settings_save(); }
void settings_set_wifi_region(uint8_t r) { cfg.wifi_region = clamp(r, 0, 2); settings_save(); }
void settings_set_brightness(uint8_t pct) { cfg.display_brightness = clamp(pct, 10, 100); settings_save(); }
void settings_set_haptic_enabled(bool val) { cfg.haptic_enabled = val; settings_save(); }
void settings_set_haptic_strength(uint8_t str) { cfg.haptic_strength = clamp(str, 0, 2); settings_save(); }
void settings_set_auto_scan(bool val) { cfg.auto_scan_on_boot = val; settings_save(); }
void settings_set_splash_duration(uint8_t sec) { cfg.splash_duration_sec = clamp(sec, 0, 5); settings_save(); }

// --- Lock code hashing ---

static void compute_hash(uint8_t d1, uint8_t d2, uint8_t d3, uint8_t out[32]) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    uint8_t input[9];
    input[0] = d1;
    input[1] = d2;
    input[2] = d3;
    memcpy(&input[3], mac, 6);

    mbedtls_sha256(input, 9, out, 0);  // 0 = SHA-256
}

void settings_set_lock_code(uint8_t d1, uint8_t d2, uint8_t d3) {
    compute_hash(d1, d2, d3, lock_hash);
    nvs_set_blob(nvs, "lock_hash", lock_hash, 32);
    nvs_commit(nvs);
    lock_hash_valid = true;
}

bool settings_verify_lock_code(uint8_t d1, uint8_t d2, uint8_t d3) {
    if (!lock_hash_valid) return true;

    uint8_t attempt[32];
    compute_hash(d1, d2, d3, attempt);

    // Constant-time comparison
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= attempt[i] ^ lock_hash[i];
    return diff == 0;
}

bool settings_has_lock_code() { return lock_hash_valid; }

uint8_t settings_max_channel() {
    switch (cfg.wifi_region) {
        case 1: return 11;  // FCC
        case 2: return 14;  // Japan
        default: return 13; // ETSI
    }
}
