#pragma once

#include <stdint.h>

#define PROBE_BUFFER_SIZE        100
#define PROBE_MAC_RANDOMIZED_BIT 0x02

struct ProbeRequest {
    uint8_t  src_mac[6];
    char     ssid_probed[33];
    int8_t   rssi;
    uint32_t timestamp_ms;
    uint8_t  channel;
    bool     mac_randomized;
};

struct ProbeSnifferState {
    ProbeRequest probes[PROBE_BUFFER_SIZE];
    uint16_t     write_index;
    uint16_t     total_count;
    uint8_t      unique_macs;
    uint8_t      unique_ssids;
    bool         running;
    bool         channel_hop;
};

void probe_sniffer_init();
void probe_sniffer_start();
void probe_sniffer_stop();
void probe_sniffer_update();
ProbeSnifferState* probe_sniffer_get_state();
void probe_sniffer_on_frame(const uint8_t* frame, uint16_t len, int8_t rssi, uint8_t channel);
