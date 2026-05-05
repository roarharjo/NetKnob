#pragma once

#include <stdint.h>

#define HEAP_LOG_INTERVAL_MS   10000
#define HEAP_WARN_THRESHOLD    81920   // 80 KB
#define HEAP_CRITICAL_THRESHOLD 30720  // 30 KB

void heap_monitor_init();
void heap_monitor_update();  // Call from main loop
void heap_monitor_log_baseline(const char* label);
