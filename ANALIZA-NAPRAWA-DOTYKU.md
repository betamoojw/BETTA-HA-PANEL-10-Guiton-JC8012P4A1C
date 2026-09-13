# Panel JC8012P4A1C-I-W-Y (Guiton 10,1") — analiza naprawy: bootloop + dotyk

Data: 2026-09-12
Firmware: `betta-ha-panel-10jc` v0.8.2-10jc
Stan końcowy: **panel się uruchamia, ekran i dotyk działają poprawnie** (kafelki aktywują się dokładnie pod palcem).

---

## 1. Sprzęt i środowisko

| Parametr | Wartość |
|---|---|
| SoC | ESP32-P4, rewizja **v1.3** (efuse v0.3) |
| Krystal | 40 MHz |
| Flash | **16 MB** (SPI DIO, 80 MHz) |
| PSRAM | 32 MB (hex PSRAM, 200 MHz, X16) |
| Ekran | **JD9365** MIPI-DSI 2-lane, natywnie 800×1280 (portret), RGB565 |
| Dotyk | **GSL3680** — I2C addr 0x40, SCL=GPIO8, SDA=GPIO7, 400 kHz, RST=GPIO22, INT=GPIO21 |
| MAC panelu | `80:f1:b2:d3:5c:e9` |
| Port programowania | COM3 (CH340), 115200 8N1 (konsola) |
| ESP-IDF | v5.5.5-dirty (`C:\Espressif\frameworks\esp-idf`) |
| Toolchain | riscv32-esp-elf esp-14.2.0_20260121 |
| Python env | `C:\Users\kruse\.espressif\python_env\idf5.5_py3.11_env` |
| esptool | v4.12.0 |
| Katalog projektu | `C:\Projects\OfflineWorkspace\PROJEKTY\PROJEKTY_W_BUDOWIE\Guiton 10` |
| Katalog build | `build-panel10jc` (preset `panel10jc`) |

Konfiguracja pamięci flash (z logu boot):
```
nvs        0x00009000  0x6000
otadata    0x0000f000  0x2000
phy_init   0x00011000  0x1000
factory    0x00020000  0x600000   <- tu leci firmware
ota_0      0x00620000  0x600000
storage    0x00c20000  0x3e0000
```

---

## 2. Były DWA niezależne błędy

### Błąd #1 — bootloop (Store access fault) — przepełnienie bufora GSL3680

**Objaw:** panel się bootował, a przy pierwszym odczycie dotyku resetował się
(`Store access fault`, "bootloop").

**Przyczyna:** w `components/esp_lcd_touch_gsl3680/esp_lcd_touch_gsl3680.c`
funkcja `esp_lcd_touch_gsl3680_read_data()` deklarowała bufor:

```c
uint8_t touch_data[24];   // ZA MAŁY
...
touch_gsl3680_i2c_read(tp, ESP_LCD_TOUCH_GSL3680_READ_XY_REG, touch_data, 44);
```

I2C zapisywał **44 bajty** do bufora **24 bajty**. Nadmiarowe 20 bajtów
nadpisywało zapisane na stosie rejestry `s2`/`s3`. Gdy nie było dotyku,
dane były zerami → odtworzony `s2 = 0` → NULL jako wskaźnik `data`
w `lvgl_port_touchpad_read` → `sw zero,48(s2)` = zapis pod adres 0x30 =
**Store access fault**.

**Naprawa (plik `components/esp_lcd_touch_gsl3680/esp_lcd_touch_gsl3680.c`):**
- linia 310: `uint8_t touch_data[44];` (dopasowany do długości odczytu I2C),
- linie 325–328 i 361–362: przycięcie `Finger_num` do `MAX_FINGER_NUM` (3),
  żeby pętle parsujące nie wyjechały poza bufor.

