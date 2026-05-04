#include <Arduino.h>
#include <lvgl.h>
#include <string.h>
#include "display.h"
#include "encoder.h"
#include "touch.h"
#include "haptic.h"
#include "wifi_scanner.h"
#include "pins.h"
#include "interchip.h"

#define SPLASH_DURATION_MS 1500

enum EncoderMode {
    ENC_CHANNEL_HOP,      // Phase 1: encoder controls channel selection
    ENC_TARGET_SELECT,    // Phase 2+: encoder selects target
    ENC_SCREEN_SWITCH,    // Phase 2+: encoder switches screen
    ENC_LOCKED            // During active attack
};

enum Screen {
    SCREEN_WIFI_SCAN,
    // SCREEN_DEAUTH,        // Phase 2
    // SCREEN_BEACON_FLOOD,  // Phase 2
    // SCREEN_BLE_SCAN,      // Phase 3
    // SCREEN_BT_SCAN,       // Phase 4
    // SCREEN_AUDIO_MON,     // Phase 5
    // SCREEN_DUAL_STATUS,   // Phase 6
    // SCREEN_BOOT_MENU,     // Phase 7
    SCREEN_COUNT
};

static Screen currentScreen = SCREEN_WIFI_SCAN;
static EncoderMode encoderMode = ENC_CHANNEL_HOP;

void on_secondary_esp_message(const EspNowMessage *msg) {
    // Phase 4: handle messages from secondary ESP32
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("NetKnob Phase 1 — booting...");
    Serial.printf("[main] free heap: %d, PSRAM: %d\n", ESP.getFreeHeap(), ESP.getPsramSize());

    Serial.println("[main] display_init...");
    display_init();
    Serial.println("[main] display_splash...");
    display_splash();

    encoder_init(PIN_ENC_A, PIN_ENC_B);
    touch_init();
    haptic_init();
    scanner_init();

    display_animate_splash(SPLASH_DURATION_MS);
    display_clear();
    display_mark_dirty();

    Serial.println("[main] setup complete");
}

static uint8_t prev_ap_count = 0xFF;
static bool prev_scanning = false;

#define SERIAL_HEARTBEAT_MS 5000
static uint32_t last_heartbeat = 0;

void loop() {
    // 1. Touch — always first for INT pulse responsiveness
    touch_read();
    touch_update();

    WifiScannerState *s = scanner_get_state();

    // 2. Encoder → channel hop (only when not in detail view)
    int8_t d = encoder_get_delta();
    if (!s->detail_view && d != 0) {
        uint8_t ch = s->current_channel;
        ch = ((ch - 1 + d + CHANNEL_MAX) % CHANNEL_MAX) + 1;
        scanner_set_channel(ch);
        haptic_click();
        display_mark_dirty();
    }

    // 3. Touch interaction
    if (!s->detail_view) {
        if (touch_tapped() && s->ap_count > 0) {
            s->selected_index = (s->selected_index + 1) % s->ap_count;
            memcpy(s->selected_bssid, s->ap_list[s->selected_index].bssid, 6);
            display_mark_dirty();
        }
        if (touch_held() && s->ap_count > 0) {
            s->detail_view = true;
            haptic_double_click();
            display_clear();
            display_mark_dirty();
        }
    } else {
        if (touch_tapped()) {
            s->detail_view = false;
            display_clear();
            display_mark_dirty();
        }
    }

    // 4. Scanner update
    scanner_update();

    // 5. Dirty check — only on AP count change (new AP found/lost)
    // Don't trigger on scanning state toggle — that causes flashing
    if (s->ap_count != prev_ap_count) {
        prev_ap_count = s->ap_count;
        display_mark_dirty();
    }

    // Exit detail view if all APs aged out
    if (s->detail_view && s->ap_count == 0) {
        s->detail_view = false;
        display_clear();
        display_mark_dirty();
    }

    // 6. Render only when dirty (channel change, touch, new AP count)
    if (display_is_dirty()) {
        if (s->detail_view) {
            display_wifi_detail(&s->ap_list[s->selected_index]);
        } else if (s->ap_count == 0 && s->scanning) {
            display_scanning(s->current_channel);
        } else {
            display_wifi_scan(s);
        }
        display_flush();
    }

    // 7. Live data updates (arc + RSSI numbers only, throttled to 2Hz)
    if (!s->detail_view && s->ap_count > 0) {
        display_update_live(s);
        display_update_arc_pulse();
    }

    // 8. LVGL tick
    lv_timer_handler();

    // 9. Serial debug heartbeat
    uint32_t now = millis();
    if (now - last_heartbeat >= SERIAL_HEARTBEAT_MS) {
        last_heartbeat = now;
        Serial.printf("[heartbeat] ch=%d aps=%d scanning=%d detail=%d heap=%d\n",
            s->current_channel, s->ap_count, s->scanning, s->detail_view,
            ESP.getFreeHeap());
    }
}
