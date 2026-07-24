# Waveshare ESP32-S3-Touch-LCD-1.47 — install guide

| | |
|---|---|
| MCU | ESP32-S3 — dual-core, **8 MB PSRAM**, 16 MB flash |
| Display | JD9853 LCD 172×320, SPI (narrow-screen UI layout), PWM backlight |
| Touch | AXS5106L capacitive (I²C) |
| RTC | **On-chip** — no dedicated RTC chip on this board |
| IMU | **None** — no QMI8658 on this variant |
| Power | **No PMU** — battery sensed over an ADC divider (GPIO12) |
| Storage | microSD over SDMMC (4-bit) |
| Haptics | Optional — motor GPIO wired but **no motor fitted by default** (see below) |

> **This board is the C6-1.47's twin with an S3 SoC.** Same 172×320 JD9853 display,
> same AXS5106L touch, same charge circuit. The differences are the SoC: this one is
> **dual-core with 8 MB PSRAM**, so it follows the full S3 build flow (PSRAM enabled,
> `esp32s3-libs`, all patches). It also has **no IMU**, so there is no step counter or
> sleep tracking, and none of the C6's deep-sleep step-counting hardware mods apply.

---

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
> (DDR). In practice this is not a problem on the ESP32-S3 — across four different
> S3 units tested here it has been stable every time; I have yet to find one that
> can't handle it. Nothing to configure — it is baked into these libs.

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

> Unlike the C6, this is a full **PSRAM + dual-core S3**, so **all** the patches apply
> here — the LVGL core-pin (dual-core render), the PSRAM screen cache, the async-DMA
> flush, the PSRAM-size report, the BLE toggle-crash fix. The
> script handles them all; you do not need to pick and choose. Full per-patch table:
> [`patches/README.md`](../../patches/README.md).

