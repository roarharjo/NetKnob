#pragma once

#include <stdint.h>

enum ScreenGroup {
    GROUP_WIFI,
    GROUP_BLE,
    GROUP_SYSTEM,
    GROUP_COUNT
};

enum ScreenId {
    SCREEN_MAIN_MENU,
    SCREEN_GROUP_MENU,
    SCREEN_WIFI_SCAN,
    SCREEN_BLE_SCAN,
    SCREEN_SETTINGS,
    SCREEN_DEBUG,
    SCREEN_SAFE_LOCK,
    SCREEN_BEACON_FLOOD,
    SCREEN_PROBE_SNIFF,
    SCREEN_COUNT
};

enum EncoderMode {
    ENC_MENU,           // Menu navigation
    ENC_CHANNEL_HOP,    // WiFi scanner channel control
    ENC_BLE_LIST,       // BLE device list scroll
    ENC_SAFE_LOCK,      // Safe-lock dial
    ENC_SETTINGS,       // Settings scroll/adjust
    ENC_ATTACK_PARAM,       // Attack parameter adjustment
    ENC_LOCKED          // Encoder ignored
};

struct ScreenDef {
    const char*   name;
    ScreenGroup   group;
    ScreenId      id;
    void          (*create)();    // First-time LVGL object creation
    void          (*show)();      // Called when screen becomes active
    void          (*hide)();      // Called when screen is deactivated
    void          (*destroy)();   // Free memory (NULL = retain forever)
    void          (*update)();    // Called each loop when active (live data)
    EncoderMode   enc_mode;
};

struct NavigationState {
    ScreenId      active_screen;
    ScreenGroup   active_group;
    uint8_t       group_selection;                   // Main menu cursor
    uint8_t       screen_selection[GROUP_COUNT];     // Per-group cursor
    bool          stealth_mode;
    uint32_t      last_activity_ms;
};

void navigation_init();
void navigation_register_screen(const ScreenDef* def);
void navigation_goto(ScreenId id);
void navigation_open_menu();       // Backspin handler
void navigation_emergency_stop();  // Shake handler
void navigation_update();          // Call from main loop (runs active screen's update)
void navigation_mark_activity();   // Reset auto-lock timer

ScreenId navigation_get_active();
EncoderMode navigation_get_encoder_mode();
const NavigationState* navigation_get_state();

// For menu screens to update navigation state
void navigation_set_group_selection(uint8_t idx);
void navigation_set_screen_selection(ScreenGroup group, uint8_t idx);
void navigation_set_active_group(ScreenGroup group);
