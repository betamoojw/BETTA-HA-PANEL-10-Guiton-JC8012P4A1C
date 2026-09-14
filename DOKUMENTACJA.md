<!-- SPDX-License-Identifier: LicenseRef-FNCL-1.1 | Copyright (c) 2026 Cpt_Kirk -->

# DOKUMENTACJA.md — Full documentation / Pełna dokumentacja

Bilingual documentation for **BETTA HA Panel — Guiton 10** (`panel10jc`).
Dokumentacja dwujęzyczna dla **BETTA HA Panel — Guiton 10** (`panel10jc`).

> Upstream: [BETTA-HA-PANEL](https://github.com/cptkirki/BETTA-HA-PANEL) by **Cpt_Kirk**. License: [FNCL-1.1](LICENSE) (non-commercial). / Źródło: [BETTA-HA-PANEL](https://github.com/cptkirki/BETTA-HA-PANEL), autor **Cpt_Kirk**. Licencja: [FNCL-1.1](LICENSE) (niekomercyjna).

---

## 1. Overview / Przegląd

**EN** — The panel is a wall-mounted diagnostic dashboard that connects to a
local **Home Assistant** instance over WebSocket and to a **Glances** server
over HTTP, rendering configurable widgets on a 1280×800 LVGL canvas. All
configuration is done from a built-in web editor, so you never edit YAML or
re-flash to change the layout.

**PL** — Panel to montowany na ścianie pulpit diagnostyczny, który łączy się z
lokalnym **Home Assistant** przez WebSocket i z serwerem **Glances** przez HTTP,
renderując konfigurowalne kafelki na płótnie LVGL 1280×800. Cała konfiguracja
odbywa się z wbudowanego edytora WWW — nie edytujesz YAML ani nie wgrywasz
firmware, aby zmienić układ.

### Key numbers / Kluczowe liczby

| Metric / Wartość                                  | Value / Wartość             |
|---------------------------------------------------|-----------------------------|
| Resolution / Rozdzielczość                        | 1280×800 (LVGL 800×1280 rotated) |
| Pages / Strony                                    | up to / do 10               |
| Home Assistant entities / Encje HA                 | up to / do **3600**         |
| HA API                                             | WebSocket + REST (forecasts, long-poll) |
| Glances endpoint / Punkt Glances                   | HTTP (CPU/RAM/disk/net)     |
| Storage for config / Pamięć konfiguracji            | NVS + optional SD card       |
| Update path / Aktualizacja                          | OTA (file or URL)            |

---

## 2. Architecture / Architektura

```
┌────────────────────────────── ESP32-P4 ──────────────────────────────┐
│  LVGL UI (main/ui)          │  HA client (WebSocket/REST)            │
│  Widget/tile engine         │  Glances client (HTTP JSON)            │
│  Page manager + navigation  │  Settings store (NVS + SD backup)      │
│  Screensaver engine         │  OTA client                           │
├──────────────────────────────────────────────────────────────────────┤
│  Drivers (main/drivers): JD9365 DSI · GSL3680 touch · LEDC backlight │
│  SD card · audio codec · RTC · WiFi                                  │
├──────────────────────────────────────────────────────────────────────┤
│  Network: ESP32-C6 co-processor (ESP-Hosted SDIO)                    │
└──────────────────────────────────────────────────────────────────────┘
                 │
        ┌────────┴─────────┐
        │  BETTA Editor    │  served from panel at http://<ip>/
        │  (web app, WWW)  │  Layout + Settings
        └──────────────────┘
```

**EN** — The application is a single ESP-IDF FreeRTOS task model: LVGL runs the
UI and a background task maintains the Home Assistant WebSocket connection,
dispatching state changes to subscribed widgets. Glances data is polled over
HTTP. The web editor is served by the built-in `httpd` and talks to a REST API
on the panel; all changes apply live via the settings store.

**PL** — Aplikacja działa w modelu zadań ESP-IDF/FreeRTOS: LVGL renderuje UI, a
zadanie w tle utrzymuje połączenie WebSocket z Home Assistant i rozsyła zmiany
stanu do subskrybowanych kafelków. Dane Glances są odpytywane przez HTTP.
Edytor WWW serwowany jest przez wbudowany `httpd` i komunikuje się z API REST
panelu; wszystkie zmiany działają na żywo.

---

## 3. Home Assistant integration / Integracja z Home Assistant

**EN**
- **Connection:** long-lived access token + base URL (`http://homeassistant:8123`).
- **Entities:** the panel imports the full entity registry (up to 3600). Entities
  are grouped by **area/room** in the editor picker for quick assignment.
- **States:** subscribed via WebSocket; REST used for weather forecasts,
  history/long-poll and energy aggregates.
- **Energy dashboard:** derives grid/solar/battery/gas/water from the HA energy
  model automatically.

**PL**
- **Połączenie:** token długoterminowy + bazowy URL (`http://homeassistant:8123`).
- **Encje:** panel importuje pełny rejestr encji (do 3600). W edytorze encje są
  pogrupowane według **strefy/pomieszczenia**.
- **Stany:** subskrybowane przez WebSocket; REST używany do prognoz pogody,
  historii/long-poll i agregatów energii.
- **Panel energii:** automatycznie wylicza sieć/słońce/baterię/gaz/wodę z modelu
  energetycznego HA.

---

## 4. Widget / tile reference / Kafelki — referencja

| Tile / Kafelek             | Data source / Źródło                            | Notes / Uwagi                                                                 |
|----------------------------|-------------------------------------------------|-------------------------------------------------------------------------------|
| Sensor tile                | HA `sensor`                                     | Value + icon + unit; configurable font/scale in editor                          |
| Temperature tile           | HA `sensor` (temperature)                       | **Text** and **Graphic** (gauge/arc) modes; thresholds color the arc            |
| Machine / CPU tile         | Glances                                         | CPU % / RAM / swap / load; power-draw field (W) at the bottom                   |
| Servers tile               | HA or Glances                                   | Per-server CPU/RAM/disk/uptime/status for game/web/Proxmox hosts                |
| Network tile               | HA (`sensor.*`) or router entities              | Separate fields: **LAN**, **WAN**, **TCP**, **UDP**; port lists, IP/WAN header  |
| Glances tile               | Glances (HTTP)                                  | CPU/RAM/disk/net/temp of the main PC                                            |
| Light / Switch / Button    | HA `light`/`switch`/`button`                    | Direct control (LED RGBIC, lamps, outlets)                                      |
| Slider                     | HA `number`/`input_number`                      | Dimmable brightness etc.                                                        |
| Graph                      | HA history                                      | Line / smoothed / bar; 4096-point decimation                                    |
| Gauge / Bars               | HA `sensor`                                     | Horizontal bar or radial gauge                                                  |
| Monitor                    | HA `sensor`                                     | Large readout monitor style                                                     |
| Entity list                | HA `sensor` group                                | List of entity values                                                           |
| Weather                    | HA weather + forecast                           | Current + up to 5-day forecast                                                  |
| Media player               | HA `media_player`                               | Now playing + transport                                                          |
| Energy                     | HA energy model                                 | Grid/solar/battery/gas/water flow                                                |
| Todo                       | HA `todo`                                       | Shopping/task lists                                                             |
| Roborock                   | HA `vacuum`                                     | Robot vacuum status/map                                                          |

**EN** — Every tile has an entity picker with per-field inputs (each tile groups
its entities into separate fields so values never mix), plus font, size, color
and scale options in the editor.

**PL** — Każdy kafelek ma wybór encji z osobnymi polami (każdy kafelek grupuje
swoje encje w osobne pola, więc wartości się nie mieszają) oraz opcje czcionki,
rozmiaru, koloru i skali w edytorze.

---

## 5. Web editor (BETTA Editor) / Edytor WWW

**EN** — Open `http://<panel-ip>/` in a browser. Two main tabs:

- **Layout** — page selector (up to 10 pages), widget palette, drag-and-drop
  canvas, per-widget inspector (entity fields, font, size, colors, scale),
  Quick Setup button that scaffolds a starter dashboard from auto-discovered
  entities.
- **Settings** — device configuration (see below). Changes save to NVS
  immediately and can be exported to the SD card as a backup.

**PL** — Otwórz `http://<ip-panelu>/` w przeglądarce. Dwie główne zakładki:

- **Układ** — wybór stron (do 10), paleta kafelków, płótno z przeciąganiem,
  inspektor kafelka (pola encji, czcionka, rozmiar, kolory, skala), przycisk
  Szybka konfiguracja, który tworzy startowy pulpit z automatycznie wykrytych
  encji.
- **Ustawienia** — konfiguracja urządzenia (poniżej). Zmiany zapisują się od
  razu do NVS i można je wyeksportować na kartę SD jako backup.

---

## 6. Settings reference / Ustawienia

| Tab / Zakładka           | Contents / Zawartość                                                                 |
|--------------------------|--------------------------------------------------------------------------------------|
| Wi-Fi                    | SSID, password, country, BSSID lock, scan                                            |
| Home Assistant           | Base URL + long-lived access token                                                   |
| Xiaozhi AI               | Built in — disabled by default (enable in Settings → Xiaozhi AI + cloud pairing; no tokens) |
| Karta SD (SD card)       | Mount status, storage info, config export/import (backup)                            |
| Czas (Time)              | NTP server, timezone                                                                  |
| Interfejs (Interface)    | Language (EN/DE/ES/FR + custom JSON), UI scale, font scale                           |
| Ekran / Wygaszacz        | Wallpaper upload, clock on/off, screensaver brightness slider, dim-after-idle, night mode |
| System                   | Daily auto-restart hour, volume, reboot, log viewer                                   |
| Motyw (Theme)            | Theme/color selection                                                                 |
| AP konfiguracyjne        | Setup AP (SSID/password)                                                             |
| Aktualizacja (OTA)       | Upload `.ota.bin` or OTA URL                                                          |
| Logi (Logs)              | Live system log / error log                                                           |

### Screensaver / Wygaszacz ekranu

**EN** — The screensaver shows a full-screen **wallpaper** (PNG/JPG uploaded to
the SD card) with an optional **clock**. You can enable/disable the clock,
set the **screensaver brightness** with a slider, and choose the **delay**
after which the screen dims / the screensaver activates. Night mode dims the
panel between configurable hours.

**PL** — Wygaszacz pokazuje pełnoekranową **tapetę** (PNG/JPG wgrane na kartę
SD) z opcjonalnym **zegarem**. Można włączyć/wyłączyć zegar, ustawić **jasność
wygaszacza** suwakiem i wybrać **opóźnienie**, po którym ekran przyciemnia się /
włącza się wygaszacz. Tryb nocny przyciemnia panel w wybranych godzinach.

---

## 7. SD card backup / Backup na karcie SD

**EN**
1. Insert a FAT32 SD card (≤ 32 GB recommended).
2. In **Settings → SD card**, mount the card.
3. Use **Export** to write the full configuration (pages, tiles, entities,
   menu, settings) as a backup copy to the card.
4. Use **Import** to restore a previous backup.
5. Keep backups private — they contain your Wi-Fi password and HA token.

**PL**
1. Włóż kartę SD FAT32 (zalecane ≤ 32 GB).
2. W **Ustawienia → Karta SD** zamontuj kartę.
3. Użyj **Eksportu**, aby zapisać pełną konfigurację (strony, kafelki, encje,
   menu, ustawienia) jako kopię zapasową na karcie.
4. Użyj **Importu**, aby przywrócić wcześniejszy backup.
5. Trzymaj kopie zapasowe prywatnie — zawierają hasło Wi-Fi i token HA.

---

## 8. OTA updates / Aktualizacje OTA

**EN**
- **Via editor:** Settings → OTA → upload an `.ota.bin` from [`release/ota/`](release/ota/) or enter an OTA URL.
- **Via serial:** `idf.py -B build-panel10jc -p COM3 flash` (developer).
- The app is dual-slot aware; a failed OTA rolls back automatically.

**PL**
- **Przez edytor:** Ustawienia → OTA → wgraj `.ota.bin` z [`release/ota/`](release/ota/) lub podaj URL OTA.
- **Przez serial:** `idf.py -B build-panel10jc -p COM3 flash` (developerskie).
- Aplikacja obsługuje podwójny slot; nieudana OTA automatycznie wraca do poprzedniej wersji.

---

## 9. Troubleshooting / Rozwiązywanie problemów

### Boot log warnings — explained / Wyjaśnienie ostrzeżeń z logu startowego

| Log line / Linia logu                                                          | Meaning / Znaczenie                                                                 | Action / Działanie                          |
|--------------------------------------------------------------------------------|-------------------------------------------------------------------------------------|---------------------------------------------|
| `W ldo: The voltage value 0 is out of the recommended range [500, 2700]`       | Informational — an unpopulated voltage LDO channel is read as 0 at boot. Harmless.  | None — ignore / Nic — zignoruj               |
| `W H_SDIO_DRV: Reset slave using GPIO[54]`                                     | Normal ESP-Hosted startup — the ESP32-C6 co-processor is reset over SDIO.           | None — ignore / Nic — zignoruj               |
| `W gsl3680: Unable to initialize the I2C address`                              | Driver can't select the I2C address — missing `driver_data` (0x40) or missing RST/INT GPIOs. Fixed in current source; on old builds touch still works via the reset fallback. | Rebuild + reflash; see [`JAK-URUCHOMIC-DOTYK-GSL3680.md`](JAK-URUCHOMIC-DOTYK-GSL3680.md) |
| `W gsl3680: touch read_data slow: 50 ms (i2c stall?)`                          | One slow I2C read; usually transient.                                               | Check touch responsiveness / Sprawdź reakcję dotyku |
| `E transport: STA TX buffer alloc failed (internal DMA heap exhausted)`        | WiFi TX buffer pressure on the C6 transport under burst traffic.                    | Usually transient; reduce entity poll burst / Zwykle przejściowe |
| `W httpd_uri: URI '/api/cameras' not found`                                    | Editor polling the IP-camera list endpoint on a build from before `APP_FEATURE_CAMERAS=y`. On the current firmware `/api/cameras` responds normally. | Update firmware / Zaktualizuj firmware |
| `W httpd_uri: URI '/app.js.gz' not found`                                      | Browser asked for a gzip-compressed asset the server serves uncompressed.           | None — harmless 404 / Nic — nieszkodliwe 404  |

> **Note / Uwaga:** the current firmware compiles in both the built-in OV02C10
> camera (`APP_FEATURE_LOCAL_CAMERA=y`) and IP cameras (`APP_FEATURE_CAMERAS=y`).
> The built-in camera is exposed under `/api/camera/*` (snapshot, status,
> motion, stream) — see **section 12**. If `/api/cameras` still 404s you are on
> an older build; update the firmware. Xiaozhi **is** compiled in but stays idle
> until it is enabled in **Settings → Xiaozhi AI** and bound to a Xiaozhi cloud
> account.

### Common issues / Typowe problemy

| Symptom / Objaw                                            | Fix / Rozwiązanie                                                                    |
|-------------------------------------------------------------|--------------------------------------------------------------------------------------|
| Panel shows dashes `–` for a tile                          | Verify the entity exists in HA and the field is mapped; the entity may be `unavailable`. |
| „Zapis nie powiódł się … entity_ids must list up to 24 pairs" | Split large entity lists across tiles; each field holds up to 24 `Label=entity` pairs. |
| Touch not responding                                       | See [`JAK-URUCHOMIC-DOTYK-GSL3680.md`](JAK-URUCHOMIC-DOTYK-GSL3680.md) and [`ANALIZA-NAPRAWA-DOTYKU.md`](ANALIZA-NAPRAWA-DOTYKU.md). |
| Screensaver does not activate                             | Check delay + brightness in **Settings → Screen**, and that a wallpaper exists on SD. |
| OTA fails                                                  | Re-flash factory image at offset `0x0`.                                               |

---

## 10. Security & privacy / Bezpieczeństwo i prywatność

**EN** — Wi-Fi credentials, the Home Assistant access token, and any Xiaozhi
tokens are stored **on the device** (NVS/SD) at runtime — they are **not**
hard-coded in this repository. The source was scanned before publication: no
API keys, passwords, tokens or private IPs are shipped. The only IPs present
are the standard setup-AP `192.168.4.1` and public Xiaozhi OTA endpoint.

**PL** — Dane Wi-Fi, token Home Assistant i ewentualne tokeny Xiaozhi są
przechowywane **na urządzeniu** (NVS/SD) w trakcie działania — **nie** są
zaszyte w tym repozytorium. Źródła zostały przeskanowane przed publikacją: nie
ma kluczy API, haseł, tokenów ani prywatnych adresów IP. Jedyne obecne adresy
to standardowe AP konfiguracyjne `192.168.4.1` i publiczny endpoint OTA Xiaozhi.

---

## 11. Build & flash quick reference / Budowanie i wgrywanie — skrót

> ⚠️ **Ważne (Windows):** idf.py na systemie z polską/kodową stroną cp1250
> potrafi się wywalić na znaku `≥` (`UnicodeEncodeError: 'charmap' codec can't
> encode character '\u2265'`). Zawsze wymuszaj UTF-8 przez `PYTHONUTF8=1` /
> `PYTHONIOENCODING=utf-8` przed uruchomieniem idf.py.

**Najprościej — gotowy skrypt** (eksportuje ESP-IDF, wymusza UTF-8, ustawia
target i buduje):

```powershell
pwsh tools/build_panel10jc.ps1
# z czyszczeniem:        pwsh tools/build_panel10jc.ps1 -Clean
# z wgraniem po buildzie: pwsh tools/build_panel10jc.ps1 -Flash -Port COM3
```

**Ręcznie:**

```powershell
# 1) Configure + build (variant panel10jc)
#    UWAGA: sdkconfig.defaults NIE zawiera CONFIG_IDF_TARGET — dla świeżego
#    build-panel10jc trzeba najpierw ustawić target esp32p4.
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
. C:\Espressif\frameworks\esp-idf\export.ps1
$env:PYTHONUTF8='1'; $env:PYTHONIOENCODING='utf-8'
idf.py -B build-panel10jc -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.panel10jc" set-target esp32p4
idf.py -B build-panel10jc build

# 2) Package release images
pwsh tools/make_factory_bin.ps1 -Variant panel10jc

# 3) Flash via serial (COM3)
idf.py -B build-panel10jc -p COM3 flash

# 4) Or flash the factory bin with esptool-js (offset 0x0)
```

See [`README.md`](README.md) and [`release-notes.md`](release-notes.md) for
version history. Full touch bring-up: [`JAK-URUCHOMIC-DOTYK-GSL3680.md`](JAK-URUCHOMIC-DOTYK-GSL3680.md).

---

## 12. Built-in camera / Wbudowana kamera

**EN** — The panel has a built-in **OV02C10** MIPI-CSI camera (front-facing)
used for motion detection (screen wake) and live preview. It is compiled in
(`CONFIG_APP_FEATURE_LOCAL_CAMERA=y`) and configured in **Settings → Camera**.
IP cameras are also compiled in (`CONFIG_APP_FEATURE_CAMERAS=y`).

Endpoints (replace `<ip>` with the panel address):

| Endpoint | Description |
|---|---|
| `GET http://<ip>/api/camera/snapshot` | JPEG snapshot (`image/jpeg`) |
| `GET http://<ip>/api/camera/status` | JSON: `running`, `width`, `height`, `resolution`, `motion_wake`, `motion_threshold`, `jpeg_quality`, `hflip`, `vflip`, `stream_enabled` |
| `GET http://<ip>/api/camera/motion` | Live motion diagnostics: `threshold`, `level`, `changed_pct`, `ignored_lighting`, `active`, `trigger_count`, `last_trigger_ms`, plus per-zone `zones[]` with `x,y,w,h,level,changed_pct` |
| `GET http://<ip>/api/camera/stream` | MJPEG multipart stream (~2 fps). Returns `{"error":"stream_disabled"}` until **Stream** is enabled, `stream_busy` if a client is already streaming |

Settings (stored in the `camera` JSON object):

| Key | Range | Default | Description |
|---|---|---|---|
| `enabled` | bool | off | Camera power on/off |
| `motion_wake` | bool | off | Wake the screen on detected motion |
| `motion_threshold` | 1–64 | 8 | Frame-difference sensitivity (lower = more sensitive) |
| `jpeg_quality` | 10–95 | 55 | Snapshot/stream JPEG quality |
| `hflip` / `vflip` | bool | off | Mirror image horizontally/vertically |
| `stream_enabled` | bool | off | Allow `/api/camera/stream` |
| `resolution` | 0–1 | 0 | 0 = full sensor, 1 = half (downscaled) |
| `motion.min_area` | 0–100 | 0 | Minimum % of frame that must change |
| `motion.min_duration_ms` | 0–1000 | 0 | Motion must last this long to count |
| `motion.cooldown_ms` | 0–30000 | 1000 | Block re-wake for this long |
| `motion.start_delay_ms` | 0–10000 | 2000 | Ignore motion right after camera starts |
| `motion.ignore_lighting` | bool | on | Ignore global brightness changes |
| `motion.zones` | up to 4 | none | Rectangles `{x,y,w,h}` in % of frame (0–100); only motion inside them counts |

**PL** — Panel ma wbudowaną kamerę **OV02C10** (MIPI-CSI, skierowaną na
użytkownika) do detekcji ruchu (budzenie ekranu) i podglądu na żywo. Jest
wkompilowana (`CONFIG_APP_FEATURE_LOCAL_CAMERA=y`) i konfigurowana w
**Ustawienia → Kamera**. Kamery IP również są wkompilowane
(`CONFIG_APP_FEATURE_CAMERAS=y`).

Endpointy (zamiast `<ip>` wpisz adres panelu):

| Endpoint | Opis |
|---|---|
| `GET http://<ip>/api/camera/snapshot` | Zdjęcie JPEG (`image/jpeg`) |
| `GET http://<ip>/api/camera/status` | JSON: `running`, `width`, `height`, `resolution`, `motion_wake`, `motion_threshold`, `jpeg_quality`, `hflip`, `vflip`, `stream_enabled` |
| `GET http://<ip>/api/camera/motion` | Diagnostyka ruchu na żywo: `threshold`, `level`, `changed_pct`, `ignored_lighting`, `active`, `trigger_count`, `last_trigger_ms` oraz `zones[]` z `x,y,w,h,level,changed_pct` |
| `GET http://<ip>/api/camera/stream` | Strumień MJPEG multipart (~2 fps). Zwraca `{"error":"stream_disabled"}` dopóki **Strumień** nie jest włączony, `stream_busy` gdy inny klient już streamuje |

Ustawienia (przechowywane w obiekcie JSON `camera`):

| Klucz | Zakres | Domyślnie | Opis |
|---|---|---|---|
| `enabled` | bool | wył. | Włączanie/wyłączanie kamery |
| `motion_wake` | bool | wył. | Budzenie ekranu po wykryciu ruchu |
| `motion_threshold` | 1–64 | 8 | Czułość różnicy klatek (niżej = bardziej czułe) |
| `jpeg_quality` | 10–95 | 55 | Jakość JPEG zdjęć/strumienia |
| `hflip` / `vflip` | bool | wył. | Odbicie lustrzane w poziomie/pionie |
| `stream_enabled` | bool | wył. | Zezwolenie na `/api/camera/stream` |
| `resolution` | 0–1 | 0 | 0 = pełna matryca, 1 = połowa (zmniejszona) |
| `motion.min_area` | 0–100 | 0 | Minimalny % kadru, który musi się zmienić |
| `motion.min_duration_ms` | 0–1000 | 0 | Ruch musi trwać tyle, by został uznany |
| `motion.cooldown_ms` | 0–30000 | 1000 | Blokada ponownego wybudzenia na ten czas |
| `motion.start_delay_ms` | 0–10000 | 2000 | Ignoruj ruch zaraz po starcie kamery |
| `motion.ignore_lighting` | bool | włącz | Ignoruj globalne zmiany jasności |
| `motion.zones` | do 4 | brak | Prostokąty `{x,y,w,h}` w % kadru (0–100); liczy się ruch tylko w nich |
