#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <string.h>
#include "wifi_scanner.h"

// ---------------------------------------------------------------------------
// Ring buffer for ISR-safe frame passing
// ---------------------------------------------------------------------------
#define RING_SLOTS 8
#define RING_BUF_SIZE 256

struct RingSlot {
    uint8_t  data[RING_BUF_SIZE];
    uint16_t len;
    int8_t   rssi;
};

static RingSlot   ring_buf[RING_SLOTS];
static volatile uint8_t ring_head = 0;   // written by callback
static volatile uint8_t ring_tail = 0;   // read by main loop
static portMUX_TYPE ring_mux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// OUI lookup table (20 entries)
// ---------------------------------------------------------------------------
struct OuiEntry {
    uint8_t     oui[3];
    const char* vendor;
};

static const OuiEntry oui_table[] = {
    { { 0x00, 0x1A, 0x2B }, "Apple"         },
    { { 0xC8, 0x2A, 0x14 }, "Samsung"       },
    { { 0x8C, 0xAA, 0xB5 }, "Samsung"       },
    { { 0xB0, 0xBE, 0x76 }, "TP-Link"       },
    { { 0xF8, 0x1A, 0x67 }, "TP-Link"       },
    { { 0xAC, 0x84, 0xC6 }, "TP-Link"       },
    { { 0x20, 0xA6, 0xCD }, "Netgear"       },
    { { 0xB4, 0xFB, 0xE4 }, "Ubiquiti"      },
    { { 0xFC, 0xEC, 0xDA }, "Ubiquiti"      },
    { { 0xDC, 0x9F, 0xDB }, "Ubiquiti"      },
    { { 0x78, 0x8A, 0x20 }, "Ubiquiti"      },
    { { 0x00, 0x24, 0xD7 }, "Intel"         },
    { { 0x34, 0x02, 0x86 }, "Intel"         },
    { { 0xE8, 0x6F, 0x38 }, "Cisco/Meraki"  },
    { { 0x00, 0x18, 0x74 }, "Cisco"         },
    { { 0xD8, 0xB3, 0x70 }, "Asus"          },
    { { 0xAC, 0x9E, 0x17 }, "Asus"          },
    { { 0xDC, 0xA6, 0x32 }, "Raspberry Pi"  },
    { { 0x28, 0x6C, 0x07 }, "Xiaomi"        },
    { { 0x64, 0xCE, 0x73 }, "Xiaomi"        },
};

static const size_t OUI_TABLE_SIZE = sizeof(oui_table) / sizeof(oui_table[0]);

// ---------------------------------------------------------------------------
// Scanner state
// ---------------------------------------------------------------------------
static WifiScannerState state;

// ---------------------------------------------------------------------------
// Promiscuous callback (IRAM_ATTR — runs in WiFi task context)
// ---------------------------------------------------------------------------
static void IRAM_ATTR promisc_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
    // Only management frames
    if (type != WIFI_PKT_MGMT) return;

    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    const uint8_t* frame = pkt->payload;
    uint16_t frame_len = pkt->rx_ctrl.sig_len;

    // Need at least the fixed fields through capability info (36 bytes)
    if (frame_len < 36) return;

    // Check subtype: beacon (0x80) or probe response (0x50)
    uint8_t fc0 = frame[0];
    if (fc0 != 0x80 && fc0 != 0x50) return;

    // Copy to ring buffer
    portENTER_CRITICAL(&ring_mux);
    uint8_t next_head = (ring_head + 1) % RING_SLOTS;
    if (next_head != ring_tail) {
        uint16_t copy_len = (frame_len > RING_BUF_SIZE) ? RING_BUF_SIZE : frame_len;
        memcpy(ring_buf[ring_head].data, frame, copy_len);
        ring_buf[ring_head].len  = copy_len;
        ring_buf[ring_head].rssi = pkt->rx_ctrl.rssi;
        ring_head = next_head;
    }
    portEXIT_CRITICAL(&ring_mux);
}

