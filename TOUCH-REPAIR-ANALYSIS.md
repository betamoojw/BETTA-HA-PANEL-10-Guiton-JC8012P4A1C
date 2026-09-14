# Panel JC8012P4A1C-I-W-Y (Guiton 10.1") — repair analysis: bootloop + touch

Date: 2026-09-12
Firmware: `betta-ha-panel-10jc` v0.8.2-10jc
Final state: **the panel boots, display and touch work correctly** (tiles activate exactly under the finger).

> Wersja polska: [ANALIZA-NAPRAWA-DOTYKU.md](./ANALIZA-NAPRAWA-DOTYKU.md)

---

## 1. Hardware and environment

| Parameter | Value |
|---|---|
| SoC | ESP32-P4, revision **v1.3** (efuse v0.3) |
| Crystal | 40 MHz |
| Flash | **16 MB** (SPI DIO, 80 MHz) |
| PSRAM | 32 MB (hex PSRAM, 200 MHz, X16) |
| Display | **JD9365** MIPI-DSI 2-lane, native 800×1280 (portrait), RGB565 |
| Touch | **GSL3680** — I2C addr 0x40, SCL=GPIO8, SDA=GPIO7, 400 kHz, RST=GPIO22, INT=GPIO21 |
| Panel MAC | `80:f1:b2:d3:5c:e9` |
| Programming port | COM3 (CH340), 115200 8N1 (console) |
| ESP-IDF | v5.5.5-dirty (`C:\Espressif\frameworks\esp-idf`) |
| Toolchain | riscv32-esp-elf esp-14.2.0_20260121 |
| Python env | `C:\Users\kruse\.espressif\python_env\idf5.5_py3.11_env` |
| esptool | v4.12.0 |
| Project directory | `C:\Projects\OfflineWorkspace\PROJEKTY\PROJEKTY_W_BUDOWIE\Guiton 10` |
| Build directory | `build-panel10jc` (preset `panel10jc`) |

Flash partition layout (from the boot log):
```
nvs        0x00009000  0x6000
otadata    0x0000f000  0x2000
phy_init   0x00011000  0x1000
factory    0x00020000  0x600000   <- firmware goes here
ota_0      0x00620000  0x600000
storage    0x00c20000  0x3e0000
```

---

## 2. There were TWO independent bugs

### Bug #1 — bootloop (Store access fault) — GSL3680 buffer overflow

**Symptom:** the panel booted, and on the first touch read it reset
(`Store access fault`, "bootloop").

**Cause:** in `components/esp_lcd_touch_gsl3680/esp_lcd_touch_gsl3680.c` the
function `esp_lcd_touch_gsl3680_read_data()` declared the buffer:

```c
uint8_t touch_data[24];   // TOO SMALL
...
touch_gsl3680_i2c_read(tp, ESP_LCD_TOUCH_GSL3680_READ_XY_REG, touch_data, 44);
```

I2C wrote **44 bytes** into a **24-byte** buffer. The surplus 20 bytes
overwrote the `s2`/`s3` registers saved on the stack. When there was no touch,
the data was zeros → restored `s2 = 0` → NULL as the `data` pointer in
`lvgl_port_touchpad_read` → `sw zero,48(s2)` = a write to address 0x30 =
**Store access fault**.

**Fix (file `components/esp_lcd_touch_gsl3680/esp_lcd_touch_gsl3680.c`):**
- line 310: `uint8_t touch_data[44];` (matched to the I2C read length),
- lines 325–328 and 361–362: clamp `Finger_num` to `MAX_FINGER_NUM` (3), so
  the parsing loops cannot run past the buffer.

Verification in the disassembler (riscv32-esp-elf-objdump): the function
`esp_lcd_touch_gsl3680_read_data` has a 208 B stack frame, the `touch_data`
buffer at `sp+132`, the read `li a3,44`, and the `Finger_num <= 3` clamp is
present. The boot log confirms: `gsl3680 startup success` and no further
resets.

### Bug #2 — touch inverted 180° ("upside down")

**Symptom:** after fix #1 the touch already worked, but a tap landed in the
wrong place — the panel "read the touch upside down / diagonally".

**Cause:** wrong axis mirrors in the touch configuration.

**Fix (file `main/drivers/touch_init_panel10jc.c`):**

| Flag | Was | Is |
|---|---|---|
| `swap_xy` | 0 | 0 (unchanged) |
| `mirror_x` | **0** | **1** |
| `mirror_y` | **1** | **0** |

