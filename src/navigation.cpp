#include "navigation.h"
#include "display.h"
#include "haptic.h"
#include "heap_monitor.h"
#include <Arduino.h>
#include <lvgl.h>
#include <string.h>

static NavigationState nav_state;
static const ScreenDef* screen_registry[SCREEN_COUNT] = {};
static bool screen_created[SCREEN_COUNT] = {};

void navigation_init() {
    memset(&nav_state, 0, sizeof(nav_state));
    nav_state.active_screen = SCREEN_MAIN_MENU;
    nav_state.active_group = GROUP_WIFI;
    nav_state.last_activity_ms = millis();
}

void navigation_register_screen(const ScreenDef* def) {
    screen_registry[def->id] = def;
}

void navigation_goto(ScreenId id) {
    const ScreenDef* current = screen_registry[nav_state.active_screen];
    const ScreenDef* target  = screen_registry[id];
    if (!target) return;

    // Hide current
    if (current && screen_created[nav_state.active_screen] && current->hide)
        current->hide();

    // Create target if first visit
    if (!screen_created[id]) {
        if (target->create) target->create();
        screen_created[id] = true;
        heap_monitor_log_baseline(target->name);
    }

    // Show target
    if (target->show) target->show();

    nav_state.active_screen = id;
    nav_state.last_activity_ms = millis();

    Serial.printf("[nav] goto %s\n", target->name);
}

void navigation_open_menu() {
    haptic_play(14);  // Medium buzz
    navigation_goto(SCREEN_MAIN_MENU);
}

void navigation_emergency_stop() {
    haptic_play(10);  // Strong double-pulse
    navigation_goto(SCREEN_MAIN_MENU);
    Serial.println("[nav] EMERGENCY STOP");
}

void navigation_update() {
    const ScreenDef* active = screen_registry[nav_state.active_screen];
    if (active && active->update) active->update();
}

void navigation_mark_activity() {
    nav_state.last_activity_ms = millis();
}

ScreenId navigation_get_active() {
    return nav_state.active_screen;
}

EncoderMode navigation_get_encoder_mode() {
    const ScreenDef* active = screen_registry[nav_state.active_screen];
    if (active) return active->enc_mode;
    return ENC_MENU;
}

const NavigationState* navigation_get_state() {
    return &nav_state;
}

void navigation_set_group_selection(uint8_t idx) {
    nav_state.group_selection = idx;
}

void navigation_set_screen_selection(ScreenGroup group, uint8_t idx) {
    if (group < GROUP_COUNT) nav_state.screen_selection[group] = idx;
}

void navigation_set_active_group(ScreenGroup group) {
    nav_state.active_group = group;
}
