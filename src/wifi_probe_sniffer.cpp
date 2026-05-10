#include "wifi_probe_sniffer.h"
#include "wifi_scanner.h"       // oui_lookup()
#include <Arduino.h>
#include <string.h>

// ---------------------------------------------------------------------------
// ISR ring buffer — unchanged from Phase 3
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

// ---------------------------------------------------------------------------
// ssid_list_update — add/increment SSID in a DeviceSSID array
// Shared by device_update and randomized_aggregate_update (DRY)
// ---------------------------------------------------------------------------
static void ssid_list_update(DeviceSSID ssids[], uint8_t* count, const char* raw_ssid, uint32_t now) {
    const char* ssid = (raw_ssid[0] == '\0') ? "(broadcast)" : raw_ssid;

    // Find existing
    for (uint8_t i = 0; i < *count; i++) {
        if (strcmp(ssids[i].ssid, ssid) == 0) {
            ssids[i].probe_count++;
            ssids[i].last_seen_ms = now;
            return;
        }
    }

    // Add new slot
    DeviceSSID* slot;
    if (*count < MAX_SSIDS_PER_DEV) {
        slot = &ssids[(*count)++];
    } else {
        // Evict oldest SSID
        uint8_t oldest = 0;
        for (uint8_t i = 1; i < *count; i++) {
            if (ssids[i].last_seen_ms < ssids[oldest].last_seen_ms)
                oldest = i;
        }
        slot = &ssids[oldest];
    }

    strncpy(slot->ssid, ssid, 32);
    slot->ssid[32] = '\0';
    slot->probe_count = 1;
    slot->last_seen_ms = now;
}

// ---------------------------------------------------------------------------
// find_or_create_device — lookup by MAC, create or LRU-evict if needed
// ---------------------------------------------------------------------------
static DeviceEntry* find_or_create_device(const uint8_t mac[6]) {
    // Search existing
    for (uint8_t i = 0; i < state.device_count; i++) {
        if (memcmp(state.devices[i].mac, mac, 6) == 0)
            return &state.devices[i];
    }

    // Pick a slot: append or evict
    DeviceEntry* dev;
    if (state.device_count < MAX_DEVICES) {
        dev = &state.devices[state.device_count++];
    } else {
        // Evict device with oldest last_seen_ms
        uint8_t oldest = 0;
        for (uint8_t i = 1; i < state.device_count; i++) {
            if (state.devices[i].last_seen_ms < state.devices[oldest].last_seen_ms)
                oldest = i;
        }
        dev = &state.devices[oldest];
    }

    memset(dev, 0, sizeof(DeviceEntry));
    memcpy(dev->mac, mac, 6);
    const char* vendor = oui_lookup(mac);
    strncpy(dev->vendor, vendor, sizeof(dev->vendor) - 1);
    dev->vendor[sizeof(dev->vendor) - 1] = '\0';
    dev->rssi_min = 0;   // sentinel: first probe will overwrite
    dev->rssi_max = -127;
    return dev;
}

// ---------------------------------------------------------------------------
// device_update — update a DeviceEntry with new probe data
// ---------------------------------------------------------------------------
static void device_update(DeviceEntry* dev, const char* ssid, int8_t rssi,
                          uint8_t channel, uint32_t now) {
    dev->probe_count++;
    dev->rssi_last = rssi;
    if (dev->rssi_min == 0 || rssi < dev->rssi_min) dev->rssi_min = rssi;
    if (rssi > dev->rssi_max) dev->rssi_max = rssi;
    if (dev->first_seen_ms == 0) dev->first_seen_ms = now;
    dev->last_seen_ms = now;
    dev->last_channel = channel;

    ssid_list_update(dev->ssids, &dev->ssid_count, ssid, now);
}