Changed in two places: `tp_cfg.flags` (lines 116–118) and
`touch_apply_display_rotation_alignment()` (lines 143–145), because for the
GSL3680 the `esp_lcd_touch_set_*` functions only update the flags (the driver
has no HW mirror — mirroring is done in software).

---

## 3. Full analysis of the touch transformation chain

The data flows like this:

```
RAW GSL3680 (x_r, y_r)        x_r ∈ [0,800), y_r ∈ [0,1280)  (native portrait)
   │  software mirror/swap (esp_lcd_touch_get_data)
   ▼
AFTER MIRROR (x_p, y_p)
   │  LVGL lv_display_rotate_point()  with LV_DISPLAY_ROTATION_270
   ▼
LOGICAL (Lx, Ly)  →  reaches LVGL (landscape 1280×800)
```

Formulas (exact, from the source code):

- mirror: `x_p = 800 - x_r` (when mirror_x), `y_p = 1280 - y_r` (when mirror_y)
- swap: exchange `x_p ↔ y_p`
- LVGL 270° (`lv_display_rotate_point`, `disp->hor_res = 800`):
  ```
  Lx = y_p
  Ly = 800 - x_p - 1
  ```

### TARGET configuration (swap=0, mirror_x=1, mirror_y=0):

```
x_p = 800 - x_r
y_p = y_r
Lx = y_r
Ly = 800 - (800 - x_r) - 1 = x_r - 1
```
So clean: `Lx = y_r`, `Ly = x_r - 1` — the axes land in the right places, no
surprises (the 1 px shift is the standard LVGL `-1` offset).

### Old configuration (swap=0, mirror_x=0, mirror_y=1):

```
x_p = x_r
y_p = 1280 - y_r
Lx = 1280 - y_r
Ly = 799 - x_r
```

Comparison of old and new: `Lx_old = 1280 - Lx_new`, `Ly_old ≈ 799 - Ly_new`
— so the old map is a **180° rotation** of the new one. Hence the "upside
down" symptom.

### Table of all 8 combinations (for 270°):

| swap | mx | my | Lx =            | Ly =            | correct for 10"? |
|---|---|---|-----------------|-----------------|---|
| 0 | 0 | 0 | y_r             | 799 − x_r       | no |
| 0 | 0 | 1 | 1280 − y_r      | 799 − x_r       | no (was — inverted 180°) |
| **0** | **1** | **0** | **y_r**         | **x_r − 1**     | **YES (current)** |
| 0 | 1 | 1 | 1280 − y_r      | x_r − 1         | no |
| 1 | 0 | 0 | x_r             | 799 − y_r       | no |
| 1 | 0 | 1 | 800 − x_r       | 799 − y_r       | no |
| 1 | 1 | 0 | x_r             | y_r − 481       | NO — out of range |
| 1 | 1 | 1 | 800 − x_r       | y_r − 481       | NO — out of range |

The combinations `swap=1, mirror_x=1` are mathematically incorrect (they map
the long axis onto the logical vertical and go out of the Ly range).

### Why we do NOT change the display rotation

The display has `sw_rotate = true`, so the config `.rotation {mirror_x, mirror_y}`
in [display_init_panel10jc.c](main/drivers/display_init_panel10jc.c) (lines 526–528)
**is not** sent to the hardware (`lvgl_port_disp_rotation_update` returns
early). The screen content is rotated in software by
`lv_draw_sw_rotate(..., LV_DISPLAY_ROTATION_270, ...)` in the flush callback,
and `lv_display_set_rotation(LV_DISPLAY_ROTATION_270)` (line 574) rotates the
touch points using the same LVGL convention. Therefore it was enough to set
the correct touch mirrors — the touch axis and the display axis are consistent
with each other.

---

## 4. Build and flash commands

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
. C:\Espressif\frameworks\esp-idf\export.ps1
Set-Location 'C:\Projects\OfflineWorkspace\PROJEKTY\PROJEKTY_W_BUDOWIE\Guiton 10'

# build (panel10jc preset: JD9365 + GSL3680, 800x1280)
idf.py -B build-panel10jc build

