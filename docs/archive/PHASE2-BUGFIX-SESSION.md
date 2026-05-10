---
type: plan
project: NetKnob
phase: 2
status: active
created: 2026-05-06
tags: [bugfix, phase2, netknob, session-plan]
---

# Phase 2 — Bugfix Session Plan

> Instruksjoner for en fokusert bugfix-sesjon.
> Prioritert etter alvorlighet. Jobb deg nedover listen.

---

## Kontekst

Phase 2 er levert og testet pa device. SESSION-REVIEW-P2 og BUGS.md dokumenterer fire apne issues. Denne sesjonen fikser dem, starter med det kritiske.

Kodebasen ligger i netknob-prosjektet (PlatformIO). Relevante filer:

| Fil | Rolle |
|-----|-------|
| `src/ble_scanner.cpp` | NimBLE skanning, callback, device list |
| `src/screens/scr_ble_scan.cpp` | BLE-skjerm rendering |
| `src/screens/scr_wifi_scan.cpp` | WiFi AP-liste og detaljvisning |
| `src/display.cpp` / `display.h` | QSPI-driver, LVGL-oppsett, `display_clear()` |

---

## Bug #2 — BLE race condition (KRITISK)

**Symptom:** Appen krasjer etter en tid pa BLE-scan. Listen blir ustabil, mister data.

**Rotarsak:** NimBLE callback kjorer i BLE-task (FreeRTOS). Main loop leser fra samme `BleScannerState`-struct uten synkronisering. Enkle struct-writes har fungert sa langt, men over tid oppstar race conditions — delvis oppdaterte structs, korrupt data, krasj.

**Bekreftet i SESSION-REVIEW-P2:** "Simple struct writes, no observed crashes in testing. Proper fix: mutex or queue."

### Fiks-strategi

Bruk **mutex** (enklest, minst invasivt). FreeRTOS queue er overkill for denne brukscasen — vi trenger bare eksklusiv tilgang til device-listen, ikke en producer-consumer-pipeline.

### Steg

1. **Opprett en mutex i `ble_scanner.cpp`:**
   ```cpp
   static SemaphoreHandle_t ble_mutex = NULL;

   // I ble_scanner_init():
   ble_mutex = xSemaphoreCreateMutex();
   ```

2. **Beskytt skriving i NimBLE callback:**
   Wrapp all tilgang til `state.devices[]`, `state.device_count`, og relaterte felt:
   ```cpp
   if (xSemaphoreTake(ble_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
       // oppdater device list
       xSemaphoreGive(ble_mutex);
   }
   // Hvis mutex ikke tas innen 5ms: dropp denne advertisementen. Akseptabelt.
   ```

3. **Beskytt lesing i main loop / scr_ble_scan:**
   Alle steder der `state` leses for rendering eller logikk ma ogsa ta mutex:
   ```cpp
   if (xSemaphoreTake(ble_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
       // les device data, kopier det som trengs for rendering
       xSemaphoreGive(ble_mutex);
   }
   ```

4. **Alternativ: Snapshot-monsteret.**
   I stedet for a holde mutex under hele renderingen (som kan blokkere callbacken), kopier device-arrayet til en lokal kopi under kort mutex-hold, og render fra kopien:
   ```cpp
   BleDevice local_copy[BLE_MAX_DEVICES];
   uint8_t local_count;
   if (xSemaphoreTake(ble_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
       memcpy(local_copy, state.devices, sizeof(BleDevice) * state.device_count);
       local_count = state.device_count;
       xSemaphoreGive(ble_mutex);
   }
   // Render fra local_copy — mutex er allerede frigitt
   ```
   Dette er den anbefalte tilnaermingen. Mutex holdes i mikrosekunder, ikke millisekunder.

### Verifisering

- Kjoer BLE-scan i 30+ minutter med mange enheter i naerheten
- Overvak serial for heap-advarsler og crash backtraces
- Sjekk at device-listen forblir konsistent (ingen korrupte navn, ingen RSSI-hopp til 0)

### Fallgruver

- **Ikke bruk `portENTER_CRITICAL` her.** Det deaktiverer interrupts globalt og er ment for ISR-kontekst. NimBLE callback kjorer i en FreeRTOS-task, ikke en ISR. Mutex er riktig primitiv.
- **Timeout pa mutex i callback:** Hold den kort (5ms). Hvis main loop blokkerer mutex under lang rendering, vil callbacken miste advertisements. Snapshot-monsteret forhindrer dette.
- **Ikke glem aging/eviction-logikken.** Denne kjorer ogsa i main loop og muterer `state` — den trenger mutex like mye som renderingen.

---

## Bug #1 — For lite plass til nettverk i WiFi-kanalvisning (MEDIUM)

**Symptom:** Kun 4 AP-er synlig pa skjermen. Pa travle kanaler (typisk kanal 6) er listen uleselig.

**Lokasjon:** `scr_wifi_scan.cpp` — AP-listevisning.

### Fiks-strategi

Kombinasjon av mindre font og encoder-scroll. Ikke velg bare en av dem.

### Steg