After patching, clear the Arduino build cache so the patched libraries recompile:
- On Windows, delete the contents of `%LOCALAPPDATA%\arduino\sketches\`
- On Linux, delete the contents of `~/.cache/arduino/sketches/`

## 6. Select the board & partition table

**a) Board.** Open `OpenWatchFace/board.h` and set:

```c
#define BOARD_SELECT  BOARD_ID_S3_147
```

Everything else — pin map, drivers, feature flags, screen geometry — follows from that
one line.

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
| Board | **ESP32-S3-LCD-Touch-1.47** |
| Erase All Flash Before Sketch Upload | **Enabled** (first flash only) |
| Events Run On | **Core 0** |
| Arduino Runs On | **Core 1** |
| Flash Mode | **QIO 120MHz** |
| Flash Size | **16MB** |
| Partition Scheme | **Custom** (uses `partitions.csv`) |
| **PSRAM** | **Enabled** ← required, see below |

> ### ⚠️ PSRAM must be enabled in the build
> The firmware puts the LVGL buffers, screen cache and stores in PSRAM. If `PSRAM` is
> **Disabled** while `board.h` declares `BOARD_HAS_PSRAM 1`, those allocations fail
> during early init — **before serial output works** — and the watch sits in a
> **silent boot loop**: a garbled/truncated panic, no readable error, and a `Saved PC`
> that changes every reset. It looks exactly like a firmware bug, but no source change
> fixes it. **If you hit that symptom, check this setting first** — an IDE update or a
> settings reset can silently clear it.

## 8. Build & flash

Open `OpenWatchFace/OpenWatchFace.ino`, then **Verify** (compile) and **Upload**.

- **Upload trouble?** Hold **BOOT**, tap **RST**, release **BOOT** to enter download
  mode, then upload again.
- **First-time clock set:** connect to a WiFi network to sync the clock automatically,
  or set `FORCE_TIME_SET` to 1 with your current time and flash once. (This board keeps
  time on the S3's on-chip RTC — it does not survive a full power loss the way the
  PCF85063 boards do, so WiFi/BLE time sync matters more here.)
- **microSD recommended:** format a card FAT32 and insert it for notification history,
  WiFi credentials, and battery-health logging. Without one the watch falls back to
  on-flash storage.

---

## Optional settings you can tune

All of these are compile-time and live in the sketch folder.

### Board header — `OpenWatchFace/board_ws_s3_touch_lcd_147.h`

| Setting | Default | What it does |
|---|---|---|
| `BOARD_PARTIAL_BUF_LINES` | `90` | Lines per LVGL render buffer (×2, internal SRAM). Higher = fewer, larger flushes; costs SRAM. Fixed at compile time — never auto-size it. |
| `BOARD_LCD_BUS_HZ` | `80000000` | Display SPI clock. Drop it if you ever see tearing or garbage. |
| `BOARD_PWR_CPU_K` | `0.00130` | Awake power model, CPU term. An **estimate** (single core ≈ half the S3's); refine against a meter — this board's ADC battery header lets you measure. |
| `BOARD_HAS_HAPTICS` | `1` | Haptic feedback. Enabled, but there is no motor fitted by default — see the note below to add one. |
| `HAPTICS_CLICK_MS` | `28` | Button-tick length (strength is length-only). Lower if too hard, raise if too faint; floor ~20 ms or a coin ERM never spins up. |

### Experimental — off by default

| Setting | File | Notes |
|---|---|---|
| `OVERCLOCK_ENABLE` | `overclock.h` | Past 240 MHz on the S3. **Can hang or scramble flash/NVS** — read the header's recovery notes first. |
| `UNDERVOLT_ENABLE` | `settings_store.h` | Core-rail undervolt. **Disabled by default** — the shipped table is all-stock, so it is a no-op until you opt in and edit it. Validate each step against a meter before trusting it; see the [overclocking & undervolting](../../README.md#experimental-overclocking--undervolting) section. |
| `CORE_UV_MV[]` | `core_voltage.h` | Real core (dig_dbias) undervolt, separate from the rail one above. Default 1150 mV = stock. Also off unless you edit it. |

> The AXP2101 rail undervolt does **not** apply here — there is no PMU rail to trim, so
> that path is a no-op on the C6.

---

## Device-specific notes

- **Dual-core S3 with PSRAM.** Unlike the C6-1.47 twin, this board runs the full
  design: the screen cache and PSRAM paths are active, and rendering is split across
  both cores.
- **No IMU.** There is no QMI8658 on this variant, so no step counter and no sleep
  tracking. (The C6-1.47 has one; this board omits it.)
- **No RTC chip.** Timekeeping uses the S3's on-chip RTC, so time is not battery-backed
  the way it is on the PCF85063 boards — rely on WiFi/BLE sync after a power loss.
- **PWM backlight.** Brightness is a PWM duty on `LCD_BL` (GPIO46), not a panel command
  like the AMOLED boards.
- **BOOT wakes deep sleep directly.** GPIO0 is RTC-capable on the S3, so a BOOT press
  wakes the watch from deep sleep with no hardware mod (the C6 needs a bridging wire for
  this; the S3 does not).

---

### Optional: add a haptic motor

This board has **no vibration motor fitted from the factory**, but the firmware's
haptics are already set up for one — enabled by default (`BOARD_HAS_HAPTICS 1`), you
just need to wire the motor. The driver GPIO is **IO10**, which is electrically free on
this board (its net runs only to the P1 breakout header).

The design is a coin ERM motor switched low-side by a **2N3904 NPN transistor**:

- **IO10 → transistor base**, through a **1K resistor** (see the resistor note below).
- **Transistor collector → motor −**; **motor + → your battery/3V3 rail**.
- **Transistor emitter → GND**.

It is active HIGH: IO10 HIGH turns the transistor on and the motor runs. On a real
ESP32 like this one, the firmware also latches IO10 LOW through deep sleep, so the
motor stays quiet while the watch is asleep.

> **The 1K base resistor is optional but recommended.** You *can* drive the base
> straight from IO10 with no resistor — it will not damage anything — but the motor
> then gets the full drive and the haptic feedback feels **too aggressive**. The 1K in
> series tames it to a pleasant tap. If you skip it and it is too strong, either add the
> resistor or lower `HAPTICS_CLICK_MS` in the board header.
