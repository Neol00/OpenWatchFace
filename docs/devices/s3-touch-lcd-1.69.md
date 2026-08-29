# Waveshare ESP32-S3-Touch-LCD-1.69 — install guide

| | |
|---|---|
| MCU | ESP32-S3 — dual-core, **8 MB PSRAM**, 16 MB flash |
| Display | ST7789V2 LCD 240×280, SPI (same UI tier as the LCD-2), PWM backlight |
| Touch | CST816T capacitive (I²C, **polled**, with a real reset line) |
| RTC | **PCF85063** — time survives power-off |
| IMU | QMI8658 (steps + sleep tracking) |
| Power | **No PMU** — ETA6098 charger, battery on an ADC divider (GPIO1), **soft power latch** |
| Audio | **Onboard buzzer** (PWM backend — alarms, chimes, notification dings) |
| Storage | **None** — no card slot; Files app is FFat-only |
| Haptics | **None** — no motor fitted |

> **Two things make this board special in the firmware:** it is the first board to
> actually use the **PWM buzzer audio backend** (`BOARD_HAS_AUDIO_PWM`), and the first
> ESP board with a **soft power latch** — battery power stays on only because the
> firmware asserts `SYS_EN`, exactly like the Tuya T5's GPIO19 keep-alive.

---

## 0. Which hardware revision do you have?

The board exists as **V1** and **V2.1**, with the same peripherals on different pins
(buzzer, power latch, PWR-key sense). The firmware **defaults to V2.1**, which is what
current stock ships — and the only wiring consistent with the 8 MB octal-PSRAM module
(OPI PSRAM claims GPIO35-37 inside the module, which is why Waveshare moved V1's
SYS_EN 35 / SYS_OUT 36 off those pins).

Symptoms of a wrong revision selection: **the buzzer is silent**, and/or **the watch
dies the instant the PWR button is released on battery**. If you really have a V1
unit, set `BOARD_169_HW_V1` to `1` at the top of
`OpenWatchFace/board_ws_s3_touch_lcd_169.h`.

## 1. Install the Arduino IDE

Download and install the Arduino IDE from
<https://www.arduino.cc/en/software/>.

Launch it once so it creates your sketchbook (libraries) folder.

## 2. Install the ESP32 core (Espressif Systems)

Open the Board Manager in the Arduino IDE, search for `esp32`, and install it. Make
sure the version you install is the one provided by **Espressif Systems** — not the
legacy "Arduino ESP32 Boards" entry.

## 3. Place the bundled libraries

Place the provided libraries from this repo into the Arduino IDE libraries directory.
The location varies depending on what OS you are running.

On Windows the default location for the libraries is:
`C:\Users\yourusername\Documents\Arduino\libraries`

On Linux the default location for the libraries is:
`/home/yourusername/Arduino/libraries`

Copy the **contents** of this repo's `libraries/` folder in, so the library folders
and `lv_conf.h` land directly inside it. `lv_conf.h` must sit **next to** the `lvgl`
folder, not inside it — LVGL requires that. If an older `lvgl` or
`GFX_Library_for_Arduino` is already there, overwrite it; the firmware needs these
exact versions (LVGL 9.5.0, Arduino_GFX 1.6.5).

> **No touch library to install.** The CST816 driver is in-tree
> (`OpenWatchFace/touch_cst816.h`) — nothing to copy.

## 4. Install the custom SoC libs

Replace the `esp32s3-libs` by first deleting the original directory then placing the
provided libs from this repo in the correct location. The location varies depending on
what OS you are running.

On Windows the location for the esp32 libs is:
`C:\Users\yourusername\AppData\Local\Arduino15\packages\esp32\tools`

On Linux the location for the esp32 libs is:
`/home/yourusername/.arduino15/packages/esp32/tools`

> The S3 boards need `esp32s3-libs`. The `esp32c6-libs` set is only for the C6, so you
> can leave it alone here.
>
> **PSRAM frequency.** The custom `esp32s3-libs` are built to run PSRAM at **120 MHz**
> (DDR). In practice this is not a problem on the ESP32-S3 — across the S3 units
> tested here it has been stable every time. Nothing to configure — it is baked into
> these libs.

