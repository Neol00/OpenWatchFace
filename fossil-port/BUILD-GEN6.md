# Building the Fossil Gen 6 (hoki / SDA429W) bare-metal firmware

Everything needed to produce a flashable image, plus a reference for **every**
build flag in the tree and what it actually does.

If you read only one thing, read [The canonical build](#the-canonical-build)
and [Load-bearing flags](#load-bearing-flags-not-optional). Two of the flags
that *look* like debug tracing are required for the watch to work at all.

---

## Prerequisites

| Thing | Where / how |
|---|---|
| ARM toolchain | `arm-none-eabi-gcc` on `$PATH` (system install is fine; GCC 13+ tested, 16.1 in use) |
| LVGL | `~/Arduino/libraries/lvgl` — **not** the `~/Documents/Arduino/...` default baked into `build.sh`. Export `LVGL_DIR`. |
| Python 3 | used by `tools/mkbootimg_v0.py` and the image verifier below |
| DTB | `fossil-port/sda429-hoki.dtb` (checked in) |

### The exFAT case-insensitivity trap

The project drive is exFAT, which is **case-insensitive**. `#include <string.h>`
therefore collides with ArduinoCore-API's `String.h`. Any directory containing
headers that case-collide with system headers must go on the include path with
**`-iquote`**, never `-I`. This is already handled for
`baremetal/compat/arduino-api`, but keep it in mind if you add headers — the
symptom is baffling errors like "strlen not declared".

---

## The canonical build

Two commands. Both matter; the second one's DTB argument is **optional and
defaults to nothing**, which silently produces an image that will not work.

```sh
cd fossil-port/baremetal

# 1. compile + link
CFLAGS_EXTRA="-DWDOG_TRACE -DDISPLAY_BISECT -DPLAT_I2C_RETEST -DTOUCH_DIAG \
              -DBOOT_DIAG -DLV_DIAG -DDSI_DIAG -DSLEEP_DIAG -DOWF_SLEEP_VERBOSE" \
LVGL_DIR=$HOME/Arduino/libraries/lvgl \
sh build-owf-image.sh

# 2. pack into an Android boot image, WITH the DTB appended
sh tools/mk-bootimg-gen6.sh build/gen6-owf/owf.bin ../sda429-hoki.dtb

# result: build/gen6/owf-boot.img  — copy it to a descriptive name
cp build/gen6/owf-boot.img build/gen6-owf/owf-real-<TAG>-recovery.img
```

Flash (the user does this; it is never run automatically) — see
[Booting, flashing and recovery](#booting-flashing-and-recovery) for which
partition and why:

```sh
fastboot flash boot fossil-port/baremetal/build/gen6-owf/owf-real-<TAG>-recovery.img
```

---

### ⚠️ OPEN REGRESSION: the logging flags are load-bearing right now

A build **without** `-DBOOT_DIAG -DLV_DIAG -DDSI_DIAG -DSLEEP_DIAG
-DOWF_SLEEP_VERBOSE` boot-loops on hardware. The same source **with** them boots
and runs normally. This is not understood yet.

What has been ruled out mechanically: every edited C file preprocesses
identically with and without the flags once print calls are filtered out (the
only diff is the intended `dsi_report = 1`); `__image_start` / `__bss_start` /
`__ramlog_start` are at identical addresses in both builds; and `uart_putc` is a
no-op on this board (`PLAT_UART_DISABLED`), so the prints are not acting as
delays. The remaining suspects are on the C++ side, where `SLEEP_DIAG` also
gates call sites in `compat/arduino_main.cpp` and `OWF_SLEEP_VERBOSE` gates a
`rail_is_on()` loop in `sleep_power.h`.

**Until this is fixed the canonical build above includes all of them.** Sizes
for reference: quiet `owf.bin` = 1775332 bytes, all-diag = 1787108.

### Log verbosity flags

The port accumulated a lot of bring-up instrumentation, all of it useful while
the subsystem in question was being debugged and all of it noise afterwards.
None of it is deleted; it is silent unless you ask for it. **Failures always
print** — only the running commentary is gated.

| Flag | Brings back |
|---|---|
| `-DBOOT_DIAG` | eMMC/SDHCI init, `gcc-*` clock bring-up, USB PHY + controller registers, I2C QUP version, framebuffer/panel/TLMM takeover, touch probe + FW version, `deadman: armed`, image/ramlog/timer banner |
| `-DLV_DIAG` | the per-10 s `LV` / `LV2` / `LV3` render, touch and SPMI census |
| `-DDSI_DIAG` | the per-10 s `DSI arm=` register dump (see note below) |
| `-DSLEEP_DIAG` | deep-sleep probe, PSCI report, MPM census, IMEM post-mortem, warm-boot selftest, `CPU_ON` test |
| `-DTOUCH_DIAG` | per-poll touch tracing |
| `-DOWF_SLEEP_VERBOSE` | the shared app's `[sleep] rails cut / no timer wake / no IMU session` lines |

`DSI_DIAG` is special: the `DSI arm=` line **still prints by itself** whenever
`recov`, the DCS drain timeout counter, or a latched CLS/timeout error is
non-zero. A display regression announces itself; only the healthy case is
silent.

A quiet boot is roughly 20 lines and a sleep is two. The default build command
above intentionally passes none of these.

### `-DSLEEP_DIAG` — deep-sleep instrumentation

The deep-sleep work left a lot of instrumentation behind. It is all silent
unless you add `-DSLEEP_DIAG` to `CFLAGS_EXTRA`. Without the flag a sleep is
two lines:

```
suspend: panel off, tickless 15000 ms chunks (buttons wake)
suspend: woke by pmic-irq after 11131 ms; parks=1 wakeups/min=5
```

With it you additionally get the read-only feasibility probe (SPMI ownership,
RTC alarm, RPM/MPM sleep counters), the PSCI version/FEATURES report, the
`mpm[...]` census either side of each sleep, the IMEM post-mortem of the
previous boot, the warm-boot selftest, and the `CPU_ON` entry-delivery test.

Two of those have SIDE EFFECTS, which is why they are gated at the call site
and not merely silenced: the selftest runs the whole warm-boot restore path
with the MMU off, and the `CPU_ON` test leaves cpu1 powered and parked in WFI
for the rest of the boot. Do not ship a build with this flag on.

Anything indicating a real fault — `spur=`, `stuck=`, a non-zero `psci_fail`
— still prints without the flag.

---

## Booting, flashing and recovery

**Read this before suggesting how to test an image.** Every rule here has been
got wrong at least once.

### `fastboot boot` DOES NOT WORK ON THIS DEVICE

There is no RAM-boot. `fastboot boot <img>` transfers only a **partial image**
and never runs it. It is not a marginal-USB problem and it is not fixable with
a smaller image — the command simply does not work on hoki. **The only way to
run an image is to flash it.** Do not suggest `fastboot boot` as a "safe,
nothing-flashed" test; it is not an option at all.

`fastboot boot recovery` does not exist either — `boot` takes an image, not a
partition name.

> **This is Gen 6 specific.** `fastboot boot` *does* work on the Gen 4. Do not
> generalise the Gen 6 rule to it. The likely difference is the USB path: the
> Gen 4 needs a hand-soldered USB tap (its charger exposes only 2 pins) while
> the Gen 6 brings all 4 USB pins out on the charger pads.

### Which partition

| Partition | When to use it |
|---|---|
| `boot` | **Current home of this firmware.** Flash here. |
| `recovery` | Scratch slot, only useful while Wear OS owns `boot`. |

Caveat that made `recovery` unreliable: **if Wear OS is installed on `boot`, it
overwrites `recovery` on every boot.** So the recovery slot only survives if
Wear OS never boots, and reaching it needs the physical button menu (you cannot
select it over fastboot).

With this firmware on `boot`, that whole problem disappears — flash `boot`
directly.

### Getting back to fastboot (the thing that makes all of this safe)

**Hold the middle (crown) + lower side buttons together for ~10-15 seconds.**
The watch boots straight into fastboot. This is a hardware combination — it does
not care what the firmware is doing, whether it has hung, or whether the CPU is
in a low-power state.

**Holding the power button also just powers the device off.** No sleep state,
hang, or bad image can prevent either of these.

> Consequence worth internalising: **there is no "bricking" risk from a bad
> boot image on this device.** A firmware that hangs, sleeps forever, or never
> reaches the display costs one button-hold, not a recovery operation. Do not
> hedge or add warnings about unrecoverable states — they are wrong and they
> waste the reader's attention.

### Do not kill `fastboot` mid-transfer

`pkill fastboot` (or a `timeout` firing during a command) leaves stale bytes in
the host USB bulk buffer and desyncs the protocol; every later command hangs.
Recover with a `USBDEVFS_RESET` ioctl on the device node rather than by killing
anything — find it via `lsusb | grep 18d1:d00d` → `/dev/bus/usb/<bus>/<dev>`,
then `fcntl.ioctl(fd, (ord('U')<<8)|20, 0)`.

### Verify the image before flashing

A DTB-less or stale-payload image wastes a whole test cycle and produces
misleading results. This check costs a second and catches both:

```sh
python3 - <<'EOF'
import struct, hashlib, os
f = "build/gen6-owf/owf-real-<TAG>-recovery.img"
d = open(f, 'rb').read(); assert d[:8] == b'ANDROID!'
ks = struct.unpack_from('<I', d, 8)[0]; ps = struct.unpack_from('<I', d, 36)[0]
k = d[ps:ps+ks]
off = k.find(b'\xd0\x0d\xfe\xed'); tot = struct.unpack_from('>I', k, off+4)[0]
b = os.path.getsize("build/gen6-owf/owf.bin")
print("total   ", len(d))
print("payload ", off, "== size(owf.bin)", b, off == b)
print("dtb     ", tot, hashlib.md5(k[off:off+tot]).hexdigest()[:8])
print("trailing", len(k)-off-tot)
EOF
```

Expected for a correct gen6-owf image:

| Field | Value |
|---|---|
| total | ~1998848 (a DTB-less image is ~232 KB smaller — that is the bug) |
| payload offset | exactly `size(owf.bin)` |
| dtb totalsize | 235622, md5 prefix `68b17ef6` (`sda429-hoki.dtb`) |
| trailing | 0 |

Then sanity-check the link:

```sh
arm-none-eabi-nm build/gen6-owf/owf.elf | grep -E "wdog_pet|i2c_bus_init|touch_init|logfile_flush"
```

`wdog_pet` **must** appear in `owf_app_task`'s call list, and
`i2c_bus_init`/`touch_init` must be linked. If they are missing you dropped a
load-bearing flag — see below.

---

## Load-bearing flags (NOT optional)

These are named like diagnostics but the firmware is broken without them. Both
were once dropped by accident; the result was a watch that rebooted after 30
seconds with dead touch.

| Flag | Where | What happens if you omit it |
|---|---|---|
| `-DWDOG_TRACE` | `main.c`, `compat/arduino_main.cpp` | The **only** thing that compiles `wdog_pet()` into the main loop. Without it nothing pets the watchdog → **the watch reboots ~30 s into every boot.** Also enables `wdog_stage()` milestone arming. |
| `-DPLAT_I2C_RETEST` | `boards/fossil_gen6.h` | Without it the board header defines `PLAT_I2C_DISABLED 1`, the QUP never initialises → **touch is completely dead.** (The flag exists because an early, since-disproven investigation blamed I2C for a bus hang.) |

## Recommended default flags

| Flag | Where | What it does |
|---|---|---|
| `-DDISPLAY_BISECT` | `platform/msm_wdog.c` | Selects the watchdog stage-timeout table used by every image proven on hardware. Stages already proven map to 0 ("don't touch the watchdog") so the usable range covers what is still unknown. |
| `-DTOUCH_DIAG` | `platform/touch_raydium.c` | Paints a single colour **only if the touch probe fails** (magenta = QUP/clock, red = chip mute + L13 rail off, green = rail fine so suspect pins/protocol, yellow = rail unreadable). Inert when touch works. |

Always present, set by `build-owf-image.sh` itself — do not pass by hand:
`-DPLAT_BOARD_FOSSIL_GEN6`, `-DLV_CONF_INCLUDE_SIMPLE`,
`-DBOARD_SELECT=BOARD_ID_FOSSIL_GEN6`, and `-DOWF_APP` on `main.c` only.

---

## Diagnostic and instrumentation flags

### Logging

| Flag | Where | Effect |
|---|---|---|
| `USB_DIAG` | `platform/usb_ci.c` | Re-enables the 5 s `USB t= portsc= pts= spd= ccs= usbsts= cmd= op= lpm= vid= cfg=` heartbeat line. Was always-on during USB bring-up; off by default since 2026-08-06 (USB fully working). Turn it back on when diagnosing enumeration/PHY-attach problems — `ccs=` alone separates "PHY never attached" from "attached but enumeration broke". |
| `TOUCH_LOG` | `platform/touch_raydium.c`, `compat/arduino_main.cpp` | Emits one `TAP held= rpts= pgt0= lastp=` line per tap (on the finger-up edge), plus a 10 s `LOOP iters= avgbody= maxbody=` line. For diagnosing touch timing. |
| `TOUCH_TRACE` | `platform/touch_raydium.c` | Older on-glass touch instrumentation (colour blocks + raw status bytes as bit-blocks). Superseded by `TOUCH_LOG`; kept for reference. |
| `PERF_BARS` | `platform/fb_splash.c` | On-glass performance overlay: loop-body time vs how often bodies run. |
| `REG_BARS` | `platform/fb_splash.c` | On-glass register readout bars. |
| `STAGE_REPORT` | `main.c` | Reports boot stage progress. |
| `USE_IMEM_MARKS` | `platform/bootmark.c` | Writes boot breadcrumbs to IMEM (recoverable after a hang). |

> **Never add an unconditional `con_puts` to a per-frame path.** It wraps the
> 64 KB ramlog faster than `logfile_flush()` (1 Hz) can drain it, and the log
> becomes one line repeated forever. This has cost a full test cycle twice.
> Accumulate per-frame data and emit at ~1 Hz or slower.

### Storage

| Flag | Where | Effect |
|---|---|---|
| `STORAGE_STAIRS` | `compat/arduino_main.cpp`, `platform/storage_gen6.c` | Runs storage bring-up as a post-`setup()` colour staircase. **Also makes `storage_init()` inert** — so with this flag storage does not really initialise. Do not ship it. |
| `STORAGE_DIAG` | `compat/arduino_main.cpp` | Replays the recorded storage verdict as one boot colour after `setup()`. |
| `PLAT_STORAGE_NOWRITE` | `platform/sdhci_msm.c` | Keeps every read path live but fails all writes before any hardware is touched. Bisect tool. |
| `PLAT_STORAGE_PROBEONLY` | `platform/storage_gen6.c` | Stops after `emmc_init`'s register pokes, before any command reaches the card. |

Both `STORAGE_STAIRS` and `STORAGE_DIAG` are **visual debugging** and are
deliberately absent from the canonical build.

### Display

| Flag | Where | Effect |
|---|---|---|
| `FB_NO_MDP` | `platform/fb_splash.c` | Compiles out all display hardware access — LVGL renders into the buffer, nothing reaches the glass. Bring-up safety. |
| `NO_TEARCHECK` | `platform/fb_splash.c` | Skips PP0 tear-check setup. |
| `NO_REPIN` | `platform/fb_splash.c` | Disables the DCS window re-pin. **Dangerous:** the per-frame re-pin lives inside the DSI recovery branch and is what keeps the dial from walking. Removing it drifts the write pointer and frames stop landing. |
| `VISUAL_TRACE` | `compat/arduino_main.cpp`, `compat/owf_fossil_lvgl.h` | Paints milestone colours during bring-up. |
| `DISPLAY_TEST` | `main.c` | Standalone display test path. |
| `GFX_TEXT_TEST` | `ui_demo.c` | Renders a synthetic log through `fb_text_dump()` — used to validate the on-glass text renderer under QEMU. |

### Bring-up bisect flags (historical)

Each parks the firmware at a known point so the reboot time names the last
milestone reached. Kept because they are cheap and have all been useful once.

`SAFETY_TEST`, `PING_TEST`, `ENTRY_STOP`, `STOP_SCHED`, `STOP_SETUP`,
`STOP_PRINT`, `STOP_VCALL`, `STOP_STAGE1`, `QUICKCALL`, `HEAP_PROBE`,
`DELAY_BISECT`, `DELAY_BISECT2`, `TICK_PROBE`, `TICK_PROBE4`, `TICK_PROBE_APR`,
`TICK_PROBE_CURE`, `TICK_PROBE_SCAN`, `TICK_STOP_AT`, `NO_AUTO_REBOOT`,
`BUZZ_TRACE` (progress via the vibration motor — unreliable, the bootloader
also buzzes).

### Subsystem disables

`PLAT_I2C_DISABLED` (implied unless `PLAT_I2C_RETEST`), `PLAT_UART_DISABLED`,
`PLAT_RTC_WRITE_DISABLED`.

### Not flags — board-header defines

These appear in `#if defined(...)` tests but are **set by the board headers**,
not passed on the command line. Listed so a grep of the tree has no loose ends:

| Symbol | Meaning |
|---|---|
| `PLAT_FOSSIL_VARIANT_FIREFISH` | Gen 4 variant selector (`build.sh gen4-firefish`) |
| `PLAT_FOSSIL_VARIANT_RAY` | Gen 4 variant selector (`build.sh gen4-ray`) |
| `PLAT_I2C_TOUCH_BASE` | QUP base for the touch controller. Gen 6 = `0x78b8000` (BLSP1 QUP4), address `0x39` — confirmed by the AsteroidOS dmesg node `78b8000.i2c/i2c-4/4-0039`. Its presence also gates the legacy Gen 4 `i2c_*` wrappers. |
| `PLAT_I2C_DEFAULT_BASE` | Default QUP for the Arduino `Wire` object |

---

## Other targets

`build.sh` builds the demo/QEMU targets, **not** the real firmware:

```sh
LVGL_DIR=$HOME/Arduino/libraries/lvgl sh build.sh [qemu|gen4|gen4-firefish|gen4-ray|gen6]
```

QEMU is the place to validate anything that draws on the glass **before**
flashing:

```sh
LVGL_DIR=$HOME/Arduino/libraries/lvgl sh build.sh qemu
bash tools/qemu-screenshot.sh 4 build/qemu/text-test.png
```

Note `build.sh`'s source list does not include the storage/FatFs/USB files —
those are gen6-owf only.

---

## Reading logs off the watch

The firmware mirrors its ramlog to **`/owf-log.txt`** on the FFat volume
(`platform/logfile.c`, flushed once per second, recreated each boot). Because
`.ramlog` is NOLOAD and survives a warm reboot, the first flush also dumps the
*previous* boot's tail — so a crash is still recoverable from the next boot's
file.

### Over USB (preferred, since 2026-08-06)

The firmware brings up the HS USB controller in device mode and exposes the
ramlog as a CDC-ACM serial port. **No build flag** — always compiled in, and it
fails soft if the PHY or controller does not come up.

```sh
# plug the watch into the host, then:
ls /dev/ttyACM*
cat /dev/ttyACM0            # or: picocom -b 115200 /dev/ttyACM0
```

The stream replays the **whole ring from the beginning**, so output printed
before the cable was plugged in still arrives, then continues live. The baud
rate is ignored — it is a one-way pipe, not a real UART.

Three stages, three files: `gcc_usb.c` (clocks) → `usb_phy_msm.c` (ULPI PHY,
init sequence read from this device's own DT, **not** the Gen 4 sequence) →
`usb_ci.c` (ChipIdea UDC + CDC-ACM gadget). When it does not enumerate, the
boot log names the stage that failed:

| line | meaning |
|---|---|
| `gcc-usb: ahb/system rc=0` | clocks up; non-zero = a GCC branch never left halt |
| `usb-phy: init seq rc=0 vid=…` | ULPI reachable; `vid=0xffff` = PHY unclocked or in reset |
| `usb: caplen=0x40 hcc=… lpm=…` | register map found (op base = `0x100 + caplen`) |
| `usb: running, portsc=…` | controller started |
| `usb: bus reset` | the host is talking to us |
| `usb: configured` | enumeration done, log streaming |

### On-device / raw

There is no root on the stock Wear OS build, so `adb root` + `dd` is not
available. Without the cable the options are reading the file on-device or
(with care) a raw dump from a booted AsteroidOS shell — note its init mounts
**p50/userdata** as ext4 and our regions live inside p50, so an fsck there can
overwrite them.

Storage region offsets, relative to the **userdata partition start**
(`/dev/block/mmcblk0p50`):

| Region | Offset (512 B blocks) | Size |
|---|---|---|
| blackbox (raw ramlog mirror, at `bb_lba + 1`) | 2048 | 16 MiB |
| NVS (Preferences) | 65536 | 8 MiB |
| FFAT (FAT volume, holds `owf-log.txt`) | 131072 | 1 GiB |

---

## Working rules earned the hard way

1. **Pass the flags and the DTB.** Both have been forgotten once, and both
   produce a broken image that looks plausible.
2. **Verify the packed image** with the script above before handing it over.
3. **Instrument-only first, then change one thing.** A build that both adds
   logging and changes behaviour loses the measurement if it regresses.
4. **Validate on-glass instruments in QEMU** before flashing.
5. **Verify source-derived register values against real source**, not memory.
   Guessed offsets cost this port about half a day on the eMMC.
6. **Check that edits actually landed** (`grep` the live file); a stale
   scripted edit once made three test cycles meaningless.