// ---------------------------------------------------------------------------
// randomized_aggregate_update — collapse randomized MACs into aggregate
// ---------------------------------------------------------------------------
static void randomized_aggregate_update(const uint8_t mac[6], const char* ssid,
                                        int8_t rssi, uint8_t channel, uint32_t now) {
    RandomizedAggregate& ra = state.randomized;
    ra.total_probes++;
    if (rssi > ra.rssi_strongest || ra.total_probes == 1) ra.rssi_strongest = rssi;
    ra.last_seen_ms = now;

    // Check sliding MAC window
    bool in_window = false;
    for (uint8_t i = 0; i < ra.recent_macs_count; i++) {
        if (memcmp(ra.recent_macs[i], mac, 6) == 0) {
            in_window = true;
            break;
        }
    }

    if (!in_window) {
        ra.unique_macs++;
        if (ra.recent_macs_count < RANDOMIZED_MAC_WINDOW) {
            memcpy(ra.recent_macs[ra.recent_macs_count++], mac, 6);
        } else {
            // FIFO shift: drop oldest, append new
            memmove(ra.recent_macs[0], ra.recent_macs[1],
                    (RANDOMIZED_MAC_WINDOW - 1) * 6);
            memcpy(ra.recent_macs[RANDOMIZED_MAC_WINDOW - 1], mac, 6);
        }
    }

    ssid_list_update(ra.ssids, &ra.ssid_count, ssid, now);
}

// ---------------------------------------------------------------------------
// parse_probe_request — called from main loop, routes to aggregation
// ---------------------------------------------------------------------------
static void parse_probe_request(const uint8_t* frame, uint16_t len,
                                int8_t rssi, uint8_t channel) {
    if (len < 24) return;

    // Source MAC at bytes 10-15
    uint8_t src_mac[6];
    memcpy(src_mac, &frame[10], 6);
    bool randomized = (src_mac[0] & PROBE_MAC_RANDOMIZED_BIT) != 0;

    // Parse SSID from tagged IEs (tag 0, always first in probe requests)
    char ssid[33] = {};
    uint16_t pos = 24;
    while (pos + 2 <= len) {
        uint8_t tag_id  = frame[pos];
        uint8_t tag_len = frame[pos + 1];
        if (pos + 2 + tag_len > len) break;
        if (tag_id == 0) {
            uint8_t copy_len = (tag_len > 32) ? 32 : tag_len;
            memcpy(ssid, &frame[pos + 2], copy_len);
            ssid[copy_len] = '\0';
            break;
        }
        pos += 2 + tag_len;
    }

    uint32_t now = millis();

    // Write to raw ring (debug/tail use only)
    ProbeRequest& raw = state.raw[state.raw_write_index % RAW_BUFFER_SIZE];
    memcpy(raw.src_mac, src_mac, 6);
    strncpy(raw.ssid_probed, ssid, 32);
    raw.ssid_probed[32] = '\0';
    raw.rssi           = rssi;
    raw.channel        = channel;
    raw.timestamp_ms   = now;
    raw.mac_randomized = randomized;
    state.raw_write_index++;

    state.total_probes++;

    // Route to aggregation
    if (randomized) {
        randomized_aggregate_update(src_mac, ssid, rssi, channel, now);
    } else {
        DeviceEntry* dev = find_or_create_device(src_mac);
        device_update(dev, ssid, rssi, channel, now);
    }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void probe_sniffer_init() {
    memset(&state, 0, sizeof(state));
}

void probe_sniffer_start() {
    if (state.running) return;  // Already running — keep accumulated data
    memset(&state, 0, sizeof(state));
    state.running          = true;
    state.session_start_ms = millis();
    // Flush ISR ring
    portENTER_CRITICAL(&probe_ring_mux);
    probe_ring_head = 0;
    probe_ring_tail = 0;
    portEXIT_CRITICAL(&probe_ring_mux);
}

void probe_sniffer_stop() {
    state.running = false;
    // State persists in memory; next start() will memset
}

void probe_sniffer_update() {
    // Drain ISR ring buffer into aggregation layer
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

void IRAM_ATTR probe_sniffer_on_frame(const uint8_t* frame, uint16_t len,
                                       int8_t rssi, uint8_t channel) {
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
