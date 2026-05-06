#include "ble_scanner.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/semphr.h>
#include <string.h>

static BleScannerState state;
static NimBLEScan* pScan = NULL;
static SemaphoreHandle_t ble_mutex = NULL;

// --- Manufacturer lookup table (25 entries from BT SIG) ---
struct MfrEntry {
    uint16_t id;
    const char* name;
};

static const MfrEntry mfr_table[] = {
    { 0x004C, "Apple" },
    { 0x0006, "Microsoft" },
    { 0x000F, "Broadcom" },
    { 0x000D, "Texas Instruments" },
    { 0x0059, "Nordic Semi" },
    { 0x00E0, "Google" },
    { 0x0075, "Samsung" },
    { 0x0171, "Amazon" },
    { 0x0157, "Xiaomi" },
    { 0x038F, "Garmin" },
    { 0x0087, "Fitbit" },
    { 0x0310, "Tile" },
    { 0x02FF, "Bose" },
    { 0x012D, "Sony" },
    { 0x0269, "Sonos" },
    { 0x0499, "Ruuvi" },
    { 0x0822, "Govee" },
    { 0x0131, "Huawei" },
    { 0x0046, "MediaTek" },
    { 0x0002, "Intel" },
    { 0x000A, "Qualcomm" },
    { 0x0056, "Harman" },
    { 0x0094, "Bang & Olufsen" },
    { 0x0078, "Nike" },
    { 0x038B, "Espressif" },
    { 0x0001, "Nokia" },
    { 0x0009, "Infineon" },
    { 0x001D, "Qualcomm" },
    { 0x00D2, "Xiaomi" },
    { 0x0386, "LG" },
    { 0x0583, "Anker" },
    { 0x0310, "Tile" },
};
static const size_t MFR_TABLE_SIZE = sizeof(mfr_table) / sizeof(mfr_table[0]);

const char* ble_manufacturer_lookup(uint16_t company_id) {
    for (size_t i = 0; i < MFR_TABLE_SIZE; i++) {
        if (mfr_table[i].id == company_id) return mfr_table[i].name;
    }
    return NULL;
}

const char* ble_device_type_str(uint8_t type) {
    switch (type) {
        case BLE_TYPE_PHONE:      return "Phone";
        case BLE_TYPE_COMPUTER:   return "Computer";
        case BLE_TYPE_WATCH:      return "Watch";
        case BLE_TYPE_HEADPHONES: return "Headphones";
        case BLE_TYPE_SPEAKER:    return "Speaker";
        case BLE_TYPE_BEACON:     return "Beacon";
        case BLE_TYPE_IOT:        return "IoT";
        case BLE_TYPE_TRACKER:    return "Tracker";
        case BLE_TYPE_TV:         return "TV";
        case BLE_TYPE_PERIPHERAL: return "Peripheral";
        default:                  return "Unknown";
    }
}

// --- Name-based manufacturer/device guessing ---
// More reliable than company_id since the device name is set by the actual device

struct NamePattern {
    const char* pattern;
    const char* manufacturer;
    uint8_t     device_type;  // 0 = don't override
};

