# JAK URuchomić DOTYK GSL3680 na panelu JC8012P4A1C-I-W-Y (Guiton 10,1")

Kompletny poradnik krok po kroku: co musi być zrobione, jakie ustawienia,
jakie drivery, żeby dotyk działał na tym panelu. Opisano stan **działający**
(z dnia 2026-09-12).

Panel: **ESP32-P4 v1.3** + ekran **JD9365** (MIPI-DSI 2-lane, natywnie
800×1280 portret) + dotyk **GSL3680** (I2C). Firmware `betta-ha-panel-10jc`
v0.8.2-10jc, ESP-IDF v5.5.5.

---

## Spis treści
1. [Wymagane elementy](#1-wymagane-elementy)
2. [Driver GSL3680 — co zawiera i kluczowa poprawka](#2-driver-gsl3680)
3. [Plik inicjalizujący dotyk — pełna konfiguracja](#3-plik-inicjalizujący-dotyk)
4. [Jak działa cały łańcuch (driver → LVGL)](#4-jak-działa-cały-łańcuch)
5. [Ustawienie osi: mirror/swap — najważniejsza decyzja](#5-ustawienie-osi)
6. [Powiązanie z ekranem (rotacja)](#6-powiązanie-z-ekranem)
7. [Budowanie i wgrywanie](#7-budowanie-i-wgrywanie)
8. [Lista kontrolna — jak sprawdzić, że działa](#8-lista-kontrolna)
9. [Rozwiązywanie problemów](#9-rozwiązywanie-problemów)

---

## 1. Wymagane elementy

Żeby dotyk działał, muszą istnieć i być poprawne **cztery** rzeczy:

| # | Element | Lokalizacja | Rola |
|---|---|---|---|
| 1 | Driver GSL3680 | `components/esp_lcd_touch_gsl3680/` | niskopoziomowa obsługa I2C + firmware kontrolera |
| 2 | Inicjalizacja dotyku | `main/drivers/touch_init_panel10jc.c` | I2C bus, GPIO, konfiguracja osi, podpięcie do LVGL |
| 3 | Port dotyku LVGL | `managed_components/espressif__esp_lvgl_port` (komponent `esp_lvgl_port`) | mostek driver→LVGL (`lvgl_port_add_touch`) |
| 4 | Powiązanie z ekranem | `main/drivers/display_init_panel10jc.c` | ekran musi być ustawiony w ten sam układ co dotyk |

Ten panel **nie ma gotowego BSP** (w przeciwieństwie do wariantów Waveshare),
więc I2C i uchwyt dotyku tworzymy ręcznie w `touch_init_panel10jc.c`, a źródło
drivery `esp_lcd_touch_gsl3680` pochodzi z oficjalnej referencji Guition.

---

## 2. Driver GSL3680

### 2.1 Struktura komponentu

```
components/esp_lcd_touch_gsl3680/
├── esp_lcd_touch_gsl3680.c        # implementacja
└── include/esp_lcd_touch_gsl3680.h# API + makra
```

### 2.2 Kluczowe stałe z nagłówka

```c
#define MAX_FINGER_NUM      3                     // max 3 punkty dotyku
#define ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS (0x40)   // adres I2C

// makro konfiguracji IO (używane w touch_init):
#define ESP_LCD_TOUCH_IO_I2C_GSL3680_CONFIG()           \
    {                                                   \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS, \
        .control_phase_bytes = 1,                       \
        .dc_bit_offset = 0,                             \
        .lcd_cmd_bits = 8,                              \
        .flags = { .disable_control_phase = 1 }         \
    }
```

### 2.3 Sekwencja startowa kontrolera

`esp_lcd_touch_gsl3680_init()` robi (w tej kolejności):
1. `esp_lcd_touch_gsl3680_clear_reg()` — wyczyszczenie rejestrów
2. `touch_gsl3680_reset()` — reset GPIO (RST=0 → 20 ms → RST=1), zapis do `0xe4` i `0xbc`
3. `esp_lcd_touch_gsl3680_load_fw()` — wgranie tabeli firmware `GSLX680_FW[]` przez I2C
4. `esp_lcd_touch_gsl3680_startup_chip()` — zapis `0xe0 = 0x00` + `gsl_DataInit`
5. `touch_gsl3680_reset()` + ponownie `startup_chip()`

Poprawny start potwierdza `esp_lcd_touch_gsl3680_read_ram_fw()`: czyta rejestr
`0xb0` i czeka aż zwróci `0x5a 0x5a 0x5a 0x5a` (max 10 prób × 50 ms).
W logu widać wtedy:

```
gsl3680: read 0xb0 = 5a,5a,5a,5a (retry 1/10)
gsl3680: gsl3680 startup success
```

### 2.4 Odczytywanie pozycji (rejestr 0x80)

Funkcja `esp_lcd_touch_gsl3680_read_data()` czyta **44 bajty** z rejestru `0x80`:

| Indeks | Znaczenie |
|---|---|
| `[0]` | liczba palców (`Finger_num`) |
| `[1]`, `[2]` | punkt 0: Y (lo, hi) |
| `[3]`, `[4]` | punkt 0: X (lo, hi w 4 bitach) + ID w górnych 4 bitach |
| `[(j+1)*4+0 .. +3]` | kolejne punkty j |

Wyciąganie współrzędnych:
```c
x  = (touch_data[(j+1)*4+3] & 0x0f) << 8 | touch_data[(j+1)*4+2];
y  =  touch_data[(j+1)*4+1] << 8        | touch_data[(j+1)*4+0];
id = (touch_data[(j+1)*4+3] & 0xf0) >> 4;
```

### 2.5 ⚠️ KRYTYCZNA POPRAWKA #1 — przepełnienie bufora (był bootloop)

**Problem:** oryginalny driver miał:

```c
uint8_t touch_data[24];   // ZA MAŁY !!!
...
touch_gsl3680_i2c_read(tp, 0x80, touch_data, 44);   // zapisuje 44 bajty
```

20 nadmiarowych bajtów nadpisywało rejestry `s2`/`s3` zapisane na stosie.
Bez dotyku dane = zera → `s2` odtworzony jako 0 → NULL jako wskaźnik w
`lvgl_port_touchpad_read` → `Store access fault` → **bootloop przy pierwszym
dotknięciu / starcie dotyku**.

**Poprawka (obowiązkowa):**

```c
uint8_t touch_data[44];   // dopasowane do długości odczytu
```

Dodatkowo — klamra na liczbę palców (bo `touch_data[0]` z zaszumionego
odczytu mógł dać >3 i wyjechać poza `XY_Coordinate[3]`):

```c
Finger_num = touch_data[0];
if (Finger_num > MAX_FINGER_NUM) {
    Finger_num = MAX_FINGER_NUM;   // 3
}
```

Tę samą klamrę powtórzono po `gsl_alg_id_main()` (druga kopia `Finger_num`).
Dokładne linie (w tej kopii repozytorium): `esp_lcd_touch_gsl3680.c` — bufor
linia 317, odczyt 44 B linia 346, klamry linie 348 i 417.

### 2.6 `get_xy` — mostek do portu LVGL

```c
static bool esp_lcd_touch_gsl3680_get_xy(..., uint16_t *x, uint16_t *y, ...)
{
    portENTER_CRITICAL(&tp->data.lock);
    if(max_point_num > Finger_num) *point_num = Finger_num;
    else                           *point_num = max_point_num;
    for(int i=0;i<*point_num;i++){
        x[i] = XY_Coordinate[i].x_position;
        y[i] = XY_Coordinate[i].y_position;
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return (*point_num > 0);   // true = jest dotyk
}
```

Ważne: driver **nie implementuje** `set_mirror_x/set_mirror_y/set_swap_xy`,
więc lustra/swap są robione **programowo** przez `esp_lcd_touch_get_data()`
(komponent `esp_lcd_touch`) na podstawie flag w konfiguracji.

---

## 3. Plik inicjalizujący dotyk

Plik: `main/drivers/touch_init_panel10jc.c`

### 3.1 Piny i parametry

```c
#define JC_TOUCH_I2C_PORT    I2C_NUM_1
#define JC_TOUCH_I2C_SCL     GPIO_NUM_8
#define JC_TOUCH_I2C_SDA     GPIO_NUM_7
#define JC_TOUCH_I2C_CLK_HZ  400000          // 400 kHz
#define JC_TOUCH_RST_GPIO    GPIO_NUM_22     // reset (aktywny niski)
#define JC_TOUCH_INT_GPIO    GPIO_NUM_21     // interrupt (aktywny niski)
#define JC_TOUCH_X_MAX       800             // oś krótka (portret)
#define JC_TOUCH_Y_MAX       1280            // oś długa (portret)
```

### 3.2 I2C bus

```c
const i2c_master_bus_config_t i2c_cfg = {
    .i2c_port = I2C_NUM_1,
    .sda_io_num = GPIO_NUM_7,
    .scl_io_num = GPIO_NUM_8,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = true,    // ← KLUCZOWE (patrz niżej)
};
i2c_new_master_bus(&i2c_cfg, &s_i2c_bus);
```

Uwaga: **I2C_NUM_1 jest współdzielony** z kodekiem audio ES8311 —
funkcja `jc8012_i2c_bus_get()` udostępnia ten sam uchwyt do `board_extras`.

⚠️ **Wewnętrzne pull-upy I2C muszą być włączone** (`enable_internal_pullup =
true`). Szyna I2C dotyku na tej płycie **opiera się na wewnętrznych
pull-upach ESP32-P4** — bez nich odczyty stają się marginalne i sporadycznie
skorumpowane (objaw: surowe próbki z absurdalnymi wartościami, np.
`raw sense = 17226`). Wcześniejszy wpis w tym poradniku (`= false`) był
błędny — kod produkcyjny ma `= true`.

### 3.3 Tworzenie uchwytu dotyku

```c
// Selektor adresu I2C — bez tego driver loguje
// "Unable to initialize the I2C address" (patrz 3.3.1).
static const esp_lcd_touch_io_gsl3680_config_t s_gsl3680_io_cfg = {
    .dev_addr = ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS,   // 0x40
};

const esp_lcd_touch_config_t tp_cfg = {
    .x_max = 800,
    .y_max = 1280,
    .rst_gpio_num = GPIO_NUM_22,
    .int_gpio_num = GPIO_NUM_21,
    .driver_data = &s_gsl3680_io_cfg,   // ← KLUCZOWE (wybór adresu I2C)
    .levels = { .reset = 0, .interrupt = 0 },
    .flags = {
        .swap_xy  = 0,
        .mirror_x = 1,    // ← KLUCZOWE (było 0)
        .mirror_y = 0,    // ← KLUCZOWE (było 1)
    },
};

esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GSL3680_CONFIG();
tp_io_config.scl_speed_hz = 400000;
esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_config, &tp_io_handle);
esp_lcd_touch_new_i2c_gsl3680(tp_io_handle, &tp_cfg, &s_touch_handle);
```

### 3.3.1 ⚠️ KRYTYCZNA POPRAWKA #3 — wybór adresu I2C

Driver GSL3680 wybiera adres I2C kontrolera (0x40) **pinem INT/A0**: w trakcie
resetu wystawia INT=0, podnosi RST i dopiero wtedy rozmawia po I2C. Żeby to
zrobić, musi mieć **wszystkie trzy** rzeczy naraz:

1. `rst_gpio_num` ≠ `GPIO_NUM_NC` (tu **GPIO22**),
2. `int_gpio_num` ≠ `GPIO_NUM_NC` (tu **GPIO21**),
3. `driver_data` wskazujący na `esp_lcd_touch_io_gsl3680_config_t` z
   `dev_addr = ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS` (**0x40**).

Gdy którejkolwiek brakuje, driver wpada w gałąź awaryjną i loguje:

```
W gsl3680: Unable to initialize the I2C address
```

a potem robi tylko zwykły reset bez wyboru adresu. Dotyk najczęściej i tak
wystartuje (po resecie GSL3680 domyślnie odpowiada pod 0x40), dlatego
ostrzeżenie łatwo zignorować — ale to objaw **brakującej konfiguracji adresu**.
Poprawka: `.driver_data = &s_gsl3680_io_cfg` + ustawione RST i INT. Po poprawce
ostrzeżenie znika — wymaga **ponownego zbudowania i wgrania firmware**.

### 3.4 Wymuszenie ustawienia osi (po utworzeniu)

Funkcja `touch_apply_display_rotation_alignment()` nadpisuje flagi jawnie
(ważne, bo driver GSL3680 nie ma HW mirror — settery tylko aktualizują flagi):

```c
const bool swap_xy  = false;
const bool mirror_x = true;    // ← po naprawie
const bool mirror_y = false;   // ← po naprawie

esp_lcd_touch_set_swap_xy (handle, swap_xy);
esp_lcd_touch_set_mirror_x(handle, mirror_x);
esp_lcd_touch_set_mirror_y(handle, mirror_y);
```

### 3.5 Podpięcie do LVGL

```c
const lvgl_port_touch_cfg_t touch_cfg = {
    .disp   = lv_display_get_default(),   // ten sam ekran co rotacja 270°
    .handle = s_touch_handle,
    .scale  = { .x = 1.0f, .y = 1.0f },   // 1:1, bez skalowania
};
s_touch_indev = lvgl_port_add_touch(&touch_cfg);
```

### 3.6 Polling — tryb TIMER (nie EVENT)

```c
lv_indev_set_mode(indev, LV_INDEV_MODE_TIMER);      // polling timerem
lv_timer_t *t = lv_indev_get_read_timer(indev);
lv_timer_set_period(t, 10);                          // co 10 ms
lv_timer_ready(t);
```

⚠️ **Dlaczego TIMER, a nie EVENT (przerwanie INT na GPIO21):** tryb EVENT
(sterowany przerwaniem) był testowany, żeby zmniejszyć opóźnienie dotyku, ale
na tej płycie **linia INT GSL3680 nie wyzwala się niezawodnie**. W trybie EVENT
LVGL trzyma timer odczytu wstrzymany i czeka na przerwanie — skoro INT nie
przychodzi, LVGL **nigdy nie odczytuje dotyku** i ekran jest kompletnie martwy.
TIMER + 10 ms to konfiguracja sprawdzona i stabilna (identyczna jak w panelach
Waveshare z tego samego portu).

### 3.7 Retry init

`touch_init()` próbuje `jc_touch_new()` do **8 razy** z odstępem **250 ms**
(kontroler czasem nie jest gotowy od razu po starcie PSRAM/SDIO).

---

## 4. Jak działa cały łańcuch (driver → LVGL)

```
GSL3680 (I2C 0x80, 44 B)
   │  esp_lcd_touch_gsl3680_read_data()   → XY_Coordinate[] (x_r, y_r)
   ▼
esp_lcd_touch_get_data()                 → mirror/swap programowy:
   │                                          x = 800 - x_r  (mirror_x=1)
   │                                          y = y_r        (mirror_y=0)
   ▼
lvgl_port_touchpad_read()                → data->point = (x_p, y_p)
   ▼
LVGL lv_display_rotate_point(270°)       → Lx = y_p ; Ly = 800 - x_p - 1
   ▼
LVGL (krajobraz 1280×800) — trafia w kafelek
```

Końcowy wzór przy obecnych ustawieniach:

```
Lx = y_r
Ly = x_r - 1
```

---

## 5. Ustawienie osi

Najważniejsza decyzja: **które osie odbić i czy zamieniać X/Y**.

Prawidłowa kombinacja dla tego panelu (montaż w poziomie, rotacja 270°):

```
swap_xy  = 0   (nie zamieniamy osi)
mirror_x = 1   (odbijamy X)
mirror_y = 0   (nie odbijamy Y)
```

Tabela wszystkich 8 kombinacji i ich wynik (przy rotacji 270°):

| swap | mx | my | Lx         | Ly         | uwagi |
|---|---|---|------------|------------|-------|
| 0 | 0 | 0 | y_r        | 799 − x_r  | |
| 0 | 0 | 1 | 1280 − y_r | 799 − x_r  | **stare ustawienie — dotyk odwrócony 180°** |
| **0** | **1** | **0** | **y_r**    | **x_r − 1** | **OBECNE — POPRAWNE** |
| 0 | 1 | 1 | 1280 − y_r | x_r − 1    | |
| 1 | 0 | 0 | x_r        | 799 − y_r  | |
| 1 | 0 | 1 | 800 − x_r  | 799 − y_r  | |
| 1 | 1 | 0 | x_r        | y_r − 481  | ❌ poza zakresem |
| 1 | 1 | 1 | 800 − x_r  | y_r − 481  | ❌ poza zakresem |

Dlaczego stare (`swap=0, mx=0, my=1`) było „do góry nogami": dawało
`Lx = 1280 − y_r`, `Ly = 799 − x_r`, co względem poprawnego
`(Lx = y_r, Ly = x_r − 1)` jest **obrotem o 180°**.

---

## 6. Powiązanie z ekranem (rotacja)

W `main/drivers/display_init_panel10jc.c`:

```c
disp_cfg.rotation = { .swap_xy = false, .mirror_x = true, .mirror_y = true };
disp_cfg.flags.sw_rotate = true;                       // rotacja programowa
...
lv_display_set_rotation(s_lv_display, LV_DISPLAY_ROTATION_270);
```

Ważny szczegół: przy `sw_rotate = true` konfiguracyjne `.rotation` **nie trafia
do sprzętu** (funkcja `lvgl_port_disp_rotation_update` zwraca wcześniej).
Treść ekranu obraca programowo `lv_draw_sw_rotate(..., 270°, ...)`, a punkty
dotyku obraca `lv_display_rotate_point(270°)` tą samą konwencją LVGL — dlatego
**nie zmieniamy rotacji ekranu**, tylko ustawiamy poprawne lustra dotyku.

Spójność zapewnia LVGL 9:
- ekran: logiczny 1280×800 → (270°) → bufor natywny 800×1280,
- dotyk: natywny 800×1280 → (`rotate_point` 270°) → logiczny 1280×800.

### 6.1 Co dokładnie trzeba zrobić po stronie LVGL (checklista)

Żeby dotyk był **aktywny i poprawnie wyświetlony**, po stronie LVGL muszą być
spełnione cztery rzeczy — jeśli którejś brakuje, dotyk nie działa albo jest
przesunięty/obrócony:

1. **Ekran LVGL na tym samym wyświetlaczu co dotyk.** W
   `display_init_panel10jc.c` ekran tworzy `lvgl_port_add_disp_dsi(...)`, a
   dotyk wiąże się z nim przez `.disp = lv_display_get_default()` w
   `touch_init_panel10jc.c`.

2. **Rotacja LVGL 270°** — `lv_display_set_rotation(s_lv_display, LV_DISPLAY_ROTATION_270)`.
   To obraca **zarówno treść ekranu, jak i punkty dotyku** tą samą konwencją
   LVGL. Bez tego krajobraz 1280×800 leżałby na boku, a dotyk działałby po
   przekątnej.

3. **Rotacja programowa (`sw_rotate = true`)** w `lvgl_port_display_cfg_t.flags`.
   Panel JD9365 jest natywnie 800×1280 (portret), więc LVGL renderuje w
   krajobrazie, a bufor obraca w callbacku flush (`lv_draw_sw_rotate`).
   Uwaga: przy `sw_rotate=true` pole `.rotation {mirror_x, mirror_y}` ekranu
   **nie trafia do sprzętu** — dlatego lustra ustawia się wyłącznie po stronie
   dotyku (sekcja 5).

4. **Indev dotyku zarejestrowany i odpytujący** — `lvgl_port_add_touch()` +
   `lv_indev_set_mode(LV_INDEV_MODE_TIMER)` + timer 10 ms + `lv_timer_ready()`.
   To mostek `esp_lcd_touch` → LVGL; bez niego LVGL w ogóle nie wie, że
   istnieje ekran dotykowy.

Dodatkowo w `touch_init_panel10jc.c` indev ma podpięte callbacki
`LV_EVENT_PRESSED` / `LV_EVENT_PRESSING` (`touch_activity_event_cb`), które:
- budzą ekran (`display_note_activity()` — reset timera wygaszacza / przyciemniania),
- po włączeniu debugu rysują czerwoną kropkę pod palcem (overlay diagnostyczny),
- zapisują surowe próbki do `/sdcard/panel/touch.csv` (rejestrator `data_log_touch`).

---

## 7. Budowanie i wgrywanie

Środowisko (Windows):
```
ESP-IDF: C:\Espressif\frameworks\esp-idf  (v5.5.5)
Python:  C:\Users\kruse\.espressif\python_env\idf5.5_py3.11_env
Toolchain: riscv32-esp-elf esp-14.2.0_20260121
```

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
. C:\Espressif\frameworks\esp-idf\export.ps1
Set-Location 'C:\Projects\OfflineWorkspace\PROJEKTY\PROJEKTY_W_BUDOWIE\Guiton 10'

# kompilacja (preset panel10jc)
idf.py -B build-panel10jc build

# wgranie (panel na COM3)
idf.py -B build-panel10jc -p COM3 flash
```

Mapa flasha (16 MB, DIO, 80 MHz):
```
0x2000  bootloader        22 992 B
0x8000  partition-table    3 072 B
0xf000  ota_data_initial   8 192 B
0x20000 aplikacja      4 902 208 B   (partycja 6 MB, 22% wolne)
```

---

## 8. Lista kontrolna

Po uruchomieniu w logu szeregowym (115200 8N1) musisz zobaczyć, **w tej
kolejności**:

```
gsl3680: init gls3680
gsl3680: gsl3680 connect
gsl3680: read reg 0xf0 before is 0 0 0 0
gsl3680: writing 0xf0 0x12
gsl3680: read reg 0xf0 after is 12 34 56 0
gsl3680: start init
gsl3680: clear reg
gsl3680: start load fw
gsl3680: load fw: all 4356 writes ACKed
gsl3680: load fw success
gsl3680: read 0xb0 = 5a,5a,5a,5a (retry 1/10)
gsl3680: gsl3680 startup success
touch: Touch rotation aligned (swap=0, mirror_x=1, mirror_y=0)
touch: Touch polling tuned: mode=timer, period=10 ms
touch: Touch initialized (esp_lvgl_port + GSL3680)
```

Test fizyczny: dotknij kafelek w lewym górnym rogu → aktywuje się **dokładnie
ten** kafelek (nie prawy dolny). Przesuń palec po 4 rogach — kursor/aktywacja
podąża za palcem.

---

## 9. Rozwiązywanie problemów

| Objaw | Przyczyna | Rozwiązanie |
|---|---|---|
| Bootloop `Store access fault` przy starcie dotyku | bufor `touch_data` < 44 B | `uint8_t touch_data[44]` + klamra `Finger_num` |
| Dotyk „do góry nogami" / po przekątnej | złe lustra osi | `mirror_x=1`, `mirror_y=0`, `swap_xy=0` |
| `gsl3680 startup failed after 10 retries` | kontroler nie wystartował (brak 0x5a5a5a5a w 0xb0) | sprawdź zasilanie/RST GPIO22, pull-upy I2C, adres 0x40, clock ≤400 kHz |
| `Unable to initialize the I2C address` | brak `driver_data` (wybór adresu I2C) lub brak RST/INT GPIO | dodaj `.driver_data = &s_gsl3680_io_cfg` (0x40) i ustaw `rst_gpio_num`/`int_gpio_num` |
| Odczyty dotyku zaszumione / absurdalne wartości (`raw sense = 17226`) | brak wewnętrznych pull-upów I2C | `enable_internal_pullup = true` w konfiguracji magistrali I2C |
| Dotyk martwy po przełączeniu na tryb EVENT/INT | linia INT GSL3680 nie wyzwala się na tej płycie | wróć do `LV_INDEV_MODE_TIMER` + 10 ms |
| Kafelek reaguje, ale przesunięty o stały wektor | offset/scale | skoryguj `x_max/y_max` lub `scale` w `lvgl_port_touch_cfg_t` |
| Dotyk wcale nie reaguje (ale ekran działa) | `lvgl_port_add_touch` nie podpięty / polling | sprawdź log `Touch initialized`, `lv_indev_set_mode(TIMER)` |

---

## 10. Podsumowanie — co było zrobione, żeby zadziałało

1. **Naprawiono przepełnienie bufora** w driverze GSL3680
   (`touch_data[24]` → `[44]`) — usunęło bootloop.
2. **Dodano klamrę `Finger_num ≤ 3`** — bezpieczeństwo pętli parsujących.
3. **Ustawiono poprawne lustra osi** (`mirror_x=1`, `mirror_y=0`, `swap=0`)
   — usunęło odwrócenie dotyku o 180°.
4. **Wybrano adres I2C** przez `.driver_data` (0x40) + RST/INT — usunęło
   ostrzeżenie „Unable to initialize the I2C address".
5. **Włączono wewnętrzne pull-upy I2C** — ustabilizowało odczyty dotyku.
6. **Ustawiono tryb TIMER + 10 ms** (nie EVENT) — linia INT na tej płycie
   nie wyzwala się niezawodnie, więc polling timerem jest jedynym pewnym trybem.
7. **Podpięto driver do LVGL** przez `lvgl_port_add_touch` (scale 1:1),
   z rotacją 270° spójną z ekranem.
8. **Zweryfikowano** w logu start kontrolera (`0x5a5a5a5a`) i test fizyczny.
