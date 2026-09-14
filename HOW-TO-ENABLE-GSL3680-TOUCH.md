# HOW TO ENABLE THE GSL3680 TOUCHSCREEN on the JC8012P4A1C-I-W-Y panel (Guiton 10.1")

Complete step-by-step guide: what must be in place, which settings and which
drivers, so the touch works on this panel. Describes the **working** state
(as of 2026-09-12).

Panel: **ESP32-P4 v1.3** + **JD9365** display (MIPI-DSI 2-lane, native
800×1280 portrait) + **GSL3680** touch (I2C). Firmware `betta-ha-panel-10jc`
v0.8.2-10jc, ESP-IDF v5.5.5.

> Wersja polska: [JAK-URUCHOMIC-DOTYK-GSL3680.md](./JAK-URUCHOMIC-DOTYK-GSL3680.md)

---

## Table of contents
1. [Required elements](#1-required-elements)
2. [GSL3680 driver](#2-gsl3680-driver)
3. [Touch initialization file](#3-touch-initialization-file)
4. [How the whole chain works](#4-how-the-whole-chain-works)
5. [Axis configuration](#5-axis-configuration)
6. [Linking with the display](#6-linking-with-the-display)
7. [Building and flashing](#7-building-and-flashing)
8. [Checklist](#8-checklist)
9. [Troubleshooting](#9-troubleshooting)
10. [Summary](#10-summary)

---

## 1. Required elements

For the touch to work, **four** things must exist and be correct:

| # | Element | Location | Role |
|---|---|---|---|
| 1 | GSL3680 driver | `components/esp_lcd_touch_gsl3680/` | low-level I2C handling + controller firmware |
| 2 | Touch initialization | `main/drivers/touch_init_panel10jc.c` | I2C bus, GPIO, axis configuration, hook-up to LVGL |
| 3 | LVGL touch port | `managed_components/espressif__esp_lvgl_port` (`esp_lvgl_port` component) | driver→LVGL bridge (`lvgl_port_add_touch`) |
| 4 | Link with the display | `main/drivers/display_init_panel10jc.c` | the display must be set in the same layout as the touch |

This panel has **no ready-made BSP** (unlike the Waveshare variants), so the
I2C bus and the touch handle are created manually in `touch_init_panel10jc.c`,
and the `esp_lcd_touch_gsl3680` driver source comes from the official Guition
reference.

---

## 2. GSL3680 driver

### 2.1 Component structure

```
components/esp_lcd_touch_gsl3680/
├── esp_lcd_touch_gsl3680.c        # implementation
└── include/esp_lcd_touch_gsl3680.h# API + macros
```

### 2.2 Key constants from the header

```c
#define MAX_FINGER_NUM      3                     // max 3 touch points
#define ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS (0x40)   // I2C address

// IO configuration macro (used in touch_init):
#define ESP_LCD_TOUCH_IO_I2C_GSL3680_CONFIG()           \
    {                                                   \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS, \
        .control_phase_bytes = 1,                       \
        .dc_bit_offset = 0,                             \
        .lcd_cmd_bits = 8,                              \
        .flags = { .disable_control_phase = 1 }         \
    }
```

### 2.3 Controller startup sequence

`esp_lcd_touch_gsl3680_init()` does (in this order):
1. `esp_lcd_touch_gsl3680_clear_reg()` — clear registers
2. `touch_gsl3680_reset()` — GPIO reset (RST=0 → 20 ms → RST=1), write to `0xe4` and `0xbc`
3. `esp_lcd_touch_gsl3680_load_fw()` — upload the `GSLX680_FW[]` firmware table over I2C
4. `esp_lcd_touch_gsl3680_startup_chip()` — write `0xe0 = 0x00` + `gsl_DataInit`
5. `touch_gsl3680_reset()` + `startup_chip()` again

A correct start is confirmed by `esp_lcd_touch_gsl3680_read_ram_fw()`: it reads
register `0xb0` and waits until it returns `0x5a 0x5a 0x5a 0x5a` (max 10
attempts × 50 ms). The log then shows:

```
gsl3680: read 0xb0 = 5a,5a,5a,5a (retry 1/10)
gsl3680: gsl3680 startup success
```

### 2.4 Reading positions (register 0x80)

`esp_lcd_touch_gsl3680_read_data()` reads **44 bytes** from register `0x80`:

| Index | Meaning |
|---|---|
| `[0]` | number of fingers (`Finger_num`) |
| `[1]`, `[2]` | point 0: Y (lo, hi) |
| `[3]`, `[4]` | point 0: X (lo, hi in 4 bits) + ID in the upper 4 bits |
| `[(j+1)*4+0 .. +3]` | subsequent points j |

Extracting the coordinates:
```c
x  = (touch_data[(j+1)*4+3] & 0x0f) << 8 | touch_data[(j+1)*4+2];
y  =  touch_data[(j+1)*4+1] << 8        | touch_data[(j+1)*4+0];
id = (touch_data[(j+1)*4+3] & 0xf0) >> 4;
```

### 2.5 ⚠️ CRITICAL FIX #1 — buffer overflow (was bootloop)

**Problem:** the original driver had:

```c
uint8_t touch_data[24];   // TOO SMALL !!!
...
touch_gsl3680_i2c_read(tp, 0x80, touch_data, 44);   // writes 44 bytes
```

The 20 surplus bytes overwrote the `s2`/`s3` registers saved on the stack.
With no touch, data = zeros → `s2` restored as 0 → NULL as a pointer inside
`lvgl_port_touchpad_read` → `Store access fault` → **bootloop on the first
touch / touch start**.

**Fix (mandatory):**

```c
uint8_t touch_data[44];   // matched to the read length
```

Additionally — a clamp on the finger count (because `touch_data[0]` from a
noisy read could give >3 and run past `XY_Coordinate[3]`):

```c
Finger_num = touch_data[0];
if (Finger_num > MAX_FINGER_NUM) {
    Finger_num = MAX_FINGER_NUM;   // 3
}
```

The same clamp is repeated after `gsl_alg_id_main()` (a second copy of
`Finger_num`). Exact lines (in this copy of the repository):
`esp_lcd_touch_gsl3680.c` — buffer line 317, 44 B read line 346, clamps
lines 348 and 417.

### 2.6 `get_xy` — bridge to the LVGL port

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
    return (*point_num > 0);   // true = touch present
}
```

Important: the driver **does not implement**
`set_mirror_x/set_mirror_y/set_swap_xy`, so mirror/swap are done **in
software** by `esp_lcd_touch_get_data()` (the `esp_lcd_touch` component) based
on the configuration flags.

---

## 3. Touch initialization file

File: `main/drivers/touch_init_panel10jc.c`

### 3.1 Pins and parameters

```c
#define JC_TOUCH_I2C_PORT    I2C_NUM_1
#define JC_TOUCH_I2C_SCL     GPIO_NUM_8
#define JC_TOUCH_I2C_SDA     GPIO_NUM_7
#define JC_TOUCH_I2C_CLK_HZ  400000          // 400 kHz
#define JC_TOUCH_RST_GPIO    GPIO_NUM_22     // reset (active low)
#define JC_TOUCH_INT_GPIO    GPIO_NUM_21     // interrupt (active low)
#define JC_TOUCH_X_MAX       800             // short axis (portrait)
#define JC_TOUCH_Y_MAX       1280            // long axis (portrait)
```

### 3.2 I2C bus

```c
const i2c_master_bus_config_t i2c_cfg = {
    .i2c_port = I2C_NUM_1,
    .sda_io_num = GPIO_NUM_7,
    .scl_io_num = GPIO_NUM_8,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = true,    // ← KEY (see below)
};
i2c_new_master_bus(&i2c_cfg, &s_i2c_bus);
```

Note: **I2C_NUM_1 is shared** with the ES8311 audio codec — the function
`jc8012_i2c_bus_get()` exposes the same handle to `board_extras`.

⚠️ **The internal I2C pull-ups must be enabled** (`enable_internal_pullup =
true`). The touch I2C bus on this board **relies on the ESP32-P4 internal
pull-ups** — without them the reads become marginal and occasionally corrupted
(symptom: raw samples with absurd values, e.g. `raw sense = 17226`). An
earlier entry in this guide (`= false`) was wrong — the production code has
`= true`.

### 3.3 Creating the touch handle

```c
// I2C address selector — without it the driver logs
// "Unable to initialize the I2C address" (see 3.3.1).
static const esp_lcd_touch_io_gsl3680_config_t s_gsl3680_io_cfg = {
    .dev_addr = ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS,   // 0x40
};

const esp_lcd_touch_config_t tp_cfg = {
    .x_max = 800,
    .y_max = 1280,
    .rst_gpio_num = GPIO_NUM_22,
    .int_gpio_num = GPIO_NUM_21,
    .driver_data = &s_gsl3680_io_cfg,   // ← KEY (I2C address selection)
    .levels = { .reset = 0, .interrupt = 0 },
    .flags = {
        .swap_xy  = 0,
        .mirror_x = 1,    // ← KEY (was 0)
        .mirror_y = 0,    // ← KEY (was 1)
    },
};

esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GSL3680_CONFIG();
tp_io_config.scl_speed_hz = 400000;
esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_config, &tp_io_handle);
esp_lcd_touch_new_i2c_gsl3680(tp_io_handle, &tp_cfg, &s_touch_handle);
```

### 3.3.1 ⚠️ CRITICAL FIX #3 — I2C address selection

The GSL3680 driver selects the controller's I2C address (0x40) **via the
INT/A0 pin**: during reset it asserts INT=0, raises RST, and only then talks
over I2C. To do that it needs **all three** things at once:

1. `rst_gpio_num` ≠ `GPIO_NUM_NC` (here **GPIO22**),
2. `int_gpio_num` ≠ `GPIO_NUM_NC` (here **GPIO21**),
3. `driver_data` pointing to an `esp_lcd_touch_io_gsl3680_config_t` with
   `dev_addr = ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS` (**0x40**).

If any of them is missing, the driver falls into the emergency branch and logs:

```
W gsl3680: Unable to initialize the I2C address
```

and then performs only a plain reset without address selection. The touch
usually still starts anyway (after reset the GSL3680 answers at 0x40 by
default), which is why the warning is easy to ignore — but it is a symptom of
a **missing address configuration**. Fix: `.driver_data = &s_gsl3680_io_cfg`
plus RST and INT set. After the fix the warning disappears — this requires
**rebuilding and re-flashing the firmware**.

### 3.4 Forcing the axis settings (after creation)

`touch_apply_display_rotation_alignment()` overwrites the flags explicitly
(important, because the GSL3680 driver has no HW mirror — the setters only
update the flags):

```c
const bool swap_xy  = false;
const bool mirror_x = true;    // ← after the fix
const bool mirror_y = false;   // ← after the fix

esp_lcd_touch_set_swap_xy (handle, swap_xy);
esp_lcd_touch_set_mirror_x(handle, mirror_x);
esp_lcd_touch_set_mirror_y(handle, mirror_y);
```

### 3.5 Hook-up to LVGL

```c
const lvgl_port_touch_cfg_t touch_cfg = {
    .disp   = lv_display_get_default(),   // the same display as the 270° rotation
    .handle = s_touch_handle,
    .scale  = { .x = 1.0f, .y = 1.0f },   // 1:1, no scaling
};
s_touch_indev = lvgl_port_add_touch(&touch_cfg);
```

### 3.6 Polling — TIMER mode (not EVENT)

```c
lv_indev_set_mode(indev, LV_INDEV_MODE_TIMER);      // polled by a timer
lv_timer_t *t = lv_indev_get_read_timer(indev);
lv_timer_set_period(t, 10);                          // every 10 ms
lv_timer_ready(t);
```

⚠️ **Why TIMER and not EVENT (INT interrupt on GPIO21):** the EVENT
(interrupt-driven) mode was tested to reduce touch latency, but on this board
**the GSL3680 INT line does not trigger reliably**. In EVENT mode LVGL keeps
the read timer suspended and waits for the interrupt — since INT never arrives,
LVGL **never reads the touch** and the screen is completely dead. TIMER +
10 ms is the tested and stable configuration (identical to the Waveshare
panels from the same port).

### 3.7 Init retry

`touch_init()` tries `jc_touch_new()` up to **8 times** with a **250 ms**
interval (the controller is sometimes not ready immediately after the
PSRAM/SDIO start).

---

## 4. How the whole chain works

```
GSL3680 (I2C 0x80, 44 B)
   │  esp_lcd_touch_gsl3680_read_data()   → XY_Coordinate[] (x_r, y_r)
   ▼
esp_lcd_touch_get_data()                 → software mirror/swap:
   │                                          x = 800 - x_r  (mirror_x=1)
   │                                          y = y_r        (mirror_y=0)
   ▼
lvgl_port_touchpad_read()                → data->point = (x_p, y_p)
   ▼
LVGL lv_display_rotate_point(270°)       → Lx = y_p ; Ly = 800 - x_p - 1
   ▼
LVGL (landscape 1280×800) — hits the tile
```

The final formula with the current settings:

```
Lx = y_r
Ly = x_r - 1
```

---

## 5. Axis configuration

The most important decision: **which axes to mirror and whether to swap X/Y**.

The correct combination for this panel (horizontal mount, 270° rotation):

```
swap_xy  = 0   (do not swap the axes)
mirror_x = 1   (mirror X)
mirror_y = 0   (do not mirror Y)
```

Table of all 8 combinations and their result (at 270° rotation):

| swap | mx | my | Lx         | Ly         | notes |
|---|---|---|------------|------------|-------|
| 0 | 0 | 0 | y_r        | 799 − x_r  | |
| 0 | 0 | 1 | 1280 − y_r | 799 − x_r  | **old setting — touch rotated 180°** |
| **0** | **1** | **0** | **y_r**    | **x_r − 1** | **CURRENT — CORRECT** |
| 0 | 1 | 1 | 1280 − y_r | x_r − 1    | |
| 1 | 0 | 0 | x_r        | 799 − y_r  | |
| 1 | 0 | 1 | 800 − x_r  | 799 − y_r  | |
| 1 | 1 | 0 | x_r        | y_r − 481  | ❌ out of range |
| 1 | 1 | 1 | 800 − x_r  | y_r − 481  | ❌ out of range |

Why the old setting (`swap=0, mx=0, my=1`) was "upside down": it produced
`Lx = 1280 − y_r`, `Ly = 799 − x_r`, which relative to the correct
`(Lx = y_r, Ly = x_r − 1)` is a **180° rotation**.

---

## 6. Linking with the display

In `main/drivers/display_init_panel10jc.c`:

```c
disp_cfg.rotation = { .swap_xy = false, .mirror_x = true, .mirror_y = true };
disp_cfg.flags.sw_rotate = true;                       // software rotation
...
lv_display_set_rotation(s_lv_display, LV_DISPLAY_ROTATION_270);
```

Important detail: with `sw_rotate = true` the config `.rotation` **does not
reach the hardware** (the function `lvgl_port_disp_rotation_update` returns
early). The screen content is rotated in software by
`lv_draw_sw_rotate(..., 270°, ...)`, and the touch points are rotated by
`lv_display_rotate_point(270°)` using the same LVGL convention — that is why
**we do not change the display rotation**, only set the correct touch mirrors.

Consistency is guaranteed by LVGL 9:
- display: logical 1280×800 → (270°) → native 800×1280 buffer,
- touch: native 800×1280 → (`rotate_point` 270°) → logical 1280×800.

### 6.1 What exactly must be done on the LVGL side (checklist)

For the touch to be **active and correctly mapped**, four things must be
satisfied on the LVGL side — if any is missing, the touch does not work or is
shifted/rotated:

1. **The LVGL display is on the same display as the touch.** In
   `display_init_panel10jc.c` the display is created by
   `lvgl_port_add_disp_dsi(...)`, and the touch is bound to it via
   `.disp = lv_display_get_default()` in `touch_init_panel10jc.c`.

2. **LVGL rotation 270°** — `lv_display_set_rotation(s_lv_display, LV_DISPLAY_ROTATION_270)`.
   This rotates **both the screen content and the touch points** using the
   same LVGL convention. Without it the 1280×800 landscape would lie on its
   side and the touch would work diagonally.

3. **Software rotation (`sw_rotate = true`)** in
   `lvgl_port_display_cfg_t.flags`. The JD9365 panel is natively 800×1280
   (portrait), so LVGL renders in landscape and the buffer is rotated in the
   flush callback (`lv_draw_sw_rotate`). Note: with `sw_rotate=true` the
   display's `.rotation {mirror_x, mirror_y}` field **does not reach the
   hardware** — therefore the mirrors are set only on the touch side
   (section 5).

4. **The touch indev is registered and polling** — `lvgl_port_add_touch()` +
   `lv_indev_set_mode(LV_INDEV_MODE_TIMER)` + 10 ms timer + `lv_timer_ready()`.
   This is the `esp_lcd_touch` → LVGL bridge; without it LVGL does not know a
   touchscreen exists at all.

Additionally, in `touch_init_panel10jc.c` the indev has the
`LV_EVENT_PRESSED` / `LV_EVENT_PRESSING` callbacks attached
(`touch_activity_event_cb`), which:
- wake the screen (`display_note_activity()` — reset the screensaver/dimming timer),
- draw a red dot under the finger when debug is enabled (diagnostic overlay),
- save raw samples to `/sdcard/panel/touch.csv` (the `data_log_touch` logger).

---

## 7. Building and flashing

Environment (Windows):
```
ESP-IDF: C:\Espressif\frameworks\esp-idf  (v5.5.5)
Python:  C:\Users\kruse\.espressif\python_env\idf5.5_py3.11_env
Toolchain: riscv32-esp-elf esp-14.2.0_20260121
```

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
. C:\Espressif\frameworks\esp-idf\export.ps1
Set-Location 'C:\Projects\OfflineWorkspace\PROJEKTY\PROJEKTY_W_BUDOWIE\Guiton 10'

# build (panel10jc preset)
idf.py -B build-panel10jc build

# flash (panel on COM3)
idf.py -B build-panel10jc -p COM3 flash
```

Flash map (16 MB, DIO, 80 MHz):
```
0x2000  bootloader        22 992 B
0x8000  partition-table    3 072 B
0xf000  ota_data_initial   8 192 B
0x20000 application    4 902 208 B   (6 MB partition, 22% free)
```

---

## 8. Checklist

After start-up, in the serial log (115200 8N1) you must see, **in this
order**:

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

Physical test: tap the tile in the top-left corner → **exactly that** tile is
activated (not the bottom-right one). Move your finger across the 4 corners —
the cursor/activation follows the finger.

---

## 9. Troubleshooting

| Symptom | Cause | Solution |
|---|---|---|
| Bootloop `Store access fault` at touch start | `touch_data` buffer < 44 B | `uint8_t touch_data[44]` + `Finger_num` clamp |
| Touch "upside down" / diagonal | wrong axis mirrors | `mirror_x=1`, `mirror_y=0`, `swap_xy=0` |
| `gsl3680 startup failed after 10 retries` | controller did not start (no 0x5a5a5a5a in 0xb0) | check power/RST GPIO22, I2C pull-ups, address 0x40, clock ≤400 kHz |
| `Unable to initialize the I2C address` | missing `driver_data` (I2C address selection) or missing RST/INT GPIO | add `.driver_data = &s_gsl3680_io_cfg` (0x40) and set `rst_gpio_num`/`int_gpio_num` |
| Noisy touch reads / absurd values (`raw sense = 17226`) | missing internal I2C pull-ups | `enable_internal_pullup = true` in the I2C bus config |
| Touch dead after switching to EVENT/INT mode | the GSL3680 INT line does not trigger on this board | go back to `LV_INDEV_MODE_TIMER` + 10 ms |
| Tile responds but is shifted by a constant vector | offset/scale | correct `x_max/y_max` or `scale` in `lvgl_port_touch_cfg_t` |
| Touch does not respond at all (but the display works) | `lvgl_port_add_touch` not hooked up / polling | check the `Touch initialized` log, `lv_indev_set_mode(TIMER)` |

---

## 10. Summary

1. **Fixed the buffer overflow** in the GSL3680 driver
   (`touch_data[24]` → `[44]`) — removed the bootloop.
2. **Added the `Finger_num ≤ 3` clamp** — safety for the parsing loops.
3. **Set the correct axis mirrors** (`mirror_x=1`, `mirror_y=0`, `swap=0`)
   — removed the 180° touch inversion.
4. **Selected the I2C address** via `.driver_data` (0x40) + RST/INT — removed
   the "Unable to initialize the I2C address" warning.
5. **Enabled the internal I2C pull-ups** — stabilized the touch reads.
6. **Set TIMER + 10 ms mode** (not EVENT) — the INT line on this board does
   not trigger reliably, so timer polling is the only reliable mode.
7. **Hooked the driver up to LVGL** via `lvgl_port_add_touch` (1:1 scale),
   with 270° rotation consistent with the display.
8. **Verified** the controller start (`0x5a5a5a5a`) in the log and a physical
   test.