static const NamePattern name_patterns[] = {
    { "iPhone",     "Apple",     BLE_TYPE_PHONE },
    { "iPad",       "Apple",     BLE_TYPE_PHONE },
    { "MacBook",    "Apple",     BLE_TYPE_COMPUTER },
    { "iMac",       "Apple",     BLE_TYPE_COMPUTER },
    { "Apple Watch","Apple",     BLE_TYPE_WATCH },
    { "AirPods",    "Apple",     BLE_TYPE_HEADPHONES },
    { "AirTag",     "Apple",     BLE_TYPE_TRACKER },
    { "HomePod",    "Apple",     BLE_TYPE_SPEAKER },
    { "Apple TV",   "Apple",     BLE_TYPE_TV },
    { "Samsung",    "Samsung",   0 },
    { "Galaxy",     "Samsung",   0 },
    { "Pixel",      "Google",    BLE_TYPE_PHONE },
    { "Chromecast", "Google",    BLE_TYPE_TV },
    { "Nest",       "Google",    BLE_TYPE_IOT },
    { "Surface",    "Microsoft", BLE_TYPE_COMPUTER },
    { "Xbox",       "Microsoft", BLE_TYPE_PERIPHERAL },
    { "ThinkPad",   "Lenovo",    BLE_TYPE_COMPUTER },
    { "Tile",       "Tile",      BLE_TYPE_TRACKER },
    { "JBL",        "JBL",       BLE_TYPE_SPEAKER },
    { "Bose",       "Bose",      BLE_TYPE_HEADPHONES },
    { "Sony",       "Sony",      0 },
    { "WH-",        "Sony",      BLE_TYPE_HEADPHONES },
    { "WF-",        "Sony",      BLE_TYPE_HEADPHONES },
    { "Sonos",      "Sonos",     BLE_TYPE_SPEAKER },
    { "Garmin",     "Garmin",    BLE_TYPE_WATCH },
    { "Fitbit",     "Fitbit",    BLE_TYPE_WATCH },
    { "Mi Band",    "Xiaomi",    BLE_TYPE_WATCH },
    { "Xiaomi",     "Xiaomi",    0 },
    { "Huawei",     "Huawei",    0 },
    { "HUAWEI",     "Huawei",    0 },
    { "Govee",      "Govee",     BLE_TYPE_IOT },
    { "Ruuvi",      "Ruuvi",     BLE_TYPE_BEACON },
    { "LG",         "LG",        0 },
    { "[LG]",       "LG",        BLE_TYPE_TV },
    { "webOS",      "LG",        BLE_TYPE_TV },
    { "BRAVIA",     "Sony",      BLE_TYPE_TV },
    { "Roku",       "Roku",      BLE_TYPE_TV },
    { "Fire TV",    "Amazon",    BLE_TYPE_TV },
    { "Echo",       "Amazon",    BLE_TYPE_SPEAKER },
    { "Alexa",      "Amazon",    BLE_TYPE_SPEAKER },
    { "Anker",      "Anker",     0 },
    { "Soundcore",  "Anker",     BLE_TYPE_SPEAKER },
    { "B&O",        "Bang & Olufsen", BLE_TYPE_SPEAKER },
    { "Bang",       "Bang & Olufsen", BLE_TYPE_SPEAKER },
    { "Nokia",      "Nokia",     0 },
    { "OnePlus",    "OnePlus",   BLE_TYPE_PHONE },
    { "OPPO",       "OPPO",      BLE_TYPE_PHONE },
};
static const size_t NAME_PATTERN_COUNT = sizeof(name_patterns) / sizeof(name_patterns[0]);

