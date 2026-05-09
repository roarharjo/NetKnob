#pragma once

#include <stdint.h>

enum AttackType {
    ATTACK_NONE,
    ATTACK_BEACON_FLOOD,
    ATTACK_PROBE_SNIFF
};

enum AttackPhase {
    ATTACK_IDLE,
    ATTACK_CONFIG,
    ATTACK_ARMED,
    ATTACK_RUNNING,
    ATTACK_COMPLETE
};

struct AttackStats {
    uint32_t packets_sent;
    uint32_t start_time_ms;
    uint32_t end_time_ms;
    float    avg_tx_rate;
};

struct AttackState {
    AttackType   type;
    AttackPhase  phase;
    AttackStats  stats;
    uint8_t      channel;
    uint16_t     duration_sec;       // 0 = infinite
    uint16_t     tx_rate;            // packets per second
    uint8_t      ssid_count;
    uint8_t      ssid_source;        // 0=random, 1=wordlist, 2=clone
    uint32_t     armed_start_ms;     // When ARMED phase began
    uint32_t     running_start_ms;   // When RUNNING phase began
};

#define ATTACK_COUNTDOWN_MS    1000
#define ATTACK_CONFIRM_HOLD_MS 1000
#define ATTACK_HAPTIC_INTERVAL 100

void attack_init();
void attack_update();
void attack_start(AttackType type);
void attack_confirm();
void attack_stop();
void attack_emergency_stop();
AttackState* attack_get_state();
bool attack_is_running();