// ---------------------------------------------------------------------------
// Beacon / probe response parser (called from main loop)
// ---------------------------------------------------------------------------
static void parse_frame(const uint8_t* frame, uint16_t len, int8_t rssi) {
    if (len < 36) return;

    AccessPoint ap;
    memset(&ap, 0, sizeof(ap));

    // BSSID at bytes 16-21
    memcpy(ap.bssid, &frame[16], 6);
    ap.rssi = rssi;
    ap.hidden = false;

    // Capability info at bytes 34-35 (little-endian)
    uint16_t cap_info = frame[34] | (frame[35] << 8);
    bool privacy = (cap_info & (1 << 4)) != 0;

    // Walk tagged IEs starting at byte 36
    bool has_rsn = false;
    bool has_wpa = false;
    bool has_sae = false;
    uint16_t pos = 36;

    while (pos + 2 <= len) {
        uint8_t tag_id  = frame[pos];
        uint8_t tag_len = frame[pos + 1];

        if (pos + 2 + tag_len > len) break;

        const uint8_t* tag_data = &frame[pos + 2];

        switch (tag_id) {
            case 0: {
                // SSID
                if (tag_len == 0) {
                    ap.hidden = true;
                    ap.ssid[0] = '\0';
                } else {
                    uint8_t copy_len = (tag_len > 32) ? 32 : tag_len;
                    memcpy(ap.ssid, tag_data, copy_len);
                    ap.ssid[copy_len] = '\0';
                    // Check for hidden SSID (all zero bytes)
                    bool all_zero = true;
                    for (uint8_t i = 0; i < copy_len; i++) {
                        if (tag_data[i] != 0) { all_zero = false; break; }
                    }
                    if (all_zero) ap.hidden = true;
                }
                break;
            }
            case 3: {
                // DS Parameter Set — channel
                if (tag_len >= 1) {
                    ap.channel = tag_data[0];
                }
                break;
            }
            case 48: {
                // RSN IE (WPA2)
                has_rsn = true;
                // Check for SAE (WPA3): walk AKM suite list
                // RSN IE structure: version(2) + group cipher(4) + pairwise count(2) + pairwise suites(n*4) + AKM count(2) + AKM suites(n*4)
                if (tag_len >= 8) {
                    uint16_t offset = 2 + 4; // skip version + group cipher
                    // Pairwise cipher count
                    if (offset + 2 <= tag_len) {
                        uint16_t pw_count = tag_data[offset] | (tag_data[offset + 1] << 8);
                        if (pw_count > (tag_len - offset - 2) / 4) break;  // malformed IE
                        offset += 2 + pw_count * 4;
                        // AKM count
                        if (offset + 2 <= tag_len) {
                            uint16_t akm_count = tag_data[offset] | (tag_data[offset + 1] << 8);
                            offset += 2;
                            for (uint16_t i = 0; i < akm_count && offset + 4 <= tag_len; i++) {
                                // AKM suite OUI = 00:0F:AC, type 8 = SAE
                                if (tag_data[offset] == 0x00 && tag_data[offset + 1] == 0x0F &&
                                    tag_data[offset + 2] == 0xAC && tag_data[offset + 3] == 0x08) {
                                    has_sae = true;
                                }
                                offset += 4;
                            }
                        }
                    }
                }
                break;
            }
            case 221: {
                // Vendor specific — check for WPA IE (OUI 00:50:F2, type 1)
                if (tag_len >= 4 &&
                    tag_data[0] == 0x00 && tag_data[1] == 0x50 &&
                    tag_data[2] == 0xF2 && tag_data[3] == 0x01) {
                    has_wpa = true;
                }
                break;
            }
            default:
                break;
        }

        pos += 2 + tag_len;
    }

    // Determine encryption type
    if (has_sae) {
        ap.encryption = 4; // WPA3
    } else if (has_rsn) {
        ap.encryption = 3; // WPA2
    } else if (has_wpa) {
        ap.encryption = 2; // WPA
    } else if (privacy) {
        ap.encryption = 1; // WEP
    } else {
        ap.encryption = 0; // OPEN
    }

    ap.last_seen = millis();

    // Deduplication by BSSID — always update to track current RSSI
    for (uint8_t i = 0; i < state.ap_count; i++) {
        if (memcmp(state.ap_list[i].bssid, ap.bssid, 6) == 0) {
            state.ap_list[i].rssi = ap.rssi;
            state.ap_list[i].last_seen = ap.last_seen;
            return;
        }
    }

    // Add new AP
    if (state.ap_count < MAX_APS_PER_CHANNEL) {
        state.ap_list[state.ap_count] = ap;
        state.ap_count++;
    } else {
        // Replace weakest AP if new one is stronger
        uint8_t weakest_idx = 0;
        int8_t  weakest_rssi = state.ap_list[0].rssi;
        for (uint8_t i = 1; i < state.ap_count; i++) {
            if (state.ap_list[i].rssi < weakest_rssi) {
                weakest_rssi = state.ap_list[i].rssi;
                weakest_idx = i;
            }
        }
        if (ap.rssi > weakest_rssi) {
            state.ap_list[weakest_idx] = ap;
        }
    }
}

