#pragma once

#include <stdint.h>

#define BEACON_MAX_SSIDS        50
#define BEACON_DEFAULT_COUNT    20
#define BEACON_DEFAULT_RATE     10
#define BEACON_MAX_RATE         500
#define ATTACK_DEFAULT_DURATION 30
#define BEACON_WORDLIST_COUNT   20

struct BeaconTemplate {
    uint8_t  frame[128];
    uint8_t  frame_len;
    uint16_t seq_number;
};

void wifi_attack_init();
void wifi_attack_start_beacon_flood();
void wifi_attack_stop();
void wifi_attack_update();
