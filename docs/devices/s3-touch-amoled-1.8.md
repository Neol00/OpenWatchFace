# Waveshare ESP32-S3-Touch-AMOLED-1.8 — install guide

| | |
|---|---|
| MCU | ESP32-S3R8 — dual-core LX7, **8 MB PSRAM**, 16 MB flash |
| Display | SH8601 AMOLED 368×448, QSPI @ 80 MHz |
| Touch | FT3168 capacitive (I²C) — **no reset line broken out** |
| RTC | PCF85063 (I²C, battery-backed) |
| IMU | QMI8658 (I²C) — INT not broken out; pedometer polls |
| PMU | AXP2101 (I²C) |
| Audio | ES8311 codec + amp |
| Storage | microSD over SDMMC (1-bit) |
| Haptics | **None** — no motor, no driver pads, not wired in software |

> **This board is a close sibling of the S3-2.06.** Same SoC, same I²C peripherals,
> same full **PSRAM + dual-core S3** build flow. The differences are the display
> (SH8601 instead of CO5300), the smaller 368×448 resolution, 16 MB flash instead of
> 32 MB, and **no haptics at all** (the 2.06 has motor pads; this board does not).
>
> **Hardware revision matters.** Waveshare ships **two** revisions under this name with
> **identical pin maps** but different display controllers: **V1 = SH8601**,
> **V2 = CO5300** (which also needs a column offset). This firmware targets **V1** by
> default — see [Hardware revision V2](#hardware-revision-v2) below for a V2 unit.

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

> This is a full **PSRAM + dual-core S3**, so **all** the patches apply here — the LVGL
> core-pin (dual-core render), the PSRAM screen cache, the async-DMA QSPI flush, the
> PSRAM-size report, the BLE toggle-crash fix, and the I²S speaker fix (this board has
> the ES8311 codec, so that one matters). The script handles them all; you do not need
> to pick and choose. Full per-patch table: [`patches/README.md`](../../patches/README.md).

After patching, clear the Arduino build cache so the patched libraries recompile:
- On Windows, delete the contents of `%LOCALAPPDATA%\arduino\sketches\`
- On Linux, delete the contents of `~/.cache/arduino/sketches/`

## 6. Select the board & partition table

**a) Board.** Open `OpenWatchFace/board.h` and set:

```c
#define BOARD_SELECT  BOARD_ID_S3_18
```

Everything else — pin map, drivers, feature flags, screen geometry — follows from that
one line.

**b) Partition table.** Copy the 16 MB table over `partitions.csv` in the
`OpenWatchFace/` folder.

On Windows (PowerShell):
```powershell
Copy-Item -Force ".\OpenWatchFace\partitions_s3_16mb.csv" ".\OpenWatchFace\partitions.csv"
```

On Linux (bash):
```bash
cp -f ./OpenWatchFace/partitions_s3_16mb.csv ./OpenWatchFace/partitions.csv
```

> `board.h` **and** the partition file must match. A mismatch (e.g. the 2.06's 32 MB
> table on this 16 MB flash) fails to flash or boots to a black screen.

## 7. Board settings

| Setting | Value |
|---|---|
| Board | **ESP32-S3-Touch-AMOLED-1.8** |
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
  or set `FORCE_TIME_SET` to 1 with your current time and flash once. This board has a
  battery-backed PCF85063 RTC, so it keeps perfect time across reboots, deep sleep and
  power-off once set.
- **microSD recommended:** format a card FAT32 and insert it for notification history,
  WiFi credentials, and battery-health logging. Without one the watch falls back to
  on-flash storage.

---

## Optional settings you can tune

All of these are compile-time and live in the sketch folder.

### Board header — `OpenWatchFace/board_ws_s3_touch_amoled_18.h`

| Setting | Default | What it does |
|---|---|---|
| `BOARD_PARTIAL_BUF_LINES` | `90` | Lines per LVGL render buffer (×2, internal SRAM). Higher = fewer, larger flushes; costs SRAM. Fixed at compile time — never auto-size it. |
| `BOARD_LCD_BUS_HZ` | `80000000` | QSPI display clock (panel default is 40 MHz). Drop it if you ever see tearing or garbage. |
| `BOARD_HAS_ULP_STEPS` | `1` | Deep-sleep step counting on the RISC-V ULP. Needs the custom libs from step 4. |

> **No haptics setting.** This board has no vibration motor, no driver circuit and no
> motor pads, so `BOARD_HAS_HAPTICS` is left off and all `haptics_*()` calls compile to
> no-ops. There is nothing to enable. (If you have somehow wired your own motor, the
> board header comment shows which macros to define — but out of the box there is no
> haptic output on this device.)

### Experimental — off by default

| Setting | File | Notes |
|---|---|---|
| `OVERCLOCK_ENABLE` | `overclock.h` | Past 240 MHz on the S3. **Can hang or scramble flash/NVS** — read the header's recovery notes first. |
| `UNDERVOLT_ENABLE` | `settings_store.h` | AXP2101 rail undervolt (DCDC1). **Disabled by default** — the shipped table is all-stock 3300 mV, so it is a no-op until you opt in and edit it. Validate each step against a meter; see the [overclocking & undervolting](../../README.md#experimental-overclocking--undervolting) section. |
| `CORE_UV_MV[]` | `core_voltage.h` | Real core (dig_dbias) undervolt, separate from the rail one above. Default 1150 mV = stock. Also off unless you edit it. |

> Both undervolt paths have a self-test safety net. This board has the AXP2101 PMU, so
> like the 2.06 it *can* use the rail undervolt — but leave it stock unless you are
> deliberately experimenting.

---

## Hardware revision V2

If your unit has a **CO5300** panel instead of the SH8601, edit
`OpenWatchFace/board_ws_s3_touch_amoled_18.h`:

```c
#define BOARD_DISPLAY_SH8601_QSPI 0
#define BOARD_DISPLAY_CO5300_QSPI 1
```

and set the column offset the V2 vendor example uses:

```c
#define LCD_COL_OFFSET1 16    /* V1/SH8601 uses 0 */
```

Everything else — pins, flags, partitions, build settings — is identical.

---

## Device-specific notes

- **Dual-core S3 with PSRAM.** The full design runs here: the screen cache and PSRAM
  paths are active, and rendering is split across both cores.
- **No haptics.** There is no vibration motor and no driver on this board, unlike the
  2.06 which has motor pads. Haptic feedback is simply absent.
- **No touch reset line.** GPIO9 is the I²S bit clock here, not a touch reset, so
  `TP_RESET` is deliberately left undefined and the touch code's reset paths compile
  out. Because a hibernated FT3168 can only be revived by a reset pulse, this board puts
  the touch panel into **MONITOR** mode before sleep instead of HIBERNATE — the
  trade-off is slightly higher sleep draw than the 2.06.
- **No panel reset line either.** `LCD_RESET` is intentionally *not* defined (a
  defined-but-negative pin macro would satisfy `#ifdef` guards and then feed −1 to a
  GPIO API — see the long comment in the board header).
- **Battery-backed RTC.** The PCF85063 keeps time across power loss.
- **ES8311 pins differ from the 2.06.** Handled by the board header; nothing to
  configure.
