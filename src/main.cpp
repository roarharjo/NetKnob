#include <Arduino.h>
#include <lvgl.h>
#include <esp_system.h>
#include "display.h"
#include "encoder.h"
#include "touch.h"
#include "haptic.h"
#include "wifi_scanner.h"
#include "gesture.h"
#include "navigation.h"
#include "heap_monitor.h"
#include "settings.h"
#include "ble_scanner.h"
#include "safe_lock.h"
#include "pins.h"
#include "interchip.h"

// Screen definitions
#include "screens/scr_main_menu.h"
#include "screens/scr_group_menu.h"
#include "screens/scr_wifi_scan.h"
#include "screens/scr_ble_scan.h"
#include "screens/scr_settings.h"
#include "screens/scr_debug.h"
#include "screens/scr_safe_lock.h"

#define SPLASH_DURATION_MS 1500

void on_secondary_esp_message(const EspNowMessage *msg) {
    // Phase 4: handle messages from secondary ESP32
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("NetKnob Phase 2 — booting...");

    esp_reset_reason_t reason = esp_reset_reason();
    const char* reasons[] = {"UNKNOWN","POWERON","EXT","SW","PANIC","INT_WDT","TASK_WDT","WDT","DEEPSLEEP","BROWNOUT","SDIO"};
    Serial.printf("[main] reset reason: %s (%d)\n", reason < 11 ? reasons[reason] : "?", reason);
    Serial.printf("[main] free heap: %d, PSRAM: %d\n", ESP.getFreeHeap(), ESP.getPsramSize());

    settings_init();

    Serial.println("[main] display_init...");
    display_init();
    Serial.println("[main] display_splash...");
    display_splash();

    encoder_init(PIN_ENC_A, PIN_ENC_B);
    touch_init();
    haptic_init();
    gesture_init();
    scanner_init();
    ble_scanner_init();
    safe_lock_init();

    display_animate_splash(SPLASH_DURATION_MS);

    // Register all screens
    navigation_init();
    navigation_register_screen(&scr_main_menu_def);
    navigation_register_screen(&scr_group_menu_def);
    navigation_register_screen(&scr_wifi_scan_def);
    navigation_register_screen(&scr_ble_scan_def);
    navigation_register_screen(&scr_settings_def);
    navigation_register_screen(&scr_debug_def);
    navigation_register_screen(&scr_safe_lock_def);

    // Boot to lock screen or main menu
    if (settings_get()->lock_enabled && settings_has_lock_code()) {
        navigation_goto(SCREEN_SAFE_LOCK);
    } else {
        navigation_goto(SCREEN_MAIN_MENU);
    }

    heap_monitor_init();
    Serial.println("[main] setup complete");
}

#define SERIAL_HEARTBEAT_MS 5000
static uint32_t last_heartbeat = 0;

void loop() {
    // 1. Touch — always first for INT pulse responsiveness
    touch_read();
    touch_update();

    // 2. Gesture processing (consumes encoder events)
    GestureEvent gesture = gesture_update();

    // 3. Gesture-level actions (highest priority)
    if (gesture == GESTURE_SHAKE) {
        navigation_emergency_stop();
    } else if (gesture == GESTURE_BACKSPIN) {
        navigation_open_menu();
    } else {
        // 4. Route encoder delta to active screen
        int8_t delta = gesture_get_delta();
        if (delta != 0) {
            navigation_mark_activity();
            ScreenId active = navigation_get_active();
            switch (active) {
                case SCREEN_MAIN_MENU:
                    scr_main_menu_on_encoder(delta);
                    break;
                case SCREEN_GROUP_MENU:
                    scr_group_menu_on_encoder(delta);
                    break;
                case SCREEN_WIFI_SCAN:
                    scr_wifi_scan_on_encoder(delta);
                    break;
                case SCREEN_BLE_SCAN:
                    scr_ble_scan_on_encoder(delta);
                    break;
                case SCREEN_SETTINGS:
                    scr_settings_on_encoder(delta);
                    break;
                case SCREEN_SAFE_LOCK:
                    scr_safe_lock_on_encoder(delta);
                    break;
                default: break;
            }
        }

        // 5. Route touch to active screen
        if (touch_tapped()) {
            navigation_mark_activity();
            ScreenId active = navigation_get_active();
            switch (active) {
                case SCREEN_MAIN_MENU:
                    scr_main_menu_on_tap();
                    break;
                case SCREEN_GROUP_MENU:
                    scr_group_menu_on_tap();
                    break;
                case SCREEN_WIFI_SCAN:
                    scr_wifi_scan_on_tap();
                    break;
                case SCREEN_BLE_SCAN:
                    scr_ble_scan_on_tap();
                    break;
                case SCREEN_SETTINGS:
                    scr_settings_on_tap();
                    break;
                case SCREEN_SAFE_LOCK:
                    scr_safe_lock_on_tap();
                    break;
                default: break;
            }
        }
        if (touch_held()) {
            navigation_mark_activity();
            ScreenId active = navigation_get_active();
            switch (active) {
                case SCREEN_WIFI_SCAN:
                    scr_wifi_scan_on_hold();
                    break;
                case SCREEN_BLE_SCAN:
                    scr_ble_scan_on_hold();
                    break;
                default: break;
            }
        }
    }

    // 6. Active screen update (live data, rendering)
    navigation_update();

    // 7. LVGL tick
    lv_timer_handler();

    // 8. Heap monitoring
    heap_monitor_update();

    // 9. Auto-lock check
    uint32_t now = millis();
    const Settings* cfg = settings_get();
    if (cfg->lock_enabled && cfg->lock_timeout_min > 0 && settings_has_lock_code()) {
        const NavigationState* nav = navigation_get_state();
        if (nav->active_screen != SCREEN_SAFE_LOCK) {
            uint32_t timeout_ms = (uint32_t)cfg->lock_timeout_min * 60000;
            if (now - nav->last_activity_ms >= timeout_ms) {
                navigation_goto(SCREEN_SAFE_LOCK);
            }
        }
    }

    // 10. Serial debug heartbeat
    if (now - last_heartbeat >= SERIAL_HEARTBEAT_MS) {
        last_heartbeat = now;
        WifiScannerState *ws = scanner_get_state();
        BleScannerState *bs = ble_scanner_get_state();
        Serial.printf("[heartbeat] screen=%d ch=%d aps=%d ble=%d heap=%d\n",
            navigation_get_active(), ws->current_channel, ws->ap_count,
            bs->device_count, ESP.getFreeHeap());
    }
}
