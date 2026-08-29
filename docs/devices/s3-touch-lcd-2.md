# Waveshare ESP32-S3-Touch-LCD-2 — install guide

| | |
|---|---|
| MCU | ESP32-S3 — dual-core, **8 MB PSRAM**, 16 MB flash |
| Display | ST7789T3 LCD 240×320, SPI (mid-narrow UI layout), PWM backlight |
| Touch | CST816D capacitive (I²C, **polled** — no INT line) |
| RTC | **On-chip** — no dedicated RTC chip on this board |
| IMU | QMI8658 (steps + sleep tracking) |
| Power | **No PMU** — ETA6096 charger, battery sensed over an ADC divider (GPIO5) |
| Storage | microSD over **SPI, sharing the display bus** (CS GPIO41) |
| Camera | 24-pin DVP header — **OV5640 fitted on this unit** (enables the Camera app) |
| Haptics | **None** — no motor fitted and none wired by default |

> **This is the first board in the firmware with a camera.** A DVP sensor on the
> header turns on `BOARD_HAS_CAMERA`, which adds a **Camera** app (capture + gallery)
> to the app menu. On every other board that app and its tile do not exist at all.

---

## 1. Install the Arduino IDE

Download and install the Arduino IDE from
<https://www.arduino.cc/en/software/>.

Launch it once so it creates your sketchbook (libraries) folder.

## 2. Install the ESP32 core (Espressif Systems)

Open the Board Manager in the Arduino IDE, search for `esp32`, and install it. Make
sure the version you install is the one provided by **Espressif Systems** — not the
legacy "Arduino ESP32 Boards" entry.

> The camera driver (`esp_camera.h`) and the JPEG decoder the gallery uses both ship
> **inside the ESP32 core's precompiled S3 libs** — there is no extra camera library
> to install. Selecting an ESP32-S3 board is what pulls them in.

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