Weryfikacja w deasemblerze (riscv32-esp-elf-objdump): funkcja
`esp_lcd_touch_gsl3680_read_data` ma ramkę stosu 208 B, bufor `touch_data`
na `sp+132`, odczyt `li a3,44`, klamra `Finger_num <= 3` obecna.
Bootlog potwierdza: `gsl3680 startup success` i brak dalszych resetów.

### Błąd #2 — dotyk odwrócony o 180° ("do góry nogami")

**Objaw:** po naprawie #1 dotyk już działał, ale klik lądował w innym miejscu
— panel „czytał dotyk do góry nogami / po przekątnej".

**Przyczyna:** złe lustra osi w konfiguracji dotyku.

**Naprawa (plik `main/drivers/touch_init_panel10jc.c`):**

| Flaga | Było | Jest |
|---|---|---|
| `swap_xy` | 0 | 0 (bez zmian) |
| `mirror_x` | **0** | **1** |
| `mirror_y` | **1** | **0** |

Zmienione w dwóch miejscach: `tp_cfg.flags` (linie 116–118) oraz
`touch_apply_display_rotation_alignment()` (linie 143–145), bo dla GSL3680
funkcje `esp_lcd_touch_set_*` tylko aktualizują flagi (driver nie ma HW
mirror — mirror robiony jest programowo).

---

## 3. Pełna analiza łańcucha transformacji dotyku

Dane płyną tak:

```
RAW GSL3680 (x_r, y_r)        x_r ∈ [0,800), y_r ∈ [0,1280)  (portret natywny)
   │  mirror/swap programowy (esp_lcd_touch_get_data)
   ▼
PO MIRROR (x_p, y_p)
   │  LVGL lv_display_rotate_point()  z LV_DISPLAY_ROTATION_270
   ▼
LOGICZNE (Lx, Ly)  →  trafia do LVGL (krajobraz 1280×800)
```

Wzory (dokładne, z kodu źródłowego):

- mirror: `x_p = 800 - x_r` (gdy mirror_x), `y_p = 1280 - y_r` (gdy mirror_y)
- swap: zamiana `x_p ↔ y_p`
- LVGL 270° (`lv_display_rotate_point`, `disp->hor_res = 800`):
  ```
  Lx = y_p
  Ly = 800 - x_p - 1
  ```

### Konfiguracja DOCELOWA (swap=0, mirror_x=1, mirror_y=0):

```
x_p = 800 - x_r
y_p = y_r
Lx = y_r
Ly = 800 - (800 - x_r) - 1 = x_r - 1
```
Czyli czyste: `Lx = y_r`, `Ly = x_r - 1` — osie trafiają w miejsca, zero
niespodzianek (przesunięcie o 1 px to standardowy offset LVGL `-1`).

### Stara konfiguracja (swap=0, mirror_x=0, mirror_y=1):

```
x_p = x_r
y_p = 1280 - y_r
Lx = 1280 - y_r
Ly = 799 - x_r
```

Porównanie starej i nowej: `Lx_old = 1280 - Lx_new`, `Ly_old ≈ 799 - Ly_new`
— czyli stara mapa to **obrót o 180°** nowej. Stąd objaw „do góry nogami".

### Tabela wszystkich 8 kombinacji (dla 270°):

| swap | mx | my | Lx =            | Ly =            | poprawna dla 10"? |
|---|---|---|-----------------|-----------------|---|
| 0 | 0 | 0 | y_r             | 799 − x_r       | nie |
| 0 | 0 | 1 | 1280 − y_r      | 799 − x_r       | nie (było — odwrócone 180°) |
| **0** | **1** | **0** | **y_r**         | **x_r − 1**     | **TAK (obecna)** |
| 0 | 1 | 1 | 1280 − y_r      | x_r − 1         | nie |
| 1 | 0 | 0 | x_r             | 799 − y_r       | nie |
| 1 | 0 | 1 | 800 − x_r       | 799 − y_r       | nie |
| 1 | 1 | 0 | x_r             | y_r − 481       | NIE — poza zakresem |
| 1 | 1 | 1 | 800 − x_r       | y_r − 481       | NIE — poza zakresem |

