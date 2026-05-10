# Session Review — NetKnob Phase 1

> Retrospective on the Phase 1 development session.
> Written for reference when planning Phase 2 and future agent-driven sessions.

---

## What Was Built

A full Phase 1 implementation from a blank PlatformIO project:
- Custom QSPI display driver for ST77916
- LVGL 9.2 integration with PSRAM frame buffer
- Rotary encoder, capacitive touch, and haptic drivers
- Promiscuous-mode WiFi scanner with channel hopping
- AP list view and AP detail view with live RSSI updates
- Neon/cyber UI aesthetic with arc animation and matrix rain splash
- 17/17 acceptance criteria met

---

## What Went Well

### Brainstorm -> Design -> Plan -> Implementation Pipeline

Starting with a structured brainstorming phase before touching code caught ambiguities
early. The phase boundary definition (passive scan only, no attacks) was established
in brainstorming and held through implementation. Without this the scope would have
drifted toward active features.

### Subagent Dispatching

The 9 implementation tasks (display driver, encoder, touch, haptic, WiFi scanner, list
view, detail view, arc animation, splash screen) were dispatched as parallel subagents
where dependencies allowed. This significantly reduced wall-clock time compared to
sequential implementation.

The dependency chain was clear: display driver had to land before any UI work could
start; WiFi scanner had to land before the list view. Everything else was parallel.

### Code Review Catching Critical Bugs Before Device Testing

The code review pass before any device testing found 4 critical bugs:
- Memory leak in the detail view (would crash after ~10 view transitions)
- Dangling pointer on AP list rebuild (would hard-fault on tap after rescan)
- Encoder delta read/clear race condition (would silently drop encoder counts)
- Missing LVGL include (would fail on clean builds)

All 4 would have produced hardware symptoms (freeze, crash, unresponsive encoder)
that are slow to debug on-device. Finding them in review saved multiple flash/test
cycles.

---

## What Was Challenging

### PSRAM Configuration Causing Boot Loops

The three-flag PSRAM config (`psram_type`, `memory_type`, `BOARD_HAS_PSRAM`) was not
documented together anywhere. Each flag is mentioned separately in different Espressif
forum threads. Missing any one produces a silent boot loop with no serial output.

Diagnosing this required the backlight blink test: a minimal sketch that blinks the
backlight GPIO before any PSRAM access, confirming the chip is actually executing code.
Once confirmed-booting, adding flags one at a time found the missing combination.

### LVGL Version Mismatch (9.5 vs 9.2)

Initial scaffolding used `lvgl@^9.0.0`, which resolved to 9.5.x. Several community
examples for this board use 9.2.x API. The arc widget constructor signature changed
between 9.2 and 9.3. Builds that referenced 9.5 examples against a 9.2 API (or vice
versa) produced confusing type errors.

Fix: pin to `~9.2.0` and use only the 9.2 API reference.

### SPI Bus Config Differences

The initial QSPI driver was written from the ESP-IDF SPI master documentation, which
describes the general API but does not give ST77916-specific parameters. The resulting
driver produced a garbled image: pixel-shifted and with swapped colour channels.

The fix required fetching the confirmed-working community reference (`temp_volosr/`)
and diffing the SPI bus configuration line by line. Four parameters differed from the
initial attempt (clock speed, max transfer size, CS handling, command bit width).

### Display Garbling

Even after the PSRAM fix and correct SPI parameters, the display showed content with
swapped blue/green channels. Root cause: `LV_COLOR_16_SWAP=1` was not set. LVGL packs
RGB565 with the high byte last; the SPI peripheral sends high byte first. The build
flag swaps the bytes at the LVGL layer, avoiding a byte-swap in the driver hot path.

This was a one-line fix once identified, but identifying it required comparing output
against known-good screenshots from the reference repo and ruling out hardware damage.

---

## Key Debugging Moments

### Backlight Blink Test

When the device produced no serial output and no display output after the PSRAM
config change, the question was: is the chip executing at all, or is it crashing
before serial initialises?

Flashing a sketch that writes `HIGH` to GPIO 47 (backlight) in `setup()` before
any other code confirmed the chip was executing. This narrowed the failure to
something that happened after the backlight write — specifically, the PSRAM
initialisation that followed.

### Fetching the Working Repo's platformio.ini

The single highest-leverage action in the hardware bring-up was fetching
`temp_volosr/platformio.ini` and diffing it against the project's `platformio.ini`.
Every board-specific parameter — PSRAM type, flash mode, memory type, upload size —
was already correct in the reference. Copying those parameters exactly eliminated the
boot loop and the SPI clock issue in one step.

Lesson: for novel hardware, start with a working reference config and add your
changes on top. Do not start from a generic board template.

---

## Lessons Learned for Future Phases

### Always Reference the Working Repo for Board Config

`temp_volosr/` is the Waveshare-provided reference implementation for this exact board.
Before spending time debugging a hardware issue, check whether the reference has a
working example of that interface. It likely does.

For Phase 2 (active WiFi features), verify the promiscuous + injection mode
configuration against the reference before writing new scanner code.

### Test the Display Driver in Isolation First

In this session the display driver was tested alongside LVGL integration. A display
corruption issue turned out to be partly a driver issue (SPI clock) and partly an
LVGL configuration issue (colour byte swap). Separating them earlier — test raw pixel
writes before wiring up LVGL — would have isolated the root causes faster.

For Phase 2, any new hardware interface (ESP-NOW, secondary chip communication) should
be tested with a minimal loopback sketch before integration.

### LVGL API Differs Between Minor Versions

LVGL follows semver loosely. API changes appear in minor versions (9.2 -> 9.3).
When using LVGL, always:
- Pin to an exact minor version in `lib_deps`
- Cross-reference the LVGL changelog before upgrading
- Check the LVGL migration guide for any widget types the project uses

The arc widget, colour macros, and event callback signatures have all changed across
the 9.x series.

---

## Time Accounting

| Phase | Effort |
|---|---|
| Initial hardware research and planning | 1 session |
| 9-task parallel implementation (subagents) | 1 session |
| Integration and wiring in main.cpp | Included above |
| Code review pass | 1 session |
| Bug fixes (4 critical bugs) | Included in code review session |
| Visual polish (arc animation, neon colours, splash) | 1 session |
| Documentation | 1 session |

---

## Quality Assessment

- 17/17 Phase 1 acceptance criteria met
- 4 critical bugs caught by code review before any device testing
- 0 known regressions introduced during bug fix pass
- All drivers use interrupt-safe patterns where required
- No dynamic allocation outside of LVGL managed heap
- Serial logging at every major init step for future debugging

The codebase is ready for Phase 2 feature work. The recommended first Phase 2 task is
wiring up `interchip.h` and establishing ESP-NOW communication with the secondary
ESP32, as this unblocks BLE and audio features that depend on the secondary chip.
