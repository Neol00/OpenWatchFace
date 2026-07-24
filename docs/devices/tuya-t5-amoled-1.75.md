# Waveshare T5-E1-Touch-AMOLED-1.75 — install guide

| | |
|---|---|
| MCU | Tuya **T5-E1** (BK7258) — **16 MB PSRAM** |
| Display | CO5300 AMOLED 466×466, QSPI (via the TuyaOpen display layer) |
| Touch | CST92xx capacitive |
| RTC | PCF85063 (I²C, battery-backed) |
| IMU | QMI8658 (I²C) |
| Power | **No PMU** — battery sensed over an ADC divider (ETA6098 charger) |
| Audio | Internal codec via `tdl_audio` (amp/codec gated off between sounds) |
| Storage | microSD via the TuyaOpen filesystem (`tkl_fs`) |
| Haptics | Optional — driver GPIO wired (IO14), but **no motor fitted by default** (see below) |
| BLE | Tuya's own NimBLE inside `libtuyaos.a` — **not** the ESP32 NimBLE path |

> **This is not an ESP32 / Arduino-ESP32 build.** The T5-E1 is a Tuya BK7258 chip, so it
> builds against the **TuyaOpen** Arduino core instead of the Espressif ESP32 core. You
> still drive it from the **Arduino IDE** — the difference is the board package
> underneath. Everything about installing the board and flashing it differs from the
> ESP32 pages, but the sketch and the **custom LVGL 9.5 libraries are the same** (the T5
> runs our own LVGL, not the vendor's v8). Read this page start-to-finish rather than
> assuming the ESP32 steps carry over.

---

## 1. Install the Arduino IDE

Download and install the Arduino IDE from
<https://www.arduino.cc/en/software/>.

Launch it once so it creates your sketchbook (libraries) folder.

## 2. Install the TuyaOpen core (instead of the ESP32 core)

The T5 does **not** use the Espressif ESP32 core. Instead:

1. In the Arduino IDE, open **File → Preferences**.
2. In **Additional boards manager URLs**, add this line:
   ```
   https://github.com/tuya/arduino-tuyaopen/releases/download/global/package_tuya_open_index.json
   ```
3. Click **OK**, then open the **Board Manager** (Tools → Board → Boards Manager).
   **Tuya Open** now appears in the list.
4. Install **version 1.2.5** specifically.

> Use exactly **1.2.5** — the paths and the prebuilt library below are matched to that
> package version (the `vendor-T5/0.0.9` tools tree it ships).

## 3. Place the bundled libraries (custom LVGL 9.5)

Place the provided libraries from this repo into the Arduino IDE libraries directory.
The location varies depending on what OS you are running.

On Windows the default location for the libraries is:
`C:\Users\yourusername\Documents\Arduino\libraries`

On Linux the default location for the libraries is:
`/home/yourusername/Arduino/libraries`

Copy the **contents** of this repo's `libraries/` folder in, so the library folders and
`lv_conf.h` land directly inside it. `lv_conf.h` must sit **next to** the `lvgl` folder,
not inside it — LVGL requires that.

> **This step is required, not optional, on the T5.** The firmware runs **our own
> LVGL 9.5** — the same version the ESP boards use — not the LVGL v8 the TuyaOpen SDK
> ships behind its `lv_vendor_*` layer. The vendor v8 mis-renders the UI, and the
> package patch in step 5 even removes the vendor-LVGL include paths so only our copy is
> seen. Install these libraries.

## 4. Replace `libtuyaos.a` with the repo's build

The TuyaOpen package ships a stock `libtuyaos.a`. This firmware needs a **modified**
build of it — the BLE / NimBLE fixes that make pairing and iOS ANCS work live inside this
library. Replace the installed one with the copy from this repo.

**Source** — in this repo, at:
```
tuya_open/tools/vendor-T5/0.0.9/libs/libtuyaos.a
```

**Destination** — the installed package. On Windows:
```
C:\Users\yourusername\AppData\Local\Arduino15\packages\tuya_open\tools\vendor-T5\0.0.9\libs\libtuyaos.a
```
On Linux:
```
~/.arduino15/packages/tuya_open/tools/vendor-T5/0.0.9/libs/libtuyaos.a
```

Overwrite the file at the destination with the one from the repo.

> Keep a backup of the original if you like, but the repo's build is the one the firmware
> is written against — the stock library will pair-fail with iOS and mis-handle
> notifications.

## 5. Apply the tuya_open package patches

Two small edits to the **installed tuya_open package** are required. They are separate
from `libtuyaos.a`, and a package install/update silently reverts them:

1. The Arduino main-thread stack is raised from 4 KB to 32 KB — 4 KB overflows into a
   silent panic when the menu / quick-shade screens build (LVGL software render + deep
   flex layout run on that thread).
2. The vendor-LVGL include paths are removed, so the SDK's LVGL v8 headers can't collide
   with our LVGL 9.5 from step 3.

Run the script once. It auto-detects the package directory, is idempotent (safe to
re-run), and reports what it changed:

```bash
cd ./patches
./apply_tuya_package_patches.sh
```

(On Windows run it from Git-Bash or WSL.)

> Re-run this after **any** install or update of the tuya_open package — an update wipes
> both edits. The QSPI display-window fix the firmware also relies on is already baked
> into the repo's `libtuyaos.a` from step 4, so there is nothing extra to apply for it.

## 6. Select the board in `board.h`

Open `OpenWatchFace/board.h` and set:

```c
#define BOARD_SELECT  BOARD_ID_TUYA_T5
```

Everything else — pins, drivers, feature flags, screen geometry — follows from that one
line. Unlike the ESP32 boards there is **no partition CSV to copy**: the T5's flash
layout is owned by the TuyaOpen package, and this board has no on-flash FAT partition
(its persistence uses Tuya's KV store), so the "Flash" volume simply does not appear.

## 7. Board settings

In the Arduino IDE:

| Setting | Value |
|---|---|
| Board | **Tuya Open → TUYA_T5AI_BOARD** |
| Programmer | **tyutool** (Tools → Programmer) |

> The **Programmer must be set to `tyutool`** — that is the TuyaOpen flashing tool the
> upload uses. Leave the other Tuya Open board options at their defaults.

## 8. Build & flash

Open `OpenWatchFace/OpenWatchFace.ino`, then **Verify** (compile) and **Upload** from the
Arduino IDE, exactly like the ESP boards — the difference is entirely in the toolchain
underneath (the TuyaOpen core + `tyutool`), not in how you drive the IDE.

- **First-time clock set:** connect to a WiFi network to sync the clock automatically,
  or set `FORCE_TIME_SET` to 1 with your current time and flash once.
- **microSD recommended:** insert a FAT32 card for notification history, WiFi
  credentials, and battery-health logging. On the T5 the card is reached through the
  TuyaOpen filesystem (`tkl_fs`), not the ESP SD library.

---

## Optional settings you can tune

All of these are compile-time and live in the sketch folder.

### Board header — `OpenWatchFace/board_tuya_t5_amoled_175.h`

| Setting | Default | What it does |
|---|---|---|
| `HAPTICS_CLICK_MS` | `28` | Button-tick length — the **real** strength knob (buzz is length-only). Lower if too hard, raise if too faint; floor ~20 ms or a coin ERM never spins up. Only matters if you fit a motor (see below). |
| `HAPTICS_INTENSITY_PCT` | `40` | **Reserved / not wired up.** IO14 has no hardware PWM on the T5 and the software-PWM attempt was reverted, so the motor runs at full strength regardless. Left in place for when amplitude control is redone; tune `HAPTICS_CLICK_MS` instead. |

### CPU & PSRAM clock / voltage (T5-specific)

The ESP32 experimental knobs (`overclock.h`, `settings_store.h` rail undervolt,
`core_voltage.h`) are **ESP32-only** and do nothing on the T5 — it is different silicon
with no PMU rail to trim. Instead, the T5 has its own two files that set clock and
voltage directly:

| File (`OpenWatchFace/tuya/compat/`) | What it controls |
|---|---|
| `owf_tuya_cpu_freq.h` | **CPU** clock speed and its matched core voltage (mV), set manually via a DVFS table. Use it to **undervolt** (lower the core rail at a given clock) or **overclock** (raise the clock, with the voltage its rail needs). |
| `owf_tuya_psram_freq.h` | **PSRAM** clock speed and LDO voltage. Use it to **overclock the PSRAM** (the biggest render-speed lever, since the LVGL draw buffers live there) and to set the voltage that rate needs. |

> **These are hands-on, hardware-risk knobs — edit the values in the files themselves.**
> Voltage is always applied *before* a speed-up and *after* a slow-down, so the core /
> PSRAM is never fast on a low rail; keep that pairing if you change the tables. On the
> T5 the **whole system heap is in PSRAM**, so a bad PSRAM setting corrupts everything
> and is recoverable only by reflashing. Verify the real clock and stress-test under
> load before trusting a new operating point. Each file's header comment documents the
> mechanism and the safe sequence.

---

## Device-specific notes

- **Different SoC and toolchain.** The T5-E1 is a Tuya BK7258, built with the TuyaOpen
  Arduino core and flashed with `tyutool`. None of the ESP32 core, `esp32s3-libs`, or
  ESP32 patch steps apply here.
- **Our LVGL, not the vendor's.** The firmware runs LVGL 9.5 from the sketch library and
  its own display bring-up (`owf_tuya_lvgl_own.h`), replacing the SDK's LVGL v8 — which
  is why steps 3 and 5 matter.
- **BLE lives in `libtuyaos.a`.** Pairing and iOS ANCS depend on fixes compiled *into*
  the library, so the repo's build must replace the stock one (step 4). The ESP32
  NimBLE / `BLEDevice` path is not used (`BOARD_HAS_BLE 0` here).
- **No PMU.** Battery is sensed on an ADC divider (BAT_ADC, via `tkl_adc`), not an
  AXP2101 — so the PMU-based deep-sleep rail gating does not apply; the T5 sleeps via its
  own SDK path.
- **Haptics are a wiring mod, not built in** (same as the S3-1.47) — see below.

---

### Optional: add a haptic motor

The stock T5 board has **no vibration motor**, but the firmware's haptics are set up
for one (enabled by default, `BOARD_HAS_HAPTICS 1`) — you just wire the motor. The
driver GPIO is **IO14**, which is electrically free and sits right next to a GND pad, so
a 2-pin header (IO14 + GND) lets a vibrator plug in and out. This is the **same mod** as
the S3-1.47.

The design is a coin ERM motor switched low-side by a **2N3904 NPN transistor**:

- **IO14 → transistor base**, through a **1K resistor** (see the resistor note below).
- **Transistor collector → motor −**; **motor + → your 3V3 / battery rail**.
- **Transistor emitter → GND**.

It is active HIGH: IO14 HIGH turns the transistor on and the motor runs. IO14 is
electrically free and sits next to a GND pad, so a 2-pin header (IO14 + GND) lets the
vibrator plug in and out.

> **The 1K base resistor is optional but recommended.** You *can* drive the base straight
> from IO14 with no resistor — it will not damage anything — but the motor then gets the
> full drive and the haptic feedback feels **too aggressive**. The 1K in series tames it
> to a pleasant tap. If you skip it and it is too strong, either add the resistor or lower
> `HAPTICS_CLICK_MS` in the board header.

There is no working amplitude control on this board (see the `HAPTICS_INTENSITY_PCT` note
above), so the motor runs at full strength; **`HAPTICS_CLICK_MS`** is the knob for the
feel — lower if too hard, raise if too faint, floor ~20 ms.