Kombinacje `swap=1, mirror_x=1` są matematycznie nieprawidłowe (mapują długą
oś na pion logiczny i wychodzą poza zakres Ly).

### Dlaczego NIE zmieniamy rotacji ekranu

Ekran ma `sw_rotate = true`, więc konfiguracyjne `.rotation {mirror_x, mirror_y}`
w [display_init_panel10jc.c](main/drivers/display_init_panel10jc.c) (linie 526–528)
**nie jest** wysyłane do sprzętu (`lvgl_port_disp_rotation_update` zwraca wcześniej).
Treść ekranu obraca programowo `lv_draw_sw_rotate(..., LV_DISPLAY_ROTATION_270, ...)`
w callbacku flush, a `lv_display_set_rotation(LV_DISPLAY_ROTATION_270)` (linia 574)
obraca punkty dotyku tą samą konwencją LVGL. Dlatego wystarczyło ustawić poprawne
lustra dotyku — oś dotyku i oś ekranu są ze sobą spójne.

---

## 4. Polecenia budowania i wgrywania

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
. C:\Espressif\frameworks\esp-idf\export.ps1
Set-Location 'C:\Projects\OfflineWorkspace\PROJEKTY\PROJEKTY_W_BUDOWIE\Guiton 10'

# kompilacja (preset panel10jc: JD9365 + GSL3680, 800x1280)
idf.py -B build-panel10jc build