> **No touch library to install.** Unlike the AXS5106L boards, the CST816D driver is
> in-tree (`OpenWatchFace/touch_cst816.h`) — nothing to copy.

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
> S3 units tested here it has been stable every time. Nothing to configure — it is
> baked into these libs.

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
#define BOARD_SELECT  BOARD_ID_S3_LCD2
```

Everything else — pin map, drivers, feature flags, screen geometry, and whether the
Camera app exists — follows from that one line.

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
> The firmware puts the LVGL buffers, screen cache and stores in PSRAM — and on this
> board the **camera framebuffers live there too**, so it is doubly required. If PSRAM
> is **Disabled** while `board.h` declares `BOARD_HAS_PSRAM 1`, those allocations fail
> during early init — **before serial output works** — and the watch sits in a
> **silent boot loop**: a garbled/truncated panic, no readable error, and a `Saved PC`
> that changes every reset. It looks exactly like a firmware bug, but no source change
> fixes it. **If you hit that symptom, check this setting first.**

## 8. Build & flash

Open `OpenWatchFace/OpenWatchFace.ino`, then **Verify** (compile) and **Upload**.

- **Upload trouble?** Hold **BOOT**, tap **RST**, release **BOOT** to enter download
  mode, then upload again.
- **First-time clock set:** connect to a WiFi network to sync the clock automatically,
  or set `FORCE_TIME_SET` to 1 with your current time and flash once. (This board keeps
  time on the S3's on-chip RTC — it does not survive a full power loss the way the
  PCF85063 boards do, so WiFi/BLE time sync matters more here.)
- **microSD strongly recommended on this board:** photos are large. With a card
  inserted they go to the card; without one they land on the small on-flash FFat
  partition and will fill it quickly. Format the card FAT32.

---

## Optional settings you can tune

All of these are compile-time and live in the sketch folder.

### Board header — `OpenWatchFace/board_ws_s3_touch_lcd_2.h`

| Setting | Default | What it does |
|---|---|---|
| `BOARD_PARTIAL_BUF_LINES` | `64` | Lines per LVGL render buffer (×2, internal SRAM). Each costs `240 × lines × 2` bytes. Higher = fewer, larger flushes; costs SRAM. Fixed at compile time — never auto-size it. |
| `BOARD_LCD_BUS_HZ` | `80000000` | Display SPI clock. Drop it if you ever see tearing or garbage. |
| `BOARD_BATT_CAL_NUM/DEN` | `1 / 1` | Per-unit battery divider trim. Measure the cell at rest and set these if the reported voltage reads off. |
| `CAM_STILL_FRAMESIZE` | `FRAMESIZE_UXGA` | Photo resolution (in `camera_dev.h`). Lower it for smaller files. |
| `CAM_STILL_QUALITY` | `10` | JPEG quality, **lower = better** on this sensor family. Raise it for smaller files. |

### Extra

| Setting | File | Notes |
|---|---|---|
| `OVERCLOCK_ENABLE` | `overclock.h` | Past 240 MHz on the S3. **Can hang or scramble flash/NVS** — read the header's recovery notes first. |
| `UNDERVOLT_ENABLE` | `settings_store.h` | Core-rail undervolt. **Disabled by default** — a no-op until you opt in and edit it. |
| `CORE_UV_MV[]` | `core_voltage.h` | Real core (dig_dbias) undervolt. Default 1150 mV = stock. Also off unless you edit it. |
| `BATT_DESIGN_MAH` | `power_model.h` | Battery design capacity (mAh). Set it to match your cell for better battery-health data. |

---

## Device-specific notes

- **The panel is a real ST7789, not a JD9853.** Both are driven through Arduino_GFX's
  `Arduino_ST7789` class, but this one needs **no vendor register table** after
  `begin()` — the library's init is complete. It is also a full 240×320 with **zero**
  window offsets, unlike the 172-wide boards and their 34-px column fudge.
- **Touch is polled.** The CST816D's INT line is not broken out, so `TP_INT` is `-1`
  and the driver is polled on every LVGL indev tick (exactly what the vendor demo
  does). There is no panel reset line either, so a wedged controller is recovered over
  I²C rather than with a reset pulse.
- **The microSD shares the display's SPI bus.** Like the C6-1.47 (and unlike the
  S3-1.47, which has a dedicated SDMMC bus), the card is a second device on the LCD's
  SPI host, so the display↔SD arbitration is active. Unlike the C6, the card rail is
  plain always-on — there is no LDO channel to power up first.
- **Battery ADC is on ADC1.** GPIO5 is `ADC1_CH4`, so battery reads never collide with
  the WiFi driver (the S3-1.47's GPIO12 sits on ADC2 and does).
- **No haptics, no PMU, no RTC chip, no audio.** This is a lean board — do not assume
  the peripheral set of the AMOLED models.
- **PWM backlight.** Brightness is a PWM duty on `LCD_BL` (GPIO1), not a panel command
  like the AMOLED boards.
- **BOOT wakes deep sleep directly.** GPIO0 is RTC-capable on the S3, so a BOOT press
  wakes the watch from deep sleep with no hardware mod.

---

## The Camera app

The **Camera** tile appears in the app menu **only because this board declares
`BOARD_HAS_CAMERA`** — on every other supported device the app is compiled out
entirely, tile included.

It has two tabs, switched by the buttons at the top:

- **Camera** — a live preview with a **Take Photo** button. Each shot is written as a
  full-resolution JPEG to `/DCIM/IMG_<n>.JPG` on the **microSD if one is mounted**,
  otherwise on the on-flash FFat partition (the same "SD if present, else flash" rule
  the rest of the firmware follows, so photos also show up in the **Files** app).
- **Gallery** — the photos already taken, **newest first**, one at a time with
  prev/next arrows, a `name  n / total` caption, and a **delete** button that asks for
  a second tap to confirm.

Notes on how it behaves:

- **The sensor is only powered while the app is open.** Leaving the app stops the
  preview, deinitialises the driver and frees its PSRAM, so an idle watch pays nothing
  for having a camera.
- **There is a brief shutter delay.** The preview runs in RGB565 (so LVGL can blit it
  with no decode) while a photo is captured in JPEG at much higher resolution. The
  driver's pixel format is fixed at init, so taking a photo re-initialises the sensor
  into JPEG mode and back — roughly a third of a second. The first frame after the
  switch is discarded so the saved image is not the under-exposed one the sensor
  produces while auto-exposure is still settling.
- **Photos are large.** At the default UXGA/quality-10 settings expect a few hundred KB
  each. Use a microSD; on flash-only storage you will run out quickly. Lower
  `CAM_STILL_FRAMESIZE` or raise `CAM_STILL_QUALITY` in `camera_dev.h` for smaller
  files.
- **If the header is empty or the ribbon is loose**, the app still opens but reports
  *"Camera not detected"* and disables the shutter — it does not hang or crash. The
  gallery keeps working, since it only reads files.
