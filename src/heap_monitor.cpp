#include "heap_monitor.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

static uint32_t last_log_ms = 0;
static bool warned = false;
static bool critical = false;

void heap_monitor_init() {
    heap_monitor_log_baseline("boot");
}

void heap_monitor_update() {
    uint32_t now = millis();
    if (now - last_log_ms < HEAP_LOG_INTERVAL_MS) return;
    last_log_ms = now;

    uint32_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t min_ever = esp_get_minimum_free_heap_size();
    uint32_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    uint32_t psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    Serial.printf("[HEAP] free=%lu min=%lu largest=%lu psram=%lu\n",
                  free_int, min_ever, largest, psram);

    if (free_int < HEAP_CRITICAL_THRESHOLD && !critical) {
        Serial.printf("[HEAP CRITICAL] %lu bytes remaining\n", free_int);
        critical = true;
    } else if (free_int < HEAP_WARN_THRESHOLD && !warned) {
        Serial.printf("[HEAP WARNING] %lu bytes remaining\n", free_int);
        warned = true;
    }

    if (free_int >= HEAP_WARN_THRESHOLD) { warned = false; critical = false; }
}

void heap_monitor_log_baseline(const char* label) {
    uint32_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    Serial.printf("[HEAP BASELINE] after=%s free=%lu psram=%lu\n", label, free_int, psram);
}
