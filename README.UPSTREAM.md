<!-- SPDX-License-Identifier: LicenseRef-FNCL-1.1 | Copyright (c) 2026 Cpt_Kirk -->

# README.UPSTREAM.md — Attribution & porting notes / Atrybucja i uwagi o porcie

## 🇬🇧 English

### Origin / Origin

This repository is a **port and extension** of the open-source project:

> **BETTA-HA-PANEL** — Home Assistant & Glances ESP32-S3 dashboard
> Author: **Cpt_Kirk** (GitHub: [cptkirki](https://github.com/cptkirki))
> Repository: https://github.com/cptkirki/BETTA-HA-PANEL

All original design, firmware logic, web-editor concepts, branding, and the
"BETTA" name are the work of **Cpt_Kirk (Chris)**. This variant does not
relicense or re-own any of that work — it remains under the original license.

### License / License

The upstream project and this variant are released under:

> **FNCL-1.1 — Federation Non-Commercial License v1.1**
> `Copyright (c) 2026 Cpt_Kirk`
> Full text: [`LICENSE`](LICENSE)

It is a **non-commercial** license. Any commercial use requires a separate,
written license from the copyright holder. Please respect the original
author's terms.

### What this variant changes / What this variant changes

This repository targets the **Guition JC8012P4A1C-I-W-Y** — a 10.1-inch,
1280×800 **ESP32-P4** panel with a MIPI-DSI **JD9365** display, **GSL3680**
capacitive touch and an ESP32-C6 (ESP-Hosted/SDIO) network co-processor.

Changes relative to upstream (build variant `panel10jc`):

| Area / Area                              | Change / Change                                                                 |
|------------------------------------------|---------------------------------------------------------------------------------|
| Target SoC                               | ESP32-S3 → **ESP32-P4** (10.1" JC8012P4A1C-I-W-Y)                                |
| Display driver                           | Added **JD9365** MIPI-DSI driver (800×1280 rotated 270°)                         |
| Touch driver                             | Added **GSL3680** I2C capacitive-touch bring-up                                   |
| Backlight                                | LEDC PWM on GPIO23                                                               |
| Network                                  | ESP32-C6 ESP-Hosted SDIO co-processor                                           |
| Cameras / Cameras                        | **Removed** (variant ships without camera support)                                |
| Xiaozhi AI / Xiaozhi AI                  | **Removed** (feature compile-disabled; no tokens bundled)                          |
| Entity scale / Entity scale              | `APP_HA_MAX_ENTITIES` raised to **3600**                                         |
| Screensaver / Screensaver                | Graphical wallpaper + clock, brightness slider, dim-after-idle, night mode       |
| Web editor / Web editor                  | Extended: Screen/Screensaver, System tabs; per-tile font/scale options           |
| Diagnostics focus                        | Server/network/Glances-oriented tiles for a home-lab diagnostics panel          |

Everything else — widget concepts, editor UX, HA/Glances integrations — is
kept as close to upstream as possible, per the original project's README and
documentation.

### Upstream resources / Upstream resources

- Upstream README: https://github.com/cptkirki/BETTA-HA-PANEL
- Panels/drivers used by the original author: https://pan.jczn1688.com/

---

## 🇵🇱 Polski

### Pochodzenie

To repozytorium jest **portem i rozszerzeniem** projektu open-source:

> **BETTA-HA-PANEL** — pulpit Home Assistant & Glances na ESP32-S3
> Autor: **Cpt_Kirk** (GitHub: [cptkirki](https://github.com/cptkirki))
> Repozytorium: https://github.com/cptkirki/BETTA-HA-PANEL

Cały oryginalny projekt, logika firmware, koncepcja edytora WWW, branding i
nazwa „BETTA" są dziełem **Cpt_Kirk (Chris)**. Ten wariant nie zmienia
licencji ani nie przejmuje własności tej pracy — pozostaje ona na oryginalnej
licencji.

### Licencja

Projekt źródłowy i ten wariant udostępniane są na licencji:

> **FNCL-1.1 — Federation Non-Commercial License v1.1**
> `Copyright (c) 2026 Cpt_Kirk`
> Pełna treść: [`LICENSE`](LICENSE)

Jest to licencja **niekomercyjna**. Wykorzystanie komercyjne wymaga osobnej,
pisemnej licencji od właściciela praw. Prosimy o poszanowanie warunków
oryginalnego autora.

### Co zmienia ten wariant

To repozytorium celuje w **Guition JC8012P4A1C-I-W-Y** — 10,1-calowy panel
1280×800 na **ESP32-P4**, z wyświetlaczem MIPI-DSI **JD9365**, dotykiem
pojemnościowym **GSL3680** i koprocesorem sieciowym ESP32-C6 (ESP-Hosted/SDIO).

Zmiany względem oryginału (wariant kompilacji `panel10jc`):

| Obszar                                   | Zmiana                                                                           |
|------------------------------------------|----------------------------------------------------------------------------------|
| SoC                                       | ESP32-S3 → **ESP32-P4** (10.1" JC8012P4A1C-I-W-Y)                                 |
| Sterownik wyświetlacza                    | Dodano **JD9365** MIPI-DSI (800×1280 obrócone 270°)                               |
| Sterownik dotyku                          | Dodano obsługę dotyku pojemnościowego **GSL3680** (I2C)                            |
| Podświetlenie                             | PWM LEDC na GPIO23                                                               |
| Sieć                                      | Koprocesor ESP32-C6 ESP-Hosted SDIO                                              |
| Kamery                                    | **Usunięte** (wariant bez kamer)                                                  |
| Xiaozhi AI                                | **Usunięty** (funkcja wyłączona w kompilacji; bez tokenów)                         |
| Skala encji                               | `APP_HA_MAX_ENTITIES` podniesione do **3600**                                     |
| Wygaszacz                                 | Tapeta graficzna + zegar, suwak jasności, przyciemnianie, tryb nocny              |
| Edytor WWW                                | Rozszerzony: zakładki Ekran/Wygaszacz, System; opcje czcionki/skalowania kafelków |
| Fokus diagnostyczny                       | Kafelki serwer/sieć/Glances dla panelu diagnostycznego domowego labu             |

Pozostałe elementy — koncepcja kafelków, UX edytora, integracje HA/Glances —
zostały zachowane jak najbliżej oryginału, zgodnie z README i dokumentacją
projektu źródłowego.

### Materiały źródłowe

- README oryginału: https://github.com/cptkirki/BETTA-HA-PANEL
- Panele/sterowniki używane przez autora: https://pan.jczn1688.com/