1. **Reduser font-storrelse for AP-listen.**
   Ga fra 14px til 11px eller 12px. Test pa device — lesbarheten pa 360x360 rund display er overraskende god selv med sma fonter.

2. **Legg til scroll via encoder.**
   AP-listen sorteres allerede etter RSSI. Legg til en `scroll_offset` (samme monster som BLE-skanneren bruker):
   - Encoder CW/CCW endrer `scroll_offset` nar listen er lengre enn synlige rader
   - Vis en enkel scroll-indikator (f.eks. liten pil opp/ned i kanten) nar det finnes flere AP-er enn synlig

3. **Beregn synlige rader dynamisk.**
   Med 360px hoeyde, header/footer, og 12px font + padding:
   - Tilgjengelig hoyde for liste: ~280px (etter header og status)
   - Rad-hoyde med 12px font + 4px padding: ~16px
   - Synlige rader: ~17 (mer enn nok for de fleste kanaler)
   - Med 14px font: ~14 rader (fortsatt mye bedre enn 4)

4. **Vurder a fjerne RSSI-bar og kun vise dBm-tall.**
   RSSI-baren tar mye horisontal plass. Et tall som "-67" tar langt mindre. Fargekodingen (gron/oransje/rod) kan legges pa selve tallet i stedet. Dette frigjor plass til lengre nettverksnavn.

### Verifisering

- Test pa en kanal med 10+ AP-er (kanal 6 i et kontorbygg er ideelt)
- Bekreft at scroll fungerer uten a kollidere med kanalhopping (encoder-modus-routing ma vaere korrekt)
- Sjekk lesbarheten pa device — ikke bare i kode

### OBS: Encoder-konflikt

WiFi-skanneren bruker allerede encoder for kanalhopping (`ENC_CHANNEL_HOP`). Scroll av AP-listen ma ha en separat interaksjon — f.eks.:
- **Touch tap** bytter mellom kanalhopp-modus og liste-scroll-modus
- Eller: encoder scroller listen, **touch tap** hopper kanal
- Velg det som foeles mest naturlig pa device. Dokumenter valget.

---

## Bug #4 — `display_clear()` dod kode (KOSMETISK)

**Symptom:** Funksjonen er definert i `display.cpp`, deklarert i `display.h`, men kalles av ingenting.

### Fiks

Fjern funksjonen. Slett definisjon fra `display.cpp` og deklarasjon fra `display.h`.

Ikke "repurpose for stealth mode" — stealth mode er uavklart (CST816T kan sannsynligvis ikke skille handflate fra finger, ref. SESSION-REVIEW-P2). Hvis stealth mode trengs senere, skriv en dedikert funksjon med riktig semantikk.

### Verifisering

- Bygg prosjektet. Ingen compiler-feil = ferdig.
- Grep etter `display_clear` i hele kodebasen for a bekrefte at ingenting refererer til den.

---

## Bug #3 — WiFi crosshair-linjer synlig gjennom tekst (KOSMETISK)

**Symptom:** Tynne dekorative linjer fra Phase 1 er tidvis synlige gjennom tekst i WiFi-skanneren.

**Lokasjon:** `scr_wifi_scan.cpp`

### Fiks

Finn crosshair-linjene i renderingskoden. De er sannsynligvis LVGL `lv_line`-objekter eller manuelle `lv_canvas_draw_line`-kall som tegnes for teksten renderes.

To alternativer:
1. **Fjern dem.** Hvis de ikke lenger passer designet, slett dem.
2. **Tegn dem under teksten.** Sett z-order slik at linjene er bak tekst-labels (`lv_obj_move_to_index` eller opprett dem for tekst-objektene).

### Verifisering

- Visuell inspeksjon pa device. Se spesielt pa kanaler med mange AP-er der tekst overlapper linjeomradet.

---

## Arbeidsrekkefolge

| # | Bug | Alvorlighet | Estimert omfang | Avhengigheter |
|---|-----|-------------|-----------------|---------------|
| 1 | #2 BLE race condition | Kritisk | Moderat — mutex + snapshot i 2-3 filer | Ingen |
| 2 | #1 WiFi AP-listeplass | Medium | Moderat — font, scroll, encoder-routing | Ingen |
| 3 | #4 `display_clear()` dod kode | Kosmetisk | Trivielt — slett 2 steder | Ingen |
| 4 | #3 Crosshair-linjer | Kosmetisk | Lite — finn og fjern/reorder | Ingen |

Alle fire er uavhengige. Bug #2 forst fordi den er kritisk og pavirker stabilitet. Bug #1 nest fordi den pavirker brukbarhet. De to kosmetiske kan gjores i hvilken som helst rekkefolge.

---

## Etter bugfix

1. **Flash og test pa device.** Alle fire fiks-er ma verifiseres fysisk, ikke bare i kode.
2. **Kjoer stabilitetstest:** 30 min med BLE + WiFi aktiv, navigering mellom skjermer. Overvak heap-logging pa serial.
3. **Oppdater BUGS.md:** Flytt fiksede bugs til "Lukket"-seksjonen med dato og kort beskrivelse av fiks.
4. **Oppdater SESSION-REVIEW-P2 sin "Known Issues"-tabell** — fjern fiksede issues, merk gjenvaerende.