## 5. Apply the library patches

The firmware relies on a few modifications to LVGL, Arduino_GFX and the ESP32 core that
live outside the sketch. Run the apply script once. It dry-runs first and aborts without
changing anything if a patch would not apply cleanly (usually a version mismatch —
install the exact versions in steps 3–4), and it is safe to re-run.

On Windows (PowerShell — needs `git` on PATH, which the toolchain provides):
```powershell
cd .\patches
./apply_patches.ps1
```

On Linux (bash — needs `git`, e.g. `sudo apt install git`):
```bash
cd ./patches
./apply_patches.sh
```

> This is a full **PSRAM + dual-core S3**, so **all** the patches apply here — the LVGL
> core-pin (dual-core render), the PSRAM screen cache, the async-DMA flush, the
> PSRAM-size report, the BLE toggle-crash fix. Full per-patch table:
> [`patches/README.md`](../../patches/README.md).

After patching, clear the Arduino build cache so the patched libraries recompile:
- On Windows, delete the contents of `%LOCALAPPDATA%\arduino\sketches\`
- On Linux, delete the contents of `~/.cache/arduino/sketches/`

## 6. Select the board & partition table

**a) Board.** Open `OpenWatchFace/board.h` and set:

```c
#define BOARD_SELECT  BOARD_ID_S3_169
```

Everything else — pin map, drivers, feature flags, screen geometry — follows from
that one line.

**b) Partition table.** Copy the S3's 16 MB table over `partitions.csv` in the
`OpenWatchFace/` folder.

On Windows (PowerShell):
```powershell
Copy-Item -Force ".\OpenWatchFace\partitions_s3_16mb.csv" ".\OpenWatchFace\partitions.csv"
```

On Linux (bash):
```bash
cp -f ./OpenWatchFace/partitions_s3_16mb.csv ./OpenWatchFace/partitions.csv
```

> `board.h` **and** the partition file must match. The custom partition is required:
> `app0` must stay pinned at `0x10000` or the watch boots to a black screen.

## 7. Board settings

| Setting | Value |
|---|---|
| Board | **ESP32S3 Dev Module** |
| Erase All Flash Before Sketch Upload | **Enabled** (first flash only) |
| Events Run On | **Core 0** |
| Arduino Runs On | **Core 1** |
| Flash Mode | **QIO 120MHz** |
| Flash Size | **16MB** |
| Partition Scheme | **Custom** (uses `partitions.csv`) |
| **PSRAM** | **OPI PSRAM** ← required, see below |

> ### ⚠️ PSRAM must be enabled in the build
> The firmware puts the LVGL buffers, screen cache and stores in PSRAM. If PSRAM is
> **Disabled** while `board.h` declares `BOARD_HAS_PSRAM 1`, those allocations fail
> during early init — **before serial output works** — and the watch sits in a
> **silent boot loop**: a garbled/truncated panic, no readable error, and a `Saved PC`
> that changes every reset. It looks exactly like a firmware bug, but no source change
> fixes it. **If you hit that symptom, check this setting first.**

## 8. Build & flash

Open `OpenWatchFace/OpenWatchFace.ino`, then **Verify** (compile) and **Upload**.

- **Upload trouble?** Hold **BOOT**, tap **RST**, release **BOOT** to enter download
  mode, then upload again.
- **First-time clock set:** nothing to do — the PCF85063 keeps time once set (WiFi
  sync or BLE), even across a full power-off.
- **No microSD on this board:** photos/files features that want a card fall back to
  the on-flash FFat partition automatically.

---

## Optional settings you can tune

All of these are compile-time and live in the sketch folder.

### Board header — `OpenWatchFace/board_ws_s3_touch_lcd_169.h`

| Setting | Default | What it does |
|---|---|---|
| `BOARD_169_HW_V1` | `0` | Hardware revision pin set — `0` = V2.1 (current stock), `1` = original V1. See step 0. |
| `BOARD_PARTIAL_BUF_LINES` | `90` | Lines per LVGL render buffer (×2, internal SRAM). Each costs `240 × lines × 2` bytes. Fixed at compile time — never auto-size it. |
| `BOARD_LCD_BUS_HZ` | `80000000` | Display SPI clock (LCD-2-proven for this controller family). Drop to `40000000` if you ever see tearing or garbage. |
| `BOARD_BATT_CAL_NUM/DEN` | `1 / 1` | Per-unit battery divider trim. Measure the cell at rest and set these if the reported voltage reads off. |

### Extra

| Setting | File | Notes |
|---|---|---|
| `OVERCLOCK_ENABLE` | `overclock.h` | Past 240 MHz on the S3. **Can hang or scramble flash/NVS** — read the header's recovery notes first. |
| `UNDERVOLT_ENABLE` | `settings_store.h` | Core-rail undervolt. **Disabled by default** — a no-op until you opt in and edit it. |
| `CORE_UV_MV[]` | `core_voltage.h` | Real core (dig_dbias) undervolt. Default 1150 mV = stock. Also off unless you edit it. |
| `BATT_DESIGN_MAH` | `power_model.h` | Battery design capacity (mAh). Set it to match your cell for better battery-health data. |

---

## Device-specific notes

- **The panel is a 240×280 window into the ST7789's 240×320 RAM** — the glass sits at
  a 20-row offset, taken verbatim from the vendor demos. Same `Arduino_ST7789` class
  and complete library init as the LCD-2; no vendor register table. Because it is
  240 wide it automatically lands in the same UI tiers as the LCD-2 (watch-face
  dial glyph, percentage app-screen widths, full 3×3 launcher grid), and the
  automatic font scaling (`UI_FONT` in `ui_fonts.h`) sizes text by the *tighter*
  of the width/height ratios — so this 280-tall panel renders some roles one
  Montserrat step smaller than the 320-tall LCD-2 instead of inheriting fonts
  that crowd its shorter layout.
- **Touch is polled, but there IS a reset line.** `TP_INT` stays `-1` (the in-tree
  CST816 driver has no ISR path; the physical INT wire sits unused on GPIO14), but
  unlike the LCD-2 this board breaks out `TP_RESET` (GPIO13) — the firmware pulses it
  before the probe and holds the controller in reset through deep sleep, the same
  quiescing the C6-1.47 does.
- **The soft power latch is why the PWR button behaves like a phone's.** Battery
  power flows through a P-FET that the PWR key only forces on *while held*; the
  firmware asserts `SYS_EN` first thing in `setup()` and holds it through deep sleep.
  **Power off** in the Power app drops the latch — a true off (µA-scale, PWR-key
  press to revive) rather than the RST-only deep sleep of the other PMU-less boards.
  On USB the 5 V rail bypasses the latch, so "off" behaves like the usual no-wake
  deep sleep until you unplug.
- **The buzzer is a beeper, not a speaker.** The PWM backend plays the alarm/timer
  melodies and notification dings as monophonic square-wave tones (loudest-note
  flattening for the polyphonic chimes). There is no volume control — it is a
  transistor-driven magnetic buzzer.
- **Battery ADC is on ADC1.** GPIO1 = `ADC1_CH0`, so battery reads never collide with
  the WiFi driver. Divider is 200k/100k (×3), same as the LCD-2.
- **PCF85063 RTC on the shared I²C bus.** Same part and driver as the S3-2.06 /
  S3-1.8 / T5 — time survives a full power-off. Its INT output is wired but unused.
- **No ULP deep-sleep step counting.** The S3 ULP step blob is baked for the
  2.06's GPIO14/15 I²C — this board's IMU sits on GPIO10/11 (and 14/15 are the touch
  INT / backlight). Steps count while awake, as on the LCD-2.
- **BOOT wakes deep sleep directly.** GPIO0 is RTC-capable on the S3, so a BOOT press
  wakes the watch from deep sleep with no hardware mod. The PWR key's sense line
  (SYS_OUT) is *not* wake-capable and is currently unused by the firmware.
