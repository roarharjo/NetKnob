# Session Review — Phase 2 Bugfix Session

> Retrospective on the bugfix session targeting 4 issues from SESSION-REVIEW-P2.
> Date: 2026-05-06

---

## Bugs Fixed

### Bug #2 — BLE Race Condition (CRITICAL)

**Root cause:** NimBLE callback runs in BLE FreeRTOS task, main loop reads/writes the same `BleScannerState` without synchronization.

**Fix (3 files):**
- `ble_scanner.cpp`: Added FreeRTOS mutex, created in `ble_scanner_init()`. Callback wraps shared state access with 5ms timeout (drops advertisement if busy). `ble_scanner_update()` holds mutex during aging/sorting.
- `ble_scanner.h`: Exposed `ble_scanner_lock()` / `ble_scanner_unlock()` API.
- `scr_ble_scan.cpp`: Snapshot pattern — copies full `BleScannerState` under short mutex hold, renders from the copy. Mutex held for ~50us (memcpy only).

### Bug #1 — WiFi AP List Too Small (MEDIUM)

**Root cause:** Only 4 AP rows at 20px font with 44px spacing. Unreadable on busy channels.

**Fix:**
- Enabled `LV_FONT_MONTSERRAT_12` in `platformio.ini`
- `scr_wifi_scan.cpp`: `VISIBLE_AP_ROWS` 4 -> 12, font 20px -> 12px, row spacing 44px -> 18px
- Added `scroll_offset` with auto-scroll on tap selection
- RSSI simplified to color-coded dBm number (saves horizontal space)
- Encoder still hops channels, tap cycles selection, scroll resets on channel change

### Bug #4 — `display_clear()` Dead Code (COSMETIC)

Removed function from `display.cpp` and declaration from `display.h`. Grep confirmed zero references.

### Bug #3 — Crosshair Lines Through Text (COSMETIC)

Removed horizontal and vertical crosshair line objects from `build_scan_screen()` in `scr_wifi_scan.cpp`. Concentric rings and glow provide sufficient visual depth.

---

## Additional Fixes Discovered During Session

### BLE Use-After-Free in `getName()` (CRITICAL — Root Crash Cause)

**Root cause:** `dev->getName()` returns `std::string` by value. Two functions stored `.c_str()` of the temporary in a `const char*` and used it after the temporary was destroyed:

```cpp
// BEFORE (dangling pointer):
const char* name = dev->getName().c_str();  // temporary destroyed at semicolon
for (...) contains_ci(name, ...);           // use-after-free

// AFTER (safe):
std::string nameStr = dev->getName();       // local copy, lives through scope
for (...) contains_ci(nameStr.c_str(), ...);
```

Fixed in both `guess_device_type()` and `guess_manufacturer()`.

### NimBLE Internal Cache Heap Leak

**Root cause:** Default `setMaxResults(0xFF)` causes NimBLE to cache all discovered devices internally alongside our own device list. With continuous scan (`duration=0`), this cache grows without bound until heap exhaustion.

**Fix:** `pScan->setMaxResults(0)` — NimBLE operates in "callbacks only" mode, erasing devices from its internal cache after each callback. Our `state.devices[]` is the sole device store.

### NimBLE Task Stack Size

Increased `CONFIG_BT_NIMBLE_TASK_STACK_SIZE` from default 4096 to 8192 as a safety margin for the callback chain depth.

### Boot Diagnostics

Added `esp_reset_reason()` logging at boot — prints POWERON, PANIC, INT_WDT, TASK_WDT, etc. Helps diagnose crash type after reboot.

Added BLE device count to serial heartbeat (`ble=%d`).

---

## Investigation That Led to Root Cause

The bugfix doc identified the race condition as the critical bug. After fixing it with mutex + snapshot, the device still crashed. Serial monitoring showed:

1. **Heap was stable** — no leak with `setMaxResults(0)` -> ruled out memory exhaustion
2. **No crash during monitoring** — crash happened between sessions -> needed longer test
3. **`getName()` returns by value** — checked NimBLE header, confirmed `std::string getName();` (not reference)
4. **Dangling pointer in two functions** — `guess_device_type()` and `guess_manufacturer()` both stored `.c_str()` of temporary

After fixing the dangling pointer, extended testing (20+ minutes) with serial monitoring showed zero crashes, zero reboots, and perfectly stable heap.

**False positive:** Several reported "crashes" were actually the auto-lock timeout switching to the Safe Lock screen. Confirmed by serial logs showing no reboot and `reset reason: UNKNOWN (0)` (power-on, not panic).

---

## NimBLE Scan Response vs Device Names

During debugging, discovered that BLE device names are delivered via scan responses (separate packets from advertisements). The interaction between NimBLE's caching, `wantDuplicates`, and `setMaxResults` determines whether names are available:

| Config | Names | RSSI Updates | Heap Stable | Stable |
|--------|-------|-------------|-------------|--------|
| `wantDuplicates=false`, `maxResults=0xFF` | Some (original) | No | No (leak) | Crashed |
| `wantDuplicates=true`, `maxResults=0` | ADV-only | Yes | Yes | Yes |
| `wantDuplicates=true`, `maxResults=0xFF` + periodic clear | None | Yes | Yes | Yes |
| `wantDuplicates=false`, `maxResults=0xFF` + name sync | Cache names | No | Yes | Crashed (name sync) |

**Selected config:** `wantDuplicates=true` + `setMaxResults(0)` — maximum stability, live RSSI, names limited to devices that embed them in ADV packets (not scan responses).

**Known limitation:** Devices that only advertise names in scan responses show as "Unknown". This includes most phones and some watches. Devices like LG TVs, BYD cars, Oura rings embed names in ADV packets and show correctly.

---

## Files Changed

| File | Change |
|------|--------|
| `src/ble_scanner.h` | Added `ble_scanner_lock()` / `ble_scanner_unlock()` |
| `src/ble_scanner.cpp` | Mutex, `setMaxResults(0)`, `wantDuplicates=true`, dangling pointer fix |
| `src/screens/scr_ble_scan.cpp` | Snapshot rendering pattern |
| `src/screens/scr_wifi_scan.cpp` | 12px font, 12 rows, scroll, removed crosshairs |
| `src/display.cpp` | Removed `display_clear()` |
| `src/display.h` | Removed `display_clear()` declaration |
| `src/main.cpp` | Reset reason logging, BLE count in heartbeat |
| `platformio.ini` | `LV_FONT_MONTSERRAT_12`, `CONFIG_BT_NIMBLE_TASK_STACK_SIZE=8192` |

---

## Lessons Learned

### `std::string` Return-by-Value is a Trap in Embedded C++

NimBLE's `getName()` returns `std::string` by value. Storing `.c_str()` and using it after the statement ends is a use-after-free. This pattern is common in Arduino/ESP32 code and is easy to miss because it "works" most of the time — the freed memory often survives long enough. With high-frequency callbacks (`wantDuplicates=true`), the freed memory gets reused immediately, making the crash deterministic.

### NimBLE's Internal Cache Must Be Managed

`setMaxResults(0xFF)` (default) with continuous scan causes unbounded cache growth. For callback-driven architectures (where you manage your own device list), use `setMaxResults(0)`.

### Auto-Lock Can Masquerade as Crashes

Without serial monitoring, a screen change from auto-lock timeout looks identical to a crash-and-reboot (device shows a different screen). Serial logs with reset reason reporting are essential for distinguishing the two.

### Serial Heartbeat Should Include All Scanner States

The original heartbeat only showed WiFi AP count. Adding BLE device count (`ble=%d`) was critical for confirming the BLE scanner was actually working during debugging.