# flash to the panel (COM3)
idf.py -B build-panel10jc -p COM3 flash
```

The build finishes cleanly (only known, pre-existing warnings).

---

## 5. Firmware and flash data

| Element | Address | Size |
|---|---|---|
| bootloader | 0x2000 | 22 992 B (0x59d0) |
| partition-table | 0x8000 | 3 072 B |
| ota_data_initial | 0xf000 | 8 192 B |
| application (app) | 0x20000 | **4 902 208 B** (0x4acd40) |

- Smallest app partition: 0x600000 (6 MB) → **22% free**
- flash: 16 MB, mode DIO, freq 80 MHz, baud 460800
- chip: ESP32-P4 rev v1.3, MAC `80:f1:b2:d3:5c:e9`

SHA256 of the built firmware:
```
44DE70BD0B3C422A889202289EC39A066C763E0F7C93501F80C18F6D2D8386E1
```

Working firmware copies:
- `release\betta-ha-panel-10jc.bin` (overwritten with the working version)
- `release\archive\betta-ha-panel-10jc-2026-09-12-dotyk-fix.bin` (dated copy)

---

## 6. Boot log evidence (abridged)

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
touch: Touch rotation aligned (swap=0, mirror_x=1, mirror_y=0)   <- after the fix
touch: Touch initialized (esp_lvgl_port + GSL3680)
```

---

## 7. Files changed

1. `components/esp_lcd_touch_gsl3680/esp_lcd_touch_gsl3680.c`
   - `touch_data[24]` → `touch_data[44]` (stack overflow fix)
   - `Finger_num <= MAX_FINGER_NUM` (3) clamp

2. `main/drivers/touch_init_panel10jc.c`
   - `mirror_x`: 0 → **1**, `mirror_y`: 1 → **0** (180° inversion fix)

3. Documentation: `ANALIZA-NAPRAWA-DOTYKU.md` (this file)

---

## 8. Touch dead zone (horizontal band) — HARDWARE DEFECT

### Symptom
The screen has a **dead horizontal band** around **LVGL y ≈ 253–371** (in the
settings screen it showed up as the non-working "SD" tile). Touches in this
band are not reported at all — no reaction whatsoever.

### Proof that it is hardware, not software
1. **The touch IC itself reports 0 fingers** in this band. The driver logs the
   raw I2C data on every new press (`RAWpre press`). In a log with ~22 presses
   the Y values jump from `~284` to `~418` — **nothing** in between, meaning
   the `Finger_num` register (byte 0 of the 44-byte block) is zero there.
2. **The configuration does not ignore any area.** In `gsl_config_data_id[]`:
   - `ignore_y = conf[0x25] = 0`
   - `ignore_x = conf[0x26] = 0`
   - `edge_cut = conf[0x27] = 0x04040404` (only 4 units at the edges)
   
   So the host algorithm (`ScreenResolution`/`PointIgnore`) has no basis for
   rejecting a point in the middle of the screen.
3. **At the band's edges the IC sets the "able" (0x40) flag** in the older Y
   byte. This means that at the edges of the dead band the IC detects a weak
   signal and marks it as unreliable itself, and in the middle of the band it
   detects nothing.

### Interpretation
The band ≈ 2 dead sensor lines out of 14 (`sen_num=14`). Physically it is
broken traces / a flex tape in the middle of the touch sensor. **This cannot
be fixed in firmware** — the host cannot conjure up a touch where the IC sees
no capacitance.

### The only software solution (applied)
Move all interactive elements OUT of the dead band:
- `main/ui/ui_settings.c` — the settings category tiles were recomputed into a
  deterministic **2-column** layout, so "SD" now sits at y ≈ 190–234
  (outside the 253–371 band). Previously the flex ROW_WRAP layout rendered
  them in 1 column and "SD" landed in the dead band (y 290–334).

### Recommendation
If the panel is under warranty — **replace it** (factory defect of the touch
sensor).

---

## 9. SD card — the driver WORKS (the UI showed an error)

The SD driver (`main/drivers/sdcard_init_panel10jc.c`) is fully functional:
- boot: `sdcard: microSD mounted at /sdcard (name=SH64G, capacity=63864569856 bytes)`
- REST: `GET /api/sd/status` → `{"mounted":true,"logging_enabled":true,"total_bytes":63847858176,"free_bytes":63841828864}`

The only problem was an **outdated text** in the settings screen
(`st_build_sd()`), which claimed "This firmware has no SD card driver".
Fixed: `main/ui/ui_settings.c` — `st_build_sd()` now shows the real state:
"Mounted", capacity and free space (via `sdcard_is_ready()` and
`sdcard_info()`), and when no card is present — a message about automatic
mounting.
