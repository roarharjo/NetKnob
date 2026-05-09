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
| 4 | Memory Stability | 1000 TX frames x 3 rounds, measures heap drift |
| 5 | Run All | Chains 1-4, prints summary table |
| R | Results | Reprints the summary table |

## After Validation

Record the results table output. These findings determine Phase 3
architecture decisions (see decision matrix in the design spec).

Delete this entire `spike/` folder once Phase 3 implementation begins.