// Case-insensitive substring search
static bool contains_ci(const char* haystack, const char* needle) {
    if (!haystack || !needle) return false;
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            char h = haystack[i + j];
            char n = needle[j];
            if (h >= 'A' && h <= 'Z') h += 32;
            if (n >= 'A' && n <= 'Z') n += 32;
            if (h != n) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// --- Apple manufacturer data subtype parsing ---
// Apple's manufacturer data: company_id (0x004C) + type byte + length + data
// Type bytes identify the Apple protocol in use

static uint8_t guess_apple_device_type(const uint8_t* data, size_t len) {
    // data starts AFTER the 2-byte company ID
    if (len < 2) return BLE_TYPE_UNKNOWN;
    uint8_t apple_type = data[0];
    switch (apple_type) {
        case 0x02: return BLE_TYPE_BEACON;      // iBeacon
        case 0x05: return BLE_TYPE_PHONE;       // AirDrop
        case 0x07: return BLE_TYPE_PHONE;       // AirPods proximity / Apple Nearby
        case 0x09: return BLE_TYPE_PHONE;       // AirPlay Target
        case 0x0A: return BLE_TYPE_PHONE;       // AirPlay Source
        case 0x0C: return BLE_TYPE_PHONE;       // Handoff
        case 0x0F: return BLE_TYPE_PHONE;       // Nearby Action (share WiFi pwd etc)
        case 0x10: return BLE_TYPE_PHONE;       // Nearby Info
        case 0x12: return BLE_TYPE_TRACKER;     // Find My (AirTag, 3rd party trackers)
        case 0x19: return BLE_TYPE_HEADPHONES;  // Audio (AirPods, Beats)
        default:   return BLE_TYPE_UNKNOWN;
    }
}

// --- Guess device type from all available data ---
static uint8_t guess_device_type(NimBLEAdvertisedDevice* dev) {
    // 1. Check BLE appearance (most reliable when present)
    if (dev->haveAppearance()) {
        uint16_t appearance = dev->getAppearance();
        uint16_t category = appearance >> 6;
        switch (category) {
            case 1:  return BLE_TYPE_PHONE;
            case 2:  return BLE_TYPE_COMPUTER;
            case 3:  return BLE_TYPE_WATCH;
            case 4:  return BLE_TYPE_WATCH;       // Clock
            case 15: return BLE_TYPE_PERIPHERAL;   // HID
            default: break;
        }
        if (appearance >= 0x0C40 && appearance <= 0x0C43) return BLE_TYPE_SPEAKER;
        if (appearance >= 0x0941 && appearance <= 0x0948) return BLE_TYPE_HEADPHONES;
        if (appearance >= 0x0200 && appearance <= 0x023F) return BLE_TYPE_PERIPHERAL;  // HID range
        if (appearance >= 0x0340 && appearance <= 0x0347) return BLE_TYPE_TV;  // Display
    }

    // 2. Check service UUIDs (both 16-bit and 128-bit)
    if (dev->haveServiceUUID()) {
        int count = dev->getServiceUUIDCount();
        for (int i = 0; i < count; i++) {
            NimBLEUUID uuid = dev->getServiceUUID(i);
            if (uuid.bitSize() == 16) {
                uint16_t u16 = uuid.getNative()->u16.value;
                switch (u16) {
                    case 0x1812: return BLE_TYPE_PERIPHERAL;  // HID
                    case 0x180D: return BLE_TYPE_WATCH;       // Heart Rate
                    case 0x1814: return BLE_TYPE_WATCH;       // Running Speed
                    case 0x1816: return BLE_TYPE_WATCH;       // Cycling Speed
                    case 0x181C: return BLE_TYPE_WATCH;       // Body Composition
                    case 0x1819: return BLE_TYPE_IOT;         // Location/Navigation
                    case 0x1803: return BLE_TYPE_TRACKER;     // Link Loss (trackers)
                    case 0x1802: return BLE_TYPE_TRACKER;     // Immediate Alert (trackers)
                    case 0x111E: return BLE_TYPE_HEADPHONES;  // Handsfree
                    case 0x110B: return BLE_TYPE_SPEAKER;     // Audio Sink
                    case 0x110A: return BLE_TYPE_SPEAKER;     // Audio Source
                }
            }
        }
    }

    // 3. Parse Apple manufacturer data subtypes (if company is Apple)
    if (dev->haveManufacturerData()) {
        std::string mfr = dev->getManufacturerData();
        if (mfr.size() >= 4) {
            uint16_t cid = (uint8_t)mfr[0] | ((uint8_t)mfr[1] << 8);
            if (cid == 0x004C) {
                uint8_t apple_type = guess_apple_device_type(
                    (const uint8_t*)mfr.data() + 2, mfr.size() - 2);
                if (apple_type != BLE_TYPE_UNKNOWN) return apple_type;
            }
        }
    }

    // 4. Check device name patterns (last resort)
    if (dev->haveName()) {
        const char* name = dev->getName().c_str();
        for (size_t i = 0; i < NAME_PATTERN_COUNT; i++) {
            if (name_patterns[i].device_type != 0 && contains_ci(name, name_patterns[i].pattern)) {
                return name_patterns[i].device_type;
            }
        }
    }

    return BLE_TYPE_UNKNOWN;
}

// --- Guess manufacturer from device name (more reliable than company_id) ---
static void guess_manufacturer(BleDevice* entry, NimBLEAdvertisedDevice* dev) {
    entry->mfr_name[0] = '\0';

    // 1. Check device name first — most reliable
    if (dev->haveName()) {
        const char* name = dev->getName().c_str();
        for (size_t i = 0; i < NAME_PATTERN_COUNT; i++) {
            if (contains_ci(name, name_patterns[i].pattern)) {
                strncpy(entry->mfr_name, name_patterns[i].manufacturer,
                        sizeof(entry->mfr_name) - 1);
                return;
            }
        }
    }

    // 2. Fall back to company_id from manufacturer data
    // But ONLY if it's not Apple's 0x004C (too many false positives)
    if (entry->company_id != 0 && entry->company_id != 0x004C) {
        const char* mfr = ble_manufacturer_lookup(entry->company_id);
        if (mfr) {
            strncpy(entry->mfr_name, mfr, sizeof(entry->mfr_name) - 1);
            return;
        }
    }

    // 3. For Apple company_id: only claim Apple if device type suggests it
    if (entry->company_id == 0x004C) {
        // Parse Apple subtype to decide
        if (dev->haveManufacturerData()) {
            std::string mfr = dev->getManufacturerData();
            if (mfr.size() >= 4) {
                uint8_t apple_type = (uint8_t)mfr[2];
                // These subtypes are genuinely Apple device protocols
                switch (apple_type) {
                    case 0x05: // AirDrop
                    case 0x07: // Nearby
                    case 0x09: // AirPlay
                    case 0x0A: // AirPlay Source
                    case 0x0C: // Handoff
                    case 0x0F: // Nearby Action
                    case 0x10: // Nearby Info
                    case 0x19: // AirPods/Beats audio
                        strncpy(entry->mfr_name, "Apple", sizeof(entry->mfr_name) - 1);
                        return;
                    case 0x12: // Find My — could be 3rd party tracker
                        strncpy(entry->mfr_name, "Find My", sizeof(entry->mfr_name) - 1);
                        return;
                    case 0x02: // iBeacon — 3rd party
                        strncpy(entry->mfr_name, "iBeacon", sizeof(entry->mfr_name) - 1);
                        return;
                }
            }
        }
    }
}

// --- Scan callback ---
class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        BleDevice entry;
        memset(&entry, 0, sizeof(entry));

        // MAC
        NimBLEAddress addr = dev->getAddress();
        memcpy(entry.mac, addr.getNative(), 6);
        entry.addr_type = (uint8_t)addr.getType();

        // Name
        if (dev->haveName()) {
            strncpy(entry.name, dev->getName().c_str(), sizeof(entry.name) - 1);
        }

        // RSSI
        entry.rssi = dev->getRSSI();
        entry.tx_power = dev->haveTXPower() ? dev->getTXPower() : INT8_MIN;

        // Manufacturer data — extract company_id
        if (dev->haveManufacturerData()) {
            std::string mfr = dev->getManufacturerData();
            if (mfr.size() >= 2) {
                entry.company_id = (uint8_t)mfr[0] | ((uint8_t)mfr[1] << 8);
            }
        }

        // Device type (uses appearance, service UUIDs, Apple subtypes, name)
        entry.device_type = guess_device_type(dev);

        // Manufacturer guess (uses name patterns, then company_id with Apple filtering)
        guess_manufacturer(&entry, dev);

        // Service UUIDs (16-bit only, up to 8)
        entry.service_count = 0;
        if (dev->haveServiceUUID()) {
            int count = dev->getServiceUUIDCount();
            for (int i = 0; i < count && entry.service_count < 8; i++) {
                NimBLEUUID uuid = dev->getServiceUUID(i);
                if (uuid.bitSize() == 16) {
                    entry.service_uuids[entry.service_count++] = uuid.getNative()->u16.value;
                }
            }
        }

        uint32_t now = millis();
        entry.first_seen_ms = now;
        entry.last_seen_ms = now;

        // Mutex-protect all shared state access (5ms timeout — drop advertisement if busy)
        if (xSemaphoreTake(ble_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

        // Dedup by MAC
        for (uint8_t i = 0; i < state.device_count; i++) {
            if (memcmp(state.devices[i].mac, entry.mac, 6) == 0) {
                // Update existing
                state.devices[i].rssi = entry.rssi;
                state.devices[i].rssi_avg = (state.devices[i].rssi_avg * 3 + entry.rssi) / 4;
                state.devices[i].last_seen_ms = now;
                state.devices[i].stale = false;
                // Update name if we got one and didn't have one
                if (entry.name[0] && !state.devices[i].name[0]) {
                    strncpy(state.devices[i].name, entry.name, sizeof(state.devices[i].name) - 1);
                }
                // Update manufacturer if we got a better guess
                if (entry.mfr_name[0] && !state.devices[i].mfr_name[0]) {
                    strncpy(state.devices[i].mfr_name, entry.mfr_name, sizeof(state.devices[i].mfr_name) - 1);
                }
                if (entry.company_id && !state.devices[i].company_id) {
                    state.devices[i].company_id = entry.company_id;
                }
                if (entry.device_type != BLE_TYPE_UNKNOWN && state.devices[i].device_type == BLE_TYPE_UNKNOWN) {
                    state.devices[i].device_type = entry.device_type;
                }
                if (entry.service_count > state.devices[i].service_count) {
                    memcpy(state.devices[i].service_uuids, entry.service_uuids, sizeof(entry.service_uuids));
                    state.devices[i].service_count = entry.service_count;
                }
                xSemaphoreGive(ble_mutex);
                return;
            }
        }

        // Add new device
        entry.rssi_avg = entry.rssi;
        if (state.device_count < BLE_MAX_DEVICES) {
            state.devices[state.device_count] = entry;
            state.device_count++;
        } else {
            // Evict weakest stale device, or weakest overall
            uint8_t evict_idx = 0;
            int8_t weakest = 0;
            bool found_stale = false;
            for (uint8_t i = 0; i < state.device_count; i++) {
                if (state.devices[i].stale && (!found_stale || state.devices[i].rssi < weakest)) {
                    evict_idx = i;
                    weakest = state.devices[i].rssi;
                    found_stale = true;
                } else if (!found_stale && (i == 0 || state.devices[i].rssi < weakest)) {
                    evict_idx = i;
                    weakest = state.devices[i].rssi;
                }
            }
            if (entry.rssi > weakest) {
                state.devices[evict_idx] = entry;
            }
        }
        xSemaphoreGive(ble_mutex);
    }
};

static ScanCallbacks scanCb;

// --- Sort by RSSI descending ---
static void sort_devices() {
    for (uint8_t i = 1; i < state.device_count; i++) {
        BleDevice key = state.devices[i];
        int8_t j = i - 1;
        while (j >= 0 && state.devices[j].rssi < key.rssi) {
            state.devices[j + 1] = state.devices[j];
            j--;
        }
        state.devices[j + 1] = key;
    }
}

// --- Public API ---

void ble_scanner_init() {
    ble_mutex = xSemaphoreCreateMutex();
    NimBLEDevice::init("NetKnob");
    pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(&scanCb, true);  // wantDuplicates=true for live RSSI updates
    pScan->setActiveScan(true);   // Active scan for scan response (device names)
    pScan->setMaxResults(0);      // Don't cache results in NimBLE (we manage our own list)
    pScan->setInterval(160);      // 100ms in 0.625ms units
    pScan->setWindow(128);        // 80ms in 0.625ms units
    memset(&state, 0, sizeof(state));
    Serial.println("[ble] scanner initialized");
}

void ble_scanner_start() {
    if (!state.scanning && pScan) {
        pScan->start(0, nullptr, false);
        state.scanning = true;
        Serial.println("[ble] scan started");
    }
}

void ble_scanner_stop() {
    if (state.scanning && pScan) {
        pScan->stop();
        state.scanning = false;
        Serial.println("[ble] scan stopped");
    }
}

void ble_scanner_update() {
    if (xSemaphoreTake(ble_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

    uint32_t now = millis();

    for (uint8_t i = 0; i < state.device_count; ) {
        uint32_t age = now - state.devices[i].last_seen_ms;
        if (age > BLE_REMOVE_MS) {
            for (uint8_t j = i; j < state.device_count - 1; j++) {
                state.devices[j] = state.devices[j + 1];
            }
            state.device_count--;
            if (state.selected_index >= state.device_count && state.device_count > 0) {
                state.selected_index = 0;
            }
        } else {
            state.devices[i].stale = (age > BLE_STALE_MS);
            i++;
        }
    }

    sort_devices();
    xSemaphoreGive(ble_mutex);
}

BleScannerState* ble_scanner_get_state() {
    return &state;
}

bool ble_scanner_lock(uint32_t timeout_ms) {
    return xSemaphoreTake(ble_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void ble_scanner_unlock() {
    xSemaphoreGive(ble_mutex);
}
