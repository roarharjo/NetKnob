#pragma once

#include <stdint.h>

#define BLE_MAX_DEVICES     50
#define BLE_STALE_MS        60000
#define BLE_REMOVE_MS       120000

enum BleDeviceType {
    BLE_TYPE_UNKNOWN,
    BLE_TYPE_PHONE,
    BLE_TYPE_COMPUTER,
    BLE_TYPE_WATCH,
    BLE_TYPE_HEADPHONES,
    BLE_TYPE_SPEAKER,
    BLE_TYPE_BEACON,
    BLE_TYPE_IOT,
    BLE_TYPE_TRACKER,     // AirTag, Tile, etc.
    BLE_TYPE_TV,
    BLE_TYPE_PERIPHERAL   // Keyboard, mouse, gamepad
};

struct BleDevice {
    uint8_t   mac[6];
    uint8_t   addr_type;       // 0=Public, 1=Random, 2=RPA
    char      name[30];
    int8_t    rssi;
    int8_t    rssi_avg;
    int8_t    tx_power;
    uint8_t   device_type;     // BleDeviceType
    uint16_t  company_id;      // From manufacturer data AD type (who made the AD payload)
    char      mfr_name[16];    // Guessed device manufacturer (from name, OUI, or company_id)
    uint16_t  service_uuids[8];
    uint8_t   service_count;
    uint32_t  first_seen_ms;
    uint32_t  last_seen_ms;
    bool      stale;
};

struct BleScannerState {
    BleDevice devices[BLE_MAX_DEVICES];
    uint8_t   device_count;
    uint8_t   selected_index;
    uint8_t   scroll_offset;
    bool      scanning;
    bool      detail_view;
};

void ble_scanner_init();
void ble_scanner_start();
void ble_scanner_stop();
void ble_scanner_update();   // Call from main loop: age devices, mark stale, remove old
BleScannerState* ble_scanner_get_state();
bool ble_scanner_lock(uint32_t timeout_ms);
void ble_scanner_unlock();
const char* ble_manufacturer_lookup(uint16_t company_id);
const char* ble_device_type_str(uint8_t type);
