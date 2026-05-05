#include "ble_scanner.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string.h>

static BleScannerState state;
static NimBLEScan* pScan = NULL;

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
        default:                  return "Unknown";
    }
}

// --- Guess device type from appearance or service UUIDs ---
static uint8_t guess_device_type(NimBLEAdvertisedDevice* dev) {
    // Check appearance (if available)
    if (dev->haveAppearance()) {
        uint16_t appearance = dev->getAppearance();
        uint16_t category = appearance >> 6;  // Top 10 bits = category
        switch (category) {
            case 1: return BLE_TYPE_PHONE;       // Phone
            case 2: return BLE_TYPE_COMPUTER;     // Computer
            case 3: return BLE_TYPE_WATCH;        // Watch
            case 4: return BLE_TYPE_WATCH;        // Clock
            case 8: return BLE_TYPE_HEADPHONES;   // Headset/Headphones (broad category 8xx)
            case 9: return BLE_TYPE_SPEAKER;      // Speakers (broad)
            default: break;
        }
        // More specific appearance values
        if (appearance >= 0x0C40 && appearance <= 0x0C43) return BLE_TYPE_SPEAKER;
        if (appearance >= 0x0941 && appearance <= 0x0948) return BLE_TYPE_HEADPHONES;
    }

    // Check service UUIDs
    if (dev->haveServiceUUID()) {
        // HID service (0x1812) often indicates keyboard/mouse/gamepad
        if (dev->isAdvertisingService(NimBLEUUID((uint16_t)0x1812))) return BLE_TYPE_COMPUTER;
        // Heart Rate (0x180D) → watch/fitness
        if (dev->isAdvertisingService(NimBLEUUID((uint16_t)0x180D))) return BLE_TYPE_WATCH;
    }

    // Don't classify by manufacturer data alone — many non-Apple devices
    // broadcast Apple's company ID (0x004C) via iBeacon format.
    // Manufacturer data tells you who made the AD payload, not the device.

    return BLE_TYPE_UNKNOWN;
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

        // Device type
        entry.device_type = guess_device_type(dev);

        // Manufacturer data
        if (dev->haveManufacturerData()) {
            std::string mfr = dev->getManufacturerData();
            if (mfr.size() >= 2) {
                entry.company_id = (uint8_t)mfr[0] | ((uint8_t)mfr[1] << 8);
            }
        }

        // Service UUIDs (16-bit only, up to 8)
        entry.service_count = 0;
        if (dev->haveServiceUUID()) {
            int count = dev->getServiceUUIDCount();
            for (int i = 0; i < count && entry.service_count < 8; i++) {
                NimBLEUUID uuid = dev->getServiceUUID(i);
                // Only store 16-bit UUIDs
                if (uuid.bitSize() == 16) {
                    entry.service_uuids[entry.service_count++] = uuid.getNative()->u16.value;
                }
            }
        }

        uint32_t now = millis();
        entry.first_seen_ms = now;
        entry.last_seen_ms = now;

        // Dedup by MAC
        for (uint8_t i = 0; i < state.device_count; i++) {
            if (memcmp(state.devices[i].mac, entry.mac, 6) == 0) {
                // Update existing: RSSI running average, last_seen, and name if now available
                state.devices[i].rssi = entry.rssi;
                state.devices[i].rssi_avg = (state.devices[i].rssi_avg * 3 + entry.rssi) / 4;
                state.devices[i].last_seen_ms = now;
                state.devices[i].stale = false;
                if (entry.name[0] && !state.devices[i].name[0]) {
                    strncpy(state.devices[i].name, entry.name, sizeof(state.devices[i].name) - 1);
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
            int8_t weakest = 0;  // RSSI 0 means "not set yet"
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
    NimBLEDevice::init("NetKnob");
    pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(&scanCb, false);  // false = don't deduplicate (we do it ourselves)
    pScan->setActiveScan(true);   // Active scan to get scan response (device names)
    pScan->setInterval(160);      // 100ms in 0.625ms units
    pScan->setWindow(128);        // 80ms in 0.625ms units
    memset(&state, 0, sizeof(state));
    Serial.println("[ble] scanner initialized");
}

void ble_scanner_start() {
    if (!state.scanning && pScan) {
        pScan->start(0, nullptr, false);  // 0 = scan indefinitely, false = don't clear results between scans
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
    uint32_t now = millis();

    // Age and remove devices
    for (uint8_t i = 0; i < state.device_count; ) {
        uint32_t age = now - state.devices[i].last_seen_ms;
        if (age > BLE_REMOVE_MS) {
            // Remove by shifting
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

    // Sort periodically (every update call is fine since it's insertion sort on small list)
    sort_devices();
}

BleScannerState* ble_scanner_get_state() {
    return &state;
}
