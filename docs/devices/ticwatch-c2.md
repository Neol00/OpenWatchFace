# Mobvoi TicWatch C2 / C2+ (skipjack) install guide

| | |
|---|---|
| SoC | Qualcomm **APQ8009W** (Snapdragon Wear 2100) single Cortex-A7, run bare-metal in AArch32 |
| Display | 360×360 round EDO AMOLED, MSM DSI **command mode** (MDP3 DMA_P pipe takeover) |
| Touch | FocalTech FTS (I²C, BLSP1 QUP5 @ `0x38`) |
| Buttons | **One** the single side pusher (TLMM GPIO 91). No crown, the bezel is decorative |
| RTC | PM8916 PMIC RTC (battery-backed, read over SPMI) |
| Power | PM8916 over SPMI + an external **STC3117** coulomb gauge on I²C |
| Storage | **None yet.** Settings live for one session only |
| Toolchain | **Bare-metal + FreeRTOS** no Arduino IDE |
| Status | **Working** display, touch, vibration, USB log console |

> **This is not an Arduino build.** There is no IDE, no board package and no
> Upload button. The firmware is a bare-metal ARM image with its own FreeRTOS
> runtime, compiled by a shell script and run with `fastboot` as an Android boot
> image.

> **You do not have to overwrite Wear OS.** **`fastboot boot` works on this
> watch** it loads the firmware into RAM and runs it, writing nothing. A power
> cycle returns you to stock. See
> [RAM boot](#option-a--ram-boot-recommended-nothing-is-written).

> ### Why this port is short
>
> The C2 is the **same silicon as the Fossil Gen 4** APQ8009W, same GIC, same
> 19.2 MHz timer, same MDP3 and DSI host, same GCC, same SPMI, same PM8916. All
> of that was already written and proven on the Gen 4, and those drivers key off
> the SoC tier rather than a board name, so they picked this watch up unchanged.
> What is genuinely different is the parts list: a different panel, a FocalTech
> touch controller instead of Raydium, an external fuel gauge, one button
> instead of three, and a 360×360 round screen that needed its own UI tier.

---

## Before you start

- **Reboot to fastboot** see [Enter fastboot](#2-enter-fastboot).
- **Hold the button** the watch switches off.
- **EDL (9008) + QFIL** the last-resort unbrick.

Because the normal way to run this firmware is a RAM boot that writes nothing,
a bad image costs you a power cycle rather than a recovery operation.

> **Never flash `aboot`, `sbl1`, `tz` or `rpm`.** Only `boot` is ours.

---

## Host tools you need

**Android SDK platform-tools** the package that provides `adb` and `fastboot`.

### Linux

```sh
sudo apt install android-sdk-platform-tools fastboot     # Debian / Ubuntu
```

If your distribution does not package it, download the **SDK Platform-Tools for
Linux** zip from <https://developer.android.com/tools/releases/platform-tools>,
extract it, and run the commands on this page from inside that folder.

> **Arch-based distributions:** install the AUR package
> **`android-sdk-platform-tools`**, *not* the repo package `android-tools`.
>
> ```sh
> yay -Sy android-sdk-platform-tools
> ```

### Windows

Use **WSL (Debian/Ubuntu)** and follow the Linux instructions; building the
image needs a Linux environment in any case. You will also need `usbipd` to
pass the watch's USB through to WSL the
[Gen 6 page](fossil-gen6.md#2-enter-fastboot) walks through that step by step
and it is identical here.

### Check it works

```sh
adb version
fastboot --version
```

---

## Part 1 Getting an image

### Option A download a prebuilt image (recommended)

Grab the TicWatch C2 `.img` from the [GitHub releases page](../../../../releases)
and skip to [Part 2 Running it](#part-2--running-it).

### Option B build it yourself

#### Prerequisites

| Thing | Where / how |
|---|---|
| ARM toolchain | `arm-none-eabi-gcc` on your `PATH` (Arm GNU 14.2.Rel1 in use) |
| Python3 | used by the image packer |
| LVGL | this repo's `libraries/lvgl` **auto-detected, nothing to set** |
| DTB | `fossil-port/dumps/c2-skipjack-fromsource/skipjack.dtb` **in this repo, and the packer picks it up by default** |

#### Build

```sh
cd fossil-port/baremetal

# 1. compile + link
CFLAGS_EXTRA="-DWDOG_TRACE" sh build-owf-image-c2.sh

# 2. pack into an Android boot image (the DTB is appended automatically)
sh tools/mk-bootimg-c2.sh build/c2-owf/owf.bin

ls build/c2/         # result: build/c2/owf-boot.img
```

> **The DTB matters, you just do not have to name it.** aboot on msm8909 picks a
> device tree by board-id from a DTB **appended to the kernel** (the `zImage-dtb`
> convention); without one it rejects the image with *"dtb not found"* a safe,
> non-destructive refusal. `mk-bootimg-c2.sh` defaults to the in-repo skipjack
> DTB, so unlike the Gen 4's packer you do not need to pass a path.

> **`-DWDOG_TRACE` is load-bearing.** aboot hands over with the APPS watchdog
> armed, and this flag is the only thing that compiles `wdog_pet()` into the
> main loop. Without it the watch warm-resets a few seconds into every boot,
> which looks exactly like "the image never ran".

> **Where the C2's device tree came from.** The C2 is a 3.18-era appended-DTB
> device, so there is no `dtbo` blob to pull off the eMMC even with root.
> The tree in this repo was compiled from Mobvoi's own published kernel source
> (`android_kernel_mobvoi_skipjack`, the one board file it builds) and
> decompiled to a merged tree. If you want your own watch's tree instead:
> `adb pull /sys/firmware/fdt ticwatch-c2.dtb` from Wear OS no root needed.

---

## Part 2 Running it

### 1. Connect the watch

Put the watch on its charging dock and connect it to the host. If it does not
enumerate, clean the dock pads and the contacts on the watch back with
isopropyl alcohol and re-seat it the magnets will happily hold the dock in a
position that charges but does not make data contact.

### 2. Enter fastboot

From Wear OS with USB debugging enabled (Settings → System → About → tap the
build number, then Developer options → ADB debugging):

```sh
adb reboot bootloader
fastboot devices        # should list the watch
```

### 3. Unlock the bootloader

Required once, before you can flash **or RAM-boot** anything.

```sh
fastboot oem unlock
```

**This wipes user data**, which is expected. Confirm on the watch if it prompts.

> **Do not re-lock the bootloader afterwards.** A locked bootloader may refuse
> to boot an unsigned image and you lose the ability to flash a fix. I take no
> responsibility if you brick your hardware or lose data.

### 4. Strongly recommended: back up the stock partitions first

**Do this before you flash anything.** Stock images for this watch are hard to
come by, and once `boot` is overwritten the original is gone for good. This step
is cheap: it is entirely read-only and writes nothing to the watch.

Wear OS is not rooted, so you need a root shell. The practical route is to
**RAM-boot TWRP** (skipjack builds exist on XDA), which touches no partition:

```sh
fastboot boot twrp-skipjack.img
adb wait-for-device && adb shell id      # expect uid=0(root)
sh fossil-port/tools/dump-c2.sh
```

`dump-c2.sh` is read-only it only ever reads block devices and it writes a
`RESTORE.md` alongside the images describing how to put them back.

> **If you only want the device tree** you need neither root nor TWRP: boot
> Wear OS, enable ADB, and `adb pull /sys/firmware/fdt ticwatch-c2.dtb`.

### 5. Run it

#### Option A RAM boot (recommended, nothing is written)

```sh
fastboot boot owf-ticwatch-c2.img
```

The firmware runs immediately. **Nothing is written to any partition** and a
power cycle returns the watch to stock, Wear OS untouched.

#### Option B permanent install (`boot` partition)

Only when you want the firmware to survive a reboot. **This replaces Wear OS.**

```sh
fastboot flash boot owf-ticwatch-c2.img
fastboot reboot
```

---

## Using it

### Reading the log over USB

Once the firmware is running, **the USB connection becomes a serial console**
a CDC-ACM device that streams the firmware log. It is not `adb`.

```sh
cat /dev/ttyACM0
```

The whole ring buffer is replayed when you connect, so you get everything
printed since boot even if you plugged in late. The watch has no exposed UART,
so this is the log.

### The UI is sized for this screen

At 360×360 the C2 is the smallest round panel in the fleet, and the shared UI
was authored for a 410×502 reference. Rather than a per-board pile of magic
numbers, the firmware derives a `BOARD_SCREEN_ROUND_SMALL` tier from the panel
height, and the watchface dial, launcher grid, app columns and player transport
all pick their own sizes from it. A future TicWatch S2/E2 (also 360×360) will
inherit the whole tier for free.

### First run

- **Set the clock** manually, or from WiFi once that exists.
- **Settings do not persist.** Storage is stubbed on this watch, so
  brightness, accent colour, alarms and notification history live for one
  session only. This is the largest missing subsystem.
- **One button.** There is no crown and no second pusher; the bezel does not
  rotate. Navigation is touch plus that single button.

---

## Build flags

Everything is **silent by default**; failures always print.

### Load-bearing

| Flag | What happens without it |
|---|---|
| `-DWDOG_TRACE` | Nothing pets the APPS watchdog and **the watch warm-resets a few seconds into every boot.** |

### Do **not** pass

| Flag | Why not |
|---|---|
| `-DDISPLAY_BISECT` | Its stage table was proven on the **Gen 6** and silently disables the part of the watchdog staircase you would need here. |
| `-DTSENS_ENABLE` | The die-temperature block is opt-in because enabling it **hard-crashes the Power app** on this SoC. It is left off deliberately. |

### Diagnostics

| Flag | Brings back |
|---|---|
| `-DBOOT_DIAG` | MDSS clock bring-up, the DMA_P splash probe, framebuffer geometry, TLMM mux, touch probe |
| `-DUSB_DIAG` | the 5 s USB heartbeat (`portsc`, `ccs`, `spd`, …) use when enumeration itself is broken |
| `-DTOUCH_DIAG` | paints a diagnostic colour **only if** the touch probe fails; inert when touch works |
| `-DLV_DIAG` | the per-10 s LVGL render / touch census |

> **Never add an unconditional print to a per-frame path.** It wraps the 64 KB
> ramlog faster than the 1 Hz flush can drain it, and the log becomes one line
> repeated forever.

---

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `fastboot devices` shows nothing | Dock contact. Clean the pads with isopropyl alcohol and re-seat it charges long before it makes data contact. |
| `fastboot boot` fails with *dtb not found* | The image was packed without a DTB. Re-run `tools/mk-bootimg-c2.sh`, which appends the in-repo skipjack DTB by default. Nothing ran, nothing was damaged. |
| Watch resets a few seconds into every boot | You built without `-DWDOG_TRACE`. |
| Screen stays black but the watch is clearly alive | aboot handed over a dark panel. The takeover path inherits the bootloader's display state; unlike the Gen 4 there is **no** blind DSI fallback here, because replaying the Gen 4 panel's command table at this panel would be sending one panel's initialisation to another. |
| Every fastboot command hangs after you interrupted one | **Never kill `fastboot` mid-transfer.** It leaves stale bytes in the host USB buffer and desyncs the protocol; unplug and replug to clear it. |
| Watch seems dead / hung | Hold the button to switch off, then re-enter fastboot. A RAM-booted image cannot brick anything. |

---

## Device-specific notes

- **The panel node in the kernel source says 400×400 and is wrong for this
  unit.** `fb_mdp3.c` auto-detects geometry from what the bootloader actually
  programmed into DMA_P, which is 360×360 the same lesson the Gen 4 taught
  when its "390×390" panel turned out to be 454×454.
- **The display is driven by taking over the bootloader's MDP3 DMA_P pipe**
  rather than initialising DSI from scratch, which is why the boot splash
  transitions seamlessly into the firmware.
- **No storage yet** eMMC, NVS, FatFs and the log file are all stubbed.
- **No WiFi or BLE.** The WCNSS/Pronto bring-up has not been started.
- **The TicWatch S2/E2 are `tunny`, not `skipjack`** a different kernel tree
  and a different board header. Same Wear 2100 family, so they would inherit
  most of this port, but they are not covered by this page.
