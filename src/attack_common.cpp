#include "attack_common.h"
#include <Arduino.h>
#include <string.h>

static AttackState state;

void attack_init() {
    memset(&state, 0, sizeof(state));
    state.type = ATTACK_NONE;
    state.phase = ATTACK_IDLE;
    state.ssid_count = 20;
    state.ssid_source = 0;
    state.tx_rate = 10;
    state.duration_sec = 30;
}

void attack_update() {
    if (state.phase == ATTACK_ARMED) {
        if (millis() - state.armed_start_ms >= ATTACK_COUNTDOWN_MS) {
            state.phase = ATTACK_RUNNING;
            state.running_start_ms = millis();
            state.stats.start_time_ms = millis();
            state.stats.packets_sent = 0;
            Serial.printf("[attack] RUNNING — type=%d\n", state.type);
        }
    }

    if (state.phase == ATTACK_RUNNING && state.duration_sec > 0) {
        uint32_t elapsed = millis() - state.running_start_ms;
        if (elapsed >= (uint32_t)state.duration_sec * 1000) {
            attack_stop();
            Serial.println("[attack] auto-timeout");
        }
    }

    if (state.phase == ATTACK_RUNNING && state.stats.packets_sent > 0) {
        uint32_t elapsed = millis() - state.stats.start_time_ms;
        if (elapsed > 0) {
            state.stats.avg_tx_rate = (float)state.stats.packets_sent / (elapsed / 1000.0f);
        }
    }
}

void attack_start(AttackType type) {
    state.type = type;
    state.phase = ATTACK_CONFIG;
    memset(&state.stats, 0, sizeof(state.stats));
    Serial.printf("[attack] CONFIG — type=%d\n", type);
}

void attack_confirm() {
    if (state.phase != ATTACK_CONFIG) return;
    state.phase = ATTACK_ARMED;
    state.armed_start_ms = millis();
    Serial.println("[attack] ARMED — countdown started");
}

void attack_stop() {
    if (state.phase == ATTACK_IDLE) return;
    state.stats.end_time_ms = millis();
    state.phase = ATTACK_COMPLETE;
    Serial.printf("[attack] COMPLETE — sent %u packets\n", state.stats.packets_sent);
}

void attack_emergency_stop() {
    state.phase = ATTACK_IDLE;
    state.type = ATTACK_NONE;
    Serial.println("[attack] EMERGENCY STOP");
}

AttackState* attack_get_state() {
    return &state;
}

bool attack_is_running() {
    return state.phase == ATTACK_ARMED || state.phase == ATTACK_RUNNING;
}
