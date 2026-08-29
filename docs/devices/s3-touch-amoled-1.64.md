# Waveshare ESP32-S3-Touch-AMOLED-1.64 install guide

| | |
|---|---|
| MCU | ESP32-S3R8 dual-core LX7, **8 MB PSRAM**, 16 MB flash |
| Display | SH8601 AMOLED 280×456, QSPI @ 80 MHz |
| Touch | FT3168 capacitive (I²C @ 0x38) **INT/RESET not identified**, driver runs polled |
| RTC | **None** SoC RTC + BLE/SNTP resync |
| IMU | QMI8658 (I²C) INT not used; pedometer polls |
| PMU | **None** ETA6098 charger, battery on an ADC divider |
| Audio | **None** no codec, no amp |
| Storage | microSD over SDMMC (1-bit) |
| Haptics | **None** no motor |

> **This board is a leaner build than the 1.8 or the 2.06.** It shares their SoC and
> their SH8601-class QSPI AMOLED path, but it drops the AXP2101 PMU, the PCF85063 RTC
> and the ES8311 audio chain. What that means in practice:
>
> - **Time is lost on a full power-off.** There is no battery-backed RTC chip. The SoC
>   RTC carries the clock across deep sleep, and the watch re-syncs from BLE or SNTP on
>   the next connect, the same arrangement as the S3-1.47.
> - **The watch is silent.** No codec and no amp, so alarms and chimes are visual only.
>   All audio calls compile to no-ops.
> - **Battery % comes from a voltage divider, not a fuel gauge.** Slightly coarser than
>   the PMU boards, and the battery-health capacity learner (which needs the PMU's
>   coulomb counter) is not available here.
>
> **Panel revision.** Waveshare has shipped panel-revision variants under a single
> product name before (the 1.8 exists as both SH8601 and CO5300). Every piece of vendor
> evidence for the 1.64 says **SH8601** the demo includes `esp_lcd_sh8601.h`, calls
> `esp_lcd_new_panel_sh8601()`, and depends on the `esp_lcd_sh8601` component so that
> is what this firmware targets. See [If your panel is a CO5300](#if-your-panel-is-a-co5300)
> if yours turns out to differ.

---

## 1. Install the Arduino IDE

Download and install the Arduino IDE from
<https://www.arduino.cc/en/software/>.

Launch it once so it creates your sketchbook (libraries) folder.

## 2. Install the ESP32 core (Espressif Systems)

Open the Board Manager in the Arduino IDE, search for `esp32`, and install it. Make
sure the version you install is the one provided by **Espressif Systems** not the
legacy "Arduino ESP32 Boards" entry.

## 3. Place the bundled libraries

Place the provided libraries from this repo into the Arduino IDE libraries directory.
The location varies depending on what OS you are running.

On Windows the default location for the libraries is:
`C:\Users\yourusername\Documents\Arduino\libraries`

On Linux the default location for the libraries is:
`~/Arduino/libraries`

Copy the **contents** of this repo's `libraries/` folder in, so the library folders
and `lv_conf.h` land directly inside it. `lv_conf.h` must sit **next to** the `lvgl`
folder, not inside it. If an older `lvgl` or `GFX_Library_for_Arduino` is already there, 
overwrite it; the firmware needs these exact versions (LVGL 9.5.0, Arduino_GFX 1.6.5).

## 4. Install the custom SoC libs

Replace the `esp32s3-libs` by first deleting the original directory then placing the
provided libs from this repo in the correct location. The location varies depending on
what OS you are running.

On Windows the location for the esp32 libs is:
`C:\Users\yourusername\AppData\Local\Arduino15\packages\esp32\tools`

On Linux the location for the esp32 libs is:
`~/.arduino15/packages/esp32/tools`

> The S3 boards need `esp32s3-libs`. The `esp32c6-libs` set is only for the C6, so you
> can leave it alone here.
>
> **PSRAM frequency.** The custom `esp32s3-libs` are built to run PSRAM at **120 MHz**
> (DDR). In practice this is not a problem on the ESP32-S3 across the S3 units tested
> here it has been stable every time. Nothing to configure, it is baked into these libs.

## 5. Apply the library patches

The firmware relies on a few modifications to LVGL, Arduino_GFX and the ESP32 core that
live outside the sketch. Run the apply script once. It dry-runs first and aborts without
changing anything if a patch would not apply cleanly (usually a version mismatch,
install the exact versions in steps 3–4), and it is safe to re-run.

On Windows (PowerShell needs `git` on PATH, which the toolchain provides):
```powershell
cd .\patches
./apply_patches.ps1
```

On Linux (bash needs `git`, e.g. `sudo apt install git`):
```bash
cd ./patches
./apply_patches.sh
```

> This is a full **PSRAM + dual-core S3**, so the LVGL core-pin (dual-core render), the
> PSRAM screen cache, the async-DMA QSPI flush, the PSRAM-size report and the BLE
> toggle-crash fix all apply. The **I²S speaker fix is irrelevant here** this board has
> no codec, but the script applies everything and the unused patch is harmless, so you
> do not need to pick and choose. Full per-patch table:
> [`patches/README.md`](../../patches/README.md).

After patching, clear the Arduino build cache so the patched libraries recompile:
- On Windows, delete the contents of `%LOCALAPPDATA%\arduino\sketches\`
- On Linux, delete the contents of `~/.cache/arduino/sketches/`

## 6. Select the board & partition table

**a) Board.** Open `OpenWatchFace/board.h` and set:

```c
#define BOARD_SELECT  BOARD_ID_S3_164
```

Everything else, pin map, drivers, feature flags, screen geometry follows from that
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
| Board | **ESP32-S3-Touch-AMOLED-1.64** |
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
> during early init **before serial output works** and the watch sits in a
> **silent boot loop**: a garbled/truncated panic, no readable error, and a `Saved PC`
> that changes every reset. It looks exactly like a firmware bug, but no source change
> fixes it. **If you hit that symptom, check this setting first** an IDE update or a
> settings reset can silently clear it.

## 8. Build & flash

Open `OpenWatchFace/OpenWatchFace.ino`, then **Verify** (compile) and **Upload**.

- **Upload trouble?** Hold **BOOT**, tap **RST**, release **BOOT** to enter download
  mode, then upload again.
- **First-time clock set:** connect to a WiFi network to sync the clock automatically,
  or set `FORCE_TIME_SET` to 1 with your current time and flash once. **Unlike the 1.8
  and 2.06, this board has no RTC chip** the clock survives deep sleep but is lost on
  a full power-off, and re-syncs on the next BLE/WiFi connect.
- **microSD recommended:** format a card FAT32 and insert it for notification history,
  WiFi credentials, and battery-health logging. Without one the watch falls back to
  on-flash storage.

---

## Optional settings you can tune

All of these are compile-time and live in the sketch folder.

### Board header `OpenWatchFace/board_ws_s3_touch_amoled_164.h`

| Setting | Default | What it does |
|---|---|---|
| `BOARD_PARTIAL_BUF_LINES` | `120` | Lines per LVGL render buffer (×2, internal SRAM). Higher = fewer, larger flushes; costs SRAM. Fixed at compile time, never auto-size it. |
| `BOARD_LCD_BUS_HZ` | `80000000` | QSPI display clock (panel default is 40 MHz). Drop it if you ever see tearing or garbage. |
| `TP_INT` | `-1` | Touch interrupt GPIO. `-1` = none, so the FT3168 is polled (see the note below). Set it if you trace the line. |
| `BOARD_BATT_CAL_NUM` / `_DEN` | `1` / `1` | Per-unit trim for the battery divider's resistor tolerance. Measure your cell and adjust if the reported voltage is off. |

> **No haptics, audio, ULP-steps or undervolt settings on this board.** There is no
> motor and no codec, so those calls compile to no-ops. ULP deep-sleep step counting is
> off because the ULP blob has the 2.06's I²C pins baked in and this board's IMU is on a
> different pair (steps still count normally while awake). The AXP2101 rail undervolt
> does not apply either there is no PMU.

### Extra

| Setting | File | Notes |
|---|---|---|
| `OVERCLOCK_ENABLE` | `overclock.h` | Past 240 MHz on the S3. **Can hang or scramble flash/NVS** read the header's recovery notes first. |
| `CORE_UV_MV[]` | `core_voltage.h` | Real core (dig_dbias) undervolt. Default 1150 mV = stock. Off unless you edit it. |
| `BATT_DESIGN_MAH` | `power_model.h` | Default battery design capacity (mAh). Set it to match your cell for better battery-health data. |

---

## If your panel is a CO5300

The vendor demo is unambiguous that this board is SH8601, but if a future revision
ships a CO5300, edit `OpenWatchFace/board_ws_s3_touch_amoled_164.h`:

```c
#define BOARD_DISPLAY_SH8601_QSPI 0
#define BOARD_DISPLAY_CO5300_QSPI 1
```

and re-derive `LCD_COL_OFFSET1` from that revision's own vendor example rather than
reusing the SH8601 value of 20 the offset is panel-specific (the 2.06's CO5300 uses
22). The CO5300 also needs `BOARD_LCD_EVEN_ALIGN 1`. Everything else, pins, flags,
partitions, build settings is identical, because both panels share one QSPI-AMOLED
code path.

---

## Device-specific notes

- **Dual-core S3 with PSRAM.** The full design runs here: the screen cache and PSRAM
  paths are active, and rendering is split across both cores.
- **Touch is polled, not interrupt-driven.** The vendor demo polls the FT3168 over I²C
  and never references an INT or RESET GPIO, so neither pin could be taken from vendor
  source and neither is guessed. `TP_INT -1` is the DriveBus library's documented "no
  pin" value, which skips `attachInterrupt` and polls exactly what the vendor demo
  does. One consequence: the ISR-driven activity flag never fires, so a very quick tap
  can occasionally be missed by the idle-sleep timeout. If you trace the INT line on
  your unit, set `TP_INT` to its GPIO and that goes away.
- **No touch reset line defined**, so `board_sleep.h` monitors the touch controller
  through sleep instead of hibernating it, the same path the S3-1.8 takes.
- **Battery ADC is on ADC1, which is a real advantage.** The divider lands on GPIO4
  (ADC1_CH3). On the S3-1.47 the equivalent divider is on GPIO12 = **ADC2**, a unit the
  WiFi driver takes over while the radio is up, forcing that board to reuse its last
  good reading when a sample collides. This board has no such contention, battery
  samples always read live.
- **IMU address.** The vendor demo probes both 0x6A and 0x6B and uses whichever answers,
  so it does not pin down this board's strap. The header uses **0x6B** (the part's
  default with SA0 high, and what every other board here uses). If the IMU does not come
  up on your unit, try 0x6A.
