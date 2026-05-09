#include "wifi_probe_sniffer.h"
#include <Arduino.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Ring buffer for ISR-safe frame passing
// ---------------------------------------------------------------------------
#define PROBE_RING_SLOTS    8
#define PROBE_RING_BUF_SIZE 128

struct ProbeRingSlot {
    uint8_t  data[PROBE_RING_BUF_SIZE];
    uint16_t len;
    int8_t   rssi;
    uint8_t  channel;
};

static ProbeRingSlot    probe_ring[PROBE_RING_SLOTS];
static volatile uint8_t probe_ring_head = 0;
static volatile uint8_t probe_ring_tail = 0;
static portMUX_TYPE     probe_ring_mux  = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static ProbeSnifferState state;

// Track how many probes were seen at last recount
static uint16_t last_recount_total = 0;

// ---------------------------------------------------------------------------
// recount_uniques — O(n^2) scan over the circular buffer
// ---------------------------------------------------------------------------
static void recount_uniques() {
    uint8_t  mac_count  = 0;
    uint8_t  ssid_count = 0;
    uint16_t filled     = (state.total_count < PROBE_BUFFER_SIZE)
                          ? state.total_count
                          : PROBE_BUFFER_SIZE;

    // We work backwards from write_index so the newest entries are at index 0
    // of our virtual scan. We just need distinct counts, order doesn't matter.
    for (uint16_t i = 0; i < filled; i++) {
        uint16_t idx = (state.write_index + PROBE_BUFFER_SIZE - 1 - i) % PROBE_BUFFER_SIZE;
        const ProbeRequest& pr = state.probes[idx];

        // Count distinct MACs
        bool mac_seen = false;
        for (uint16_t j = i + 1; j < filled; j++) {
            uint16_t jdx = (state.write_index + PROBE_BUFFER_SIZE - 1 - j) % PROBE_BUFFER_SIZE;
            if (memcmp(pr.src_mac, state.probes[jdx].src_mac, 6) == 0) {
                mac_seen = true;
                break;
            }
        }
        if (!mac_seen) mac_count++;

        // Count distinct non-empty SSIDs
        if (pr.ssid_probed[0] != '\0') {
            bool ssid_seen = false;
            for (uint16_t j = i + 1; j < filled; j++) {
                uint16_t jdx = (state.write_index + PROBE_BUFFER_SIZE - 1 - j) % PROBE_BUFFER_SIZE;
                if (strcmp(pr.ssid_probed, state.probes[jdx].ssid_probed) == 0) {
                    ssid_seen = true;
                    break;
                }
            }
            if (!ssid_seen) ssid_count++;
        }
    }

    state.unique_macs  = mac_count;
    state.unique_ssids = ssid_count;
}

// ---------------------------------------------------------------------------
// parse_probe_request — called from main loop (not ISR)
// ---------------------------------------------------------------------------
static void parse_probe_request(const uint8_t* frame, uint16_t len, int8_t rssi, uint8_t channel) {
    // Need at least 24 bytes for the MAC header
    if (len < 24) return;

    ProbeRequest pr;
    memset(&pr, 0, sizeof(pr));

    // Source MAC at bytes 10-15
    memcpy(pr.src_mac, &frame[10], 6);
    pr.mac_randomized = (pr.src_mac[0] & PROBE_MAC_RANDOMIZED_BIT) != 0;
    pr.rssi           = rssi;
    pr.channel        = channel;
    pr.timestamp_ms   = millis();

    // Walk tagged IEs starting at byte 24
    // Probe requests have no fixed body between MAC header and IEs
    uint16_t pos = 24;
    while (pos + 2 <= len) {
        uint8_t tag_id  = frame[pos];
        uint8_t tag_len = frame[pos + 1];

        if (pos + 2 + tag_len > len) break;

        if (tag_id == 0) {
            // SSID element
            uint8_t copy_len = (tag_len > 32) ? 32 : tag_len;
            memcpy(pr.ssid_probed, &frame[pos + 2], copy_len);
            pr.ssid_probed[copy_len] = '\0';
            break;  // SSID is always the first IE in probe requests
        }

        pos += 2 + tag_len;
    }

    // Write to circular buffer
    state.probes[state.write_index] = pr;
    state.write_index = (state.write_index + 1) % PROBE_BUFFER_SIZE;
    state.total_count++;

    // Recount uniques every 50 new probes
    if (state.total_count - last_recount_total >= 50) {
        recount_uniques();
        last_recount_total = state.total_count;
    }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void probe_sniffer_init() {
    memset(&state, 0, sizeof(state));
    state.running     = false;
    state.channel_hop = true;
    last_recount_total = 0;
}

void probe_sniffer_start() {
    state.running = true;
}

void probe_sniffer_stop() {
    state.running = false;
}

void probe_sniffer_update() {
    // Drain ring buffer
    while (true) {
        portENTER_CRITICAL(&probe_ring_mux);
        bool has_data = (probe_ring_tail != probe_ring_head);
        ProbeRingSlot slot;
        if (has_data) {
            slot = probe_ring[probe_ring_tail];
            probe_ring_tail = (probe_ring_tail + 1) % PROBE_RING_SLOTS;
        }
        portEXIT_CRITICAL(&probe_ring_mux);

        if (!has_data) break;

        parse_probe_request(slot.data, slot.len, slot.rssi, slot.channel);
    }
}

ProbeSnifferState* probe_sniffer_get_state() {
    return &state;
}

void IRAM_ATTR probe_sniffer_on_frame(const uint8_t* frame, uint16_t len, int8_t rssi, uint8_t channel) {
    if (!state.running) return;

    portENTER_CRITICAL(&probe_ring_mux);
    uint8_t next_head = (probe_ring_head + 1) % PROBE_RING_SLOTS;
    if (next_head != probe_ring_tail) {
        uint16_t copy_len = (len > PROBE_RING_BUF_SIZE) ? PROBE_RING_BUF_SIZE : len;
        memcpy(probe_ring[probe_ring_head].data, frame, copy_len);
        probe_ring[probe_ring_head].len     = copy_len;
        probe_ring[probe_ring_head].rssi    = rssi;
        probe_ring[probe_ring_head].channel = channel;
        probe_ring_head = next_head;
    }
    portEXIT_CRITICAL(&probe_ring_mux);
}