// ---------------------------------------------------------------------------
// Sort APs by RSSI descending (insertion sort)
// ---------------------------------------------------------------------------
static void sort_aps() {
    // Insertion sort by RSSI descending — with 3 dBm hysteresis to prevent flickering
    for (uint8_t i = 1; i < state.ap_count; i++) {
        AccessPoint key = state.ap_list[i];
        int8_t j = i - 1;
        while (j >= 0 && state.ap_list[j].rssi < key.rssi - 3) {
            state.ap_list[j + 1] = state.ap_list[j];
            j--;
        }
        state.ap_list[j + 1] = key;
    }

    // Restore selected_index to follow the BSSID
    for (uint8_t i = 0; i < state.ap_count; i++) {
        if (memcmp(state.ap_list[i].bssid, state.selected_bssid, 6) == 0) {
            state.selected_index = i;
            return;
        }
    }
    // If selected BSSID no longer in list, reset to 0
    if (state.ap_count > 0) {
        state.selected_index = 0;
        memcpy(state.selected_bssid, state.ap_list[0].bssid, 6);
    }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void scanner_init() {
    memset(&state, 0, sizeof(state));
    state.current_channel = 1;
    state.scanning = false;
    state.detail_view = false;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(promisc_callback);

    scanner_set_channel(1);
}

void scanner_set_channel(uint8_t ch) {
    if (ch < CHANNEL_MIN) ch = CHANNEL_MIN;
    if (ch > CHANNEL_MAX) ch = CHANNEL_MAX;

    state.current_channel = ch;
    state.ap_count = 0;
    state.selected_index = 0;
    state.scanning = true;
    state.scan_start_ms = millis();

    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

void scanner_update() {
    // Drain ring buffer and parse each frame
    while (true) {
        portENTER_CRITICAL(&ring_mux);
        bool has_data = (ring_tail != ring_head);
        RingSlot slot;
        if (has_data) {
            slot = ring_buf[ring_tail];
            ring_tail = (ring_tail + 1) % RING_SLOTS;
        }
        portEXIT_CRITICAL(&ring_mux);

        if (!has_data) break;

        parse_frame(slot.data, slot.len, slot.rssi);
    }

    // Check if dwell time has elapsed
    if (state.scanning && (millis() - state.scan_start_ms >= DWELL_TIME_MS)) {
        state.scanning = false;
        sort_aps();
    }

    // Age out APs not seen for 5 seconds
    uint32_t now = millis();
    for (uint8_t i = 0; i < state.ap_count; ) {
        if (now - state.ap_list[i].last_seen > 5000) {
            // Remove by shifting remaining entries down
            for (uint8_t j = i; j < state.ap_count - 1; j++) {
                state.ap_list[j] = state.ap_list[j + 1];
            }
            state.ap_count--;
            // Fix selected_index
            if (state.selected_index >= state.ap_count && state.ap_count > 0) {
                state.selected_index = 0;
                memcpy(state.selected_bssid, state.ap_list[0].bssid, 6);
            }
        } else {
            i++;
        }
    }

    // Auto-restart: keep scanning same channel, updating RSSI in place.
    if (!state.scanning && (millis() - state.scan_start_ms >= DWELL_TIME_MS + 400)) {
        state.scanning = true;
        state.scan_start_ms = millis();
    }
}

WifiScannerState* scanner_get_state() {
    return &state;
}

const char* oui_lookup(const uint8_t bssid[6]) {
    for (size_t i = 0; i < OUI_TABLE_SIZE; i++) {
        if (bssid[0] == oui_table[i].oui[0] &&
            bssid[1] == oui_table[i].oui[1] &&
            bssid[2] == oui_table[i].oui[2]) {
            return oui_table[i].vendor;
        }
    }
    return "Unknown";
}

const char* encryption_str(uint8_t enc) {
    switch (enc) {
        case 0: return "OPEN";
        case 1: return "WEP";
        case 2: return "WPA";
        case 3: return "WPA2";
        case 4: return "WPA3";
        default: return "???";
    }
}

uint16_t channel_to_freq(uint8_t ch) {
    if (ch >= 1 && ch <= 13) {
        return 2407 + ch * 5;
    }
    if (ch == 14) {
        return 2484;
    }
    return 0;
}