# wgranie na panel (COM3)
idf.py -B build-panel10jc -p COM3 flash
```

Kompilacja kończy się czysto (tylko znane, wcześniej istniejące ostrzeżenia).

---

## 5. Dane firmware i flasha

| Element | Adres | Rozmiar |
|---|---|---|
| bootloader | 0x2000 | 22 992 B (0x59d0) |
| partition-table | 0x8000 | 3 072 B |
| ota_data_initial | 0xf000 | 8 192 B |
| aplikacja (app) | 0x20000 | **4 902 208 B** (0x4acd40) |

- Najmniejsza partycja app: 0x600000 (6 MB) → **22% wolne**
- flash: 16 MB, mode DIO, freq 80 MHz, baud 460800
- chip: ESP32-P4 rev v1.3, MAC `80:f1:b2:d3:5c:e9`

SHA256 zbudowanego firmware:
```
44DE70BD0B3C422A889202289EC39A066C763E0F7C93501F80C18F6D2D8386E1
```

Kopie roboczego firmware:
- `release\betta-ha-panel-10jc.bin` (nadpisany wersją działającą)
- `release\archive\betta-ha-panel-10jc-2026-09-12-dotyk-fix.bin` (kopia z datą)

---

## 6. Dowody z logu boot (skrót)

```
boot: chip revision: v1.3
boot: SPI Flash Size : 16MB
esp_psram: Found 32MB PSRAM device
app_init: Project name:     betta-ha-panel-10jc
app_init: App version:      v0.8.2-10jc
jd9365: LCD ID: 93 65 04
display: Display initialized (JD9365 + DSI 2ch, 800x1280@270deg, ...)
gsl3680: init gls3680
gsl3680: read reg 0xf0 after is 12 34 56 0
gsl3680: load fw: all 4356 writes ACKed
gsl3680: gsl3680 startup success
touch: Touch rotation aligned (swap=0, mirror_x=1, mirror_y=0)   <- po naprawie
touch: Touch polling tuned: mode=timer, period=10 ms
touch: Touch initialized (esp_lvgl_port + GSL3680)
```

---

## 7. Pliki zmienione

1. `components/esp_lcd_touch_gsl3680/esp_lcd_touch_gsl3680.c`
   - `touch_data[24]` → `touch_data[44]` (naprawa przepełnienia stosu)
   - klamra `Finger_num <= MAX_FINGER_NUM` (3)

2. `main/drivers/touch_init_panel10jc.c`
   - `mirror_x`: 0 → **1**, `mirror_y`: 1 → **0** (naprawa odwrócenia 180°)

3. Dokumentacja: `ANALIZA-NAPRAWA-DOTYKU.md` (ten plik)

---

## 8. Martwa strefa dotyku (poziomy pas) — WADA SPRZĘTOWA

### Objaw
Na ekranie jest **poziomy pas martwy** w okolicy **LVGL y ≈ 253–371** (w oknie
ustawień objawiał się jako niedziałający kafelek „SD”). Dotknięcia w tym pasie
w ogóle nie są zgłaszane — brak jakiejkolwiek reakcji.

### Dowód, że to sprzęt, a nie oprogramowanie
1. **IC dotyku sam zgłasza 0 palców** w tym pasie. Driver loguje surowe dane
   I2C przy każdym nowym naciśnięciu (`RAWpre press`). W logu z ~22 naciśnięciami
   wartości Y przeskakują z `~284` do `~418` — pomiędzy nimi **nic**, czyli
   rejestr `Finger_num` (bajt 0 bloku 44-bajtowego) jest tam zerem.
2. **Konfiguracja nie ignoruje żadnego obszaru.** W `gsl_config_data_id[]`:
   - `ignore_y = conf[0x25] = 0`
   - `ignore_x = conf[0x26] = 0`
   - `edge_cut = conf[0x27] = 0x04040404` (tylko 4 jednostki na krawędziach)
   
   Zatem hostowy algorytm (`ScreenResolution`/`PointIgnore`) nie ma żadnej
   podstawy, żeby odrzucić punkt w środku ekranu.
3. **Na granicach pasa IC ustawia flagę „able” (0x40)** w starszym bajcie Y.
   Oznacza to, że na krawędziach martwego pasa IC wykrywa słaby sygnał i sam
   go oznacza jako niewiarygodny, a w środku pasa nie wykrywa nic.

### Interpretacja
Pas ≈ 2 martwe linie sensora z 14 (`sen_num=14`). Fizycznie to przerwane
ścieżki/taśma flex w środku czujnika dotyku. **Tego nie da się naprawić w
firmware** — host nie wyczaruje dotyku tam, gdzie IC nie widzi pojemności.

### Jedyne rozwiązanie programowe (zastosowane)
Przesunięcie wszystkich elementów interaktywnych POZA martwy pas:
- `main/ui/ui_settings.c` — kafelki kategorii ustawień przeliczone na
  deterministyczny układ **2 kolumny**, więc „SD” leży przy y ≈ 190–234
  (poza pasem 253–371). Wcześniej układ flex ROW_WRAP renderował je w 1
  kolumnie i „SD” trafiał w martwy pas (y 290–334).

### Zalecenie
Jeśli panel jest na gwarancji — **wymiana** (wada fabryczna czujnika dotyku).

---

## 9. Karta SD — sterownik DZIAŁA (UI pokazywało błąd)

Sterownik SD (`main/drivers/sdcard_init_panel10jc.c`) jest w pełni sprawny:
- boot: `sdcard: microSD mounted at /sdcard (name=SH64G, capacity=63864569856 bytes)`
- REST: `GET /api/sd/status` → `{"mounted":true,"logging_enabled":true,"total_bytes":63847858176,"free_bytes":63841828864}`

Jedynym problemem był **nieaktualny tekst** w ekranie ustawień
(`st_build_sd()`), który twierdził „Ten firmware nie posiada sterownika karty
SD”. Naprawiono: `main/ui/ui_settings.c` — `st_build_sd()` pokazuje teraz
realny stan: „Zamontowana”, pojemność i wolne miejsce (przez `sdcard_is_ready()`
i `sdcard_info()`), a gdy brak karty — komunikat o automatycznym montowaniu.
