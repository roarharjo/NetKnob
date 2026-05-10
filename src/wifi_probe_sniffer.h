#pragma once

#include <stdint.h>

// Tunable constants (FSD Appendix A)
#define MAX_DEVICES            32
#define MAX_SSIDS_PER_DEV      16
#define RAW_BUFFER_SIZE        32
#define RANDOMIZED_MAC_WINDOW   8
#define PROBE_MAC_RANDOMIZED_BIT 0x02

// Unchanged from Phase 3
struct ProbeRequest {
    uint8_t  src_mac[6];
    char     ssid_probed[33];
    int8_t   rssi;
    uint32_t timestamp_ms;
    uint8_t  channel;
    bool     mac_randomized;
};

struct DeviceSSID {
    char     ssid[33];
    uint16_t probe_count;
    uint32_t last_seen_ms;
};

struct DeviceEntry {
    uint8_t      mac[6];
    char         vendor[20];
    int8_t       rssi_last;
    int8_t       rssi_min;
    int8_t       rssi_max;
    uint16_t     probe_count;
    uint8_t      ssid_count;
    DeviceSSID   ssids[MAX_SSIDS_PER_DEV];
    uint32_t     first_seen_ms;
    uint32_t     last_seen_ms;
    uint8_t      last_channel;
};

struct RandomizedAggregate {
    uint16_t    unique_macs;
    uint16_t    total_probes;
    uint8_t     ssid_count;
    DeviceSSID  ssids[MAX_SSIDS_PER_DEV];
    int8_t      rssi_strongest;
    uint32_t    last_seen_ms;
    uint8_t     recent_macs[RANDOMIZED_MAC_WINDOW][6];
    uint8_t     recent_macs_count;
};

struct ProbeSnifferState {
    DeviceEntry          devices[MAX_DEVICES];
    uint8_t              device_count;
    RandomizedAggregate  randomized;
    ProbeRequest         raw[RAW_BUFFER_SIZE];
    uint16_t             raw_write_index;
    uint32_t             total_probes;
    uint32_t             session_start_ms;
    bool                 running;
    bool                 paused;
};

void probe_sniffer_init();
void probe_sniffer_start();
void probe_sniffer_stop();
void probe_sniffer_update();
ProbeSnifferState* probe_sniffer_get_state();
void probe_sniffer_on_frame(const uint8_t* frame, uint16_t len, int8_t rssi, uint8_t channel);
