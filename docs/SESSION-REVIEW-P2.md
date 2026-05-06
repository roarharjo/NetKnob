# Session Review — NetKnob Phase 2

> Retrospective on the Phase 2 development session.
> Written for reference when planning Phase 3.

---

## What Was Built

Phase 2 transforms the Phase 1 single-screen WiFi scanner into a multi-screen platform:

- **Navigation system** with gesture detection (backspin = menu, shake = emergency stop)
- **Screen lifecycle framework** (ScreenDef with create/show/hide/destroy/update)
- **7 screens**: Main Menu, Group Menu, WiFi Scanner, BLE Scanner, Settings, Safe-Lock, Debug
- **BLE passive scanner** via NimBLE with device classification and manufacturer guessing
- **Settings persistence** via NVS (9 settings)
- **Safe-lock** with 3-digit combination, SHA-256 hashing, escalating lockout
- **Heap monitoring** with periodic serial logging and threshold alerts
- **Encoder event stream** with velocity calculation for gesture detection

52 acceptance criteria defined in the FSD. Core functionality verified on device.

---

## What Went Well

### Subagent-Driven Development

17 tasks executed via fresh subagent per task. Independent tasks dispatched in parallel:
- Tasks 7+8 (Main Menu + Group Menu) — parallel
- Tasks 10+13 (Settings + BLE Scanner) — parallel
- Tasks 11+14+15 (Settings Screen + BLE Screen + Debug Screen) — 3-way parallel

Each subagent received the complete spec for its task, existing interfaces, and no session history. This kept them focused and prevented context pollution.

### Incremental Integration with Test Points

The plan had two flashable milestones:
- **After Task 9**: Navigation working, WiFi scanner accessible via menus
- **After Task 15**: All screens functional

Both milestones were built, flashed, and tested on device. Issues found during testing (lock screen bugs, BLE classification) were fixed immediately rather than accumulated.

### Plan-First Approach Paid Off

The 17-task implementation plan (written before any code) mapped every file, interface, and dependency. This meant subagents could work independently without cross-file confusion. The plan's implementation order (heap monitor → encoder events → gestures → navigation → screens) ensured each task had its dependencies ready.

---

## What Was Challenging

### WiFi Scanner Refactor (Task 6)

Extracting ~550 lines of WiFi rendering from display.cpp into scr_wifi_scan.cpp was the riskiest task. The original code used `lv_screen_active()` as the parent for all LVGL objects; the new architecture needed a dedicated `scr_root` per screen. This required changing how scan view and detail view coexist (both as children of the same root, toggled via HIDDEN flags).

### BLE Device Classification

The initial implementation classified any device with Apple's manufacturer company ID (0x004C) as an Apple Phone. In practice, ~80% of visible BLE devices broadcast Apple's company ID because:
- iBeacon protocol uses 0x004C by design
- Apple Find My network trackers (including 3rd party) use 0x004C
- Apple devices rotate their MAC addresses (RPA), creating multiple entries per physical device

Fix required parsing Apple's manufacturer data subtypes (AirDrop, Nearby, Find My, iBeacon) and adding name-based manufacturer guessing with ~45 patterns. The company_id is now only used as a last resort, and Apple's ID specifically is only trusted when the subtype confirms a genuine Apple protocol.

### Safe-Lock Stop Detection

The initial implementation auto-confirmed digit 0 on lock screen open because stop detection triggered after 500ms of no encoder movement. Fix: added a `has_moved` flag — stop detection only runs after the user has moved the encoder at least once per digit.

Also: the default lock code 0-0-0 is useless since the dial starts at 0 and stop detection would auto-open the lock. Solution: no default code. Enabling lock in settings redirects to the set-code flow.

---

## Architecture Decisions

### Retain-and-Hide vs Destroy-and-Recreate

Phase 1 destroyed and recreated the LVGL screen object on every view switch. Phase 2 retains screen objects in memory and switches between them with `lv_screen_load()`. This uses more SRAM (~55 KB for all retained screens) but eliminates rebuild latency and heap fragmentation.

Exception: Safe-Lock and Debug screens use destroy-on-exit to free memory when not needed.

### Gesture Layer Before Screen Routing

The main loop processes gestures before routing encoder deltas to screens:
1. `gesture_update()` consumes encoder events, detects backspin/shake
2. If gesture detected → navigate (overrides screen-specific handling)
3. If no gesture → pass remaining delta to active screen

This ensures navigation gestures work consistently regardless of which screen is active.

### Separate Encoder Event Stream

The Phase 1 encoder accumulated a simple delta counter. Phase 2 adds a parallel event ring buffer with per-pulse timestamps. Both coexist — `encoder_get_delta()` still works (used nowhere now but preserved), and `encoder_get_events()` provides the event stream for velocity calculation.

---

## Known Issues / Tech Debt

| Issue | Severity | Notes |
|---|---|---|
| BLE callback thread safety | ~~Low~~ **Fixed** | Fixed in bugfix session: FreeRTOS mutex + snapshot rendering pattern. |
| `display_clear()` is dead code | ~~Cosmetic~~ **Fixed** | Removed in bugfix session. |
| Palm cover / stealth mode | Deferred | Skipped — CST816T likely can't distinguish palm from finger. FSD has fallback trigger defined. |
| WiFi scanner crosshair lines | ~~Cosmetic~~ **Fixed** | Removed in bugfix session. |
| BLE scan response names | Known limitation | Devices that only advertise names in scan response (not ADV packet) show as Unknown. Most phones, some watches affected. Devices like LG TV, BYD, Oura Ring that embed names in ADV packets show correctly. |

---

## Memory Profile

```
Phase 1:  RAM 34.9% (114 KB), Flash 30.4% (1.0 MB)
Phase 2:  RAM 37.9% (124 KB), Flash 36.7% (1.2 MB)
Delta:    +10 KB RAM, +213 KB Flash
```

NimBLE accounts for ~10 KB RAM and ~200 KB Flash. All LVGL screens combined use ~55 KB internal SRAM. Comfortable margin for Phase 3.

---

## Lessons for Phase 3

### BLE Manufacturer Data is Unreliable for Device Identification

Don't trust `company_id` from manufacturer-specific AD type as the device's manufacturer. It identifies who designed the AD data format, not who made the device. Apple's iBeacon protocol means any iBeacon shows company_id 0x004C regardless of who made it.

### Test on Device After Each Major Integration

The two-milestone approach (flash after Task 9, flash after Task 15) caught real issues that wouldn't appear in code review: the lock auto-confirming at 0, the BLE Apple flooding, and the display sizing. For Phase 3 (active attacks), flash and test after each attack module is integrated.

### Gesture Thresholds Need Physical Tuning

`BACKSPIN_MIN_VELOCITY` (20 steps/sec) and `SHAKE_WINDOW_MS` (500ms) are starting values from the FSD. They work on the physical device but may need adjustment as more encoder-intensive screens are added.
