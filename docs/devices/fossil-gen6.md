# Fossil Gen 6 (hoki) install guide

| | |
|---|---|
| SoC | Qualcomm **SDA429W** (Snapdragon Wear 4100) quad Cortex-A53, run bare-metal in AArch32 |
| Display | 416×416 AUO AMOLED, MSM DSI **command mode** (MDP5 pipe takeover) |
| Touch | Raydium RM_TS (I²C, BLSP1 QUP4 @ `0x39`) |
| RTC | PM660 PMIC RTC (battery-backed, read via SPMI) |
| IMU | QMI8658 **not fitted / not detected on this unit**, step counting disabled |
| Power | PM660 PMIC over SPMI (fuel gauge, charger, buttons) |
| Storage | Internal eMMC a raw window inside `userdata`, exposed as a FAT volume |
| Toolchain | **Bare-metal + FreeRTOS** no Arduino IDE |
| Status | **Working** display, touch, storage, USB, deep sleep |

> **This is not an Arduino build.** There is no IDE, no board package and no
> Upload button. The firmware is a bare-metal ARM image with its own FreeRTOS
> runtime, compiled by a shell script and flashed with `fastboot` as an Android
> boot image. If you have only ever flashed the ESP32 boards, nothing on this
> page carries over except the LVGL libraries.

> **Wear OS is replaced, not dual-booted.** Flashing this firmware to `boot`
> overwrites Wear OS. If you may want to go back, take a
> [full backup of the stock partitions](#4-optional-back-up-your-original-wear-os-installation)
> first. There is also a temporary option that leaves Wear OS in place, see
> [Temporary install](#option-b--temporary-install-recovery-partition).

---

## Before you start

Two hardware button combinations work regardless of what the firmware is doing,
hung, sleeping, mid-boot, or a completely invalid image:

- **Crown (middle) + lower side button, held together ~10 s** → boots
  straight into **fastboot**.
- **Power button held** → the watch switches off.

Neither cares about the software. A bad image costs you one button-hold, not a
recovery operation. Flash freely.

---

## Host tools you need

Both flashing and backing up are done with **Android SDK platform-tools**, 
the package that provides `adb` and `fastboot`. Install it before going further.

### Linux

Most distributions package it. On Debian/Ubuntu:

```sh
sudo apt install android-sdk-platform-tools fastboot
```

If your distribution does not package it, or you want to be certain you have
Google's build, download the **SDK Platform-Tools for Linux** zip from
<https://developer.android.com/tools/releases/platform-tools>, extract it, and
run the commands on this page from inside that folder.

> **Arch-based distributions (Arch, Manjaro, EndeavourOS, Artix):** install the
> AUR package **`android-sdk-platform-tools`**, *not* the repo package
> `android-tools`. They are different packages: `android-sdk-platform-tools`
> ships Google's official prebuilt binaries, which is what these instructions
> assume, while `android-tools` is a separate community build of the tools.

```sh
yay -Sy android-sdk-platform-tools
```

### Windows

If you run Windows then i would recommend you use WSL debian/ubuntu instead.
If you want to build the image yourself then you will need access to a linux
enviroment. If you use WSL then follow the Linux instructions instead.

1. Download the **SDK Platform-Tools for Windows** zip from
   <https://developer.android.com/tools/releases/platform-tools>.
2. Extract it somewhere permanent, e.g. `C:\platform-tools`.
3. Either add that folder to your `PATH`, or run the commands on this page from
   a terminal opened inside it.

Adding/modifying PATH can be done inside "Advanced System Settings" in Windows then
select "Enviroment Variables", then select either the users PATH or systems PATH and
edit it, add the directory where you installed the android-sdk-platform-tools as a 
new entry in the PATH list. Make sure to restart any terminal window or reboot for the 
PATH changes to take effect. 

You may also need Google's USB driver on Windows only for the watch to appear in fastboot
mode, see <https://developer.android.com/studio/run/win-usb>.

### Check it works

```sh
adb version
fastboot --version
```

---

## Part 1 Getting an image

You have two options. **Most people should take the prebuilt image.**

### Option A download a prebuilt image (recommended)

Grab the Fossil Gen 6 `.img` from the [GitHub releases page](../../../../releases).
That is the whole step. Skip to [Part 2 Flashing](#part-2--flashing).

### Option B build it yourself

Only needed if you want to change the firmware or the build flags.

#### Prerequisites

| Thing | Where / how |
|---|---|
| ARM toolchain | `arm-none-eabi-gcc` on your `PATH` (GCC 13+; 16.1 in use) |
| Python3 | used by the image packer and the verifier |
| LVGL | this repo's `libraries/lvgl`, or `~/Arduino/libraries/lvgl` |
| DTB | `fossil-port/sda429-hoki.dtb` |

The LVGL copy must be the one from this repo's `libraries/` folder, with
`lv_conf.h` sitting **next to** the `lvgl` folder, the same arrangement the
ESP32 boards use. Point `LVGL_DIR` at it.

#### Build

Two commands. **Both matter.** The second one's DTB argument looks optional and
is not, without it you get an image that will not boot.

```sh
cd fossil-port/baremetal

# 1. compile + link
CFLAGS_EXTRA="-DWDOG_TRACE -DDISPLAY_BISECT -DPLAT_I2C_RETEST -DTOUCH_DIAG \
              -DBOOT_DIAG -DLV_DIAG -DDSI_DIAG -DSLEEP_DIAG -DOWF_SLEEP_VERBOSE" \
LVGL_DIR=$HOME/Arduino/libraries/lvgl \
sh build-owf-image.sh

# 2. pack into an Android boot image, WITH the DTB appended
sh tools/mk-bootimg-gen6.sh build/gen6-owf/owf.bin ../sda429-hoki.dtb

ls build/gen6/ # result should be: build/gen6/owf-boot.img
```

> ### ⚠️ Known issue build with the logging flags on
>
> The five `*_DIAG` / `VERBOSE` flags above are **currently required**, even
> though they are nominally just logging. There is an open regression where a
> build with them **omitted** boot-loops on hardware; the same source with them
> **on** boots and runs normally. Until that is root-caused, build with the line
> exactly as written above.
>
> The cost is only a chattier log. If you want it quiet, see
> [Logging / debugging flags](#logging--debugging-flags) but expect the
> boot loop until this note disappears.

> **Do not drop flags from that `CFLAGS_EXTRA` line.** Two of them are named
> like debug options but the firmware is broken without them, see
> [Load-bearing flags](#load-bearing-flags-do-not-omit). This mistake has been
> made more than once and produces a watch that reboots after 30 seconds with
> dead touch.

#### Verify the image before flashing

A DTB-less or stale image wastes a flash cycle and gives misleading results.
This catches both in a second:

```sh
python3 - <<'EOF'
import struct, hashlib, os
f = "build/gen6-owf/owf-boot.img"
d = open(f,'rb').read(); assert d[:8] == b'ANDROID!'
ks = struct.unpack_from('<I', d, 8)[0]; ps = struct.unpack_from('<I', d, 36)[0]
k  = d[ps:ps+ks]
off = k.find(b'\xd0\x0d\xfe\xed'); tot = struct.unpack_from('>I', k, off+4)[0]
b = os.path.getsize("build/gen6-owf/owf.bin")
print("payload ", off, "== size(owf.bin)", b, off == b)
print("dtb     ", tot, hashlib.md5(k[off:off+tot]).hexdigest()[:8])
print("trailing", len(k)-off-tot)
EOF
```

Expected: payload offset **exactly** `size(owf.bin)`, dtb totalsize **235622**
with md5 prefix **`68b17ef6`**, trailing **0**.

Full build reference, including every flag in the tree:
[`fossil-port/BUILD-GEN6.md`](../../fossil-port/BUILD-GEN6.md).

---

## Part 2 Flashing

### 1. Connect the charger the right way round

**Do this before anything else, it is the single most common cause of "my
watch will not show up".**

The Gen 6 charger exposes all four USB pins on flat pads, and the magnetic
puck attaches in **two orientations**. One of them gives you a working data
connection; the other frequently does not, and the failure looks exactly like a
broken cable or a driver problem: the device never enumerates, or `adb` and
`fastboot` see it intermittently and hang mid-transfer.

> **Attach the charger so the cable exits from the LEFT side of the watch 
> the side with no buttons.** That orientation has proven consistently
> reliable for me.

If it still will not enumerate:

1. Clean the charger pads **and** the contacts on the back of the watch with
   isopropyl alcohol and let them dry. Sweat and skin oil on those pads cause
   exactly this.
2. Flip the puck to the other orientation and try again.
3. Re-seat it, the magnets can hold the puck in a position where it is
   attached but not fully making contact on all four pads.

### 2. Enter fastboot

Hold the **crown (middle) + lower side button** together for **~10 seconds**.
The watch should boot into fastboot mode. Confirm that fastboot is connected
and reachable with:

```sh
fastboot devices
```

If you are using Windows WSL then you will need to attach the watch to the linux 
enviroment. The easiest way is to use the program `usbipd`. To install it use `winget`
in a terminal window or download the installer from `usbipd-win` github and install it:

```sh
winget install usbipd
```

If that does not work try

```sh
winget install usbipd-win
```

or download and install: <https://github.com/dorssel/usbipd-win/releases>

Then restart any terminal window or reboot. To attach the watch's USB connection to WSL
make sure to start your WSL distro and have it running in the background. Then use `usbipd`:

```sh
usbipd list    # Check all connected usb devices, take note of the watch busid 

usbipd bind --busid X-X  # Replace X-X, requires starting terminal as admin or run with sudo

usbipd attach --wsl --busid=X-X  # Replace X-X with the watch busid
```

Check if the host sees the watch in the WSL enviroment:

```sh
fastboot devices
```

### 3. Unlock the bootloader

Required once, before you can flash anything.

```sh
fastboot oem unlock
```

Confirm on the watch if it prompts. **This wipes user data.** Navigate with the
side keys and hold down the middle button to select the option that unlocks bootloader. 

> **Do not re-lock the bootloader afterwards.** `fastboot oem lock` with
> non-stock firmware installed is risky: a locked bootloader may refuse to boot
> an unsigned image, and you lose the ability to flash a fix. There is no
> benefit for this use case. Locking the bootloader again might you brick your hardware so
> leave it unlocked. Starting the watch in shipping mode will make
> the device only start up again until you connect a charger and it will lock the bootloader 
> again. Entering shipping mode is done via the fastboot mode menu. I have tested entering 
> shipping mode once and it did not brick my device, however i can not guarantee that it 
> will work for any other devices, also the watch stopped booting after the bootloader re-locked 
> itself, booting showed a corruption message. Also remember that unlocking the bootloader will
> factory reset the watch Wear OS installation. I take no resposiblity if you brick your 
> hardware or lose any valuable data. 

### 4. Optional: back up your original Wear OS installation

If you want to be able to return the watch to exactly how it shipped, take a
full dump of the stock partitions **before** you flash anything over them. This
is optional, skip it if you have no intention of going back.

#### Why you cannot just use adb on Wear OS

Enabling Developer options and ADB USB debugging in Wear OS gives you an `adb`
connection, but **Wear OS is not rooted**, so `adb shell` has no permission to
read the raw block devices. `dd` on a partition fails, and there is no way
around it from inside Wear OS. So you cannot dump the images from the stock
system, no matter how the developer settings are configured.

#### The trick: borrow AsteroidOS's kernel for a rooted shell

[AsteroidOS](https://asteroidos.org/) publishes a `boot.img` for the Gen 6
(**hoki**) whose kernel gives you a **root adb shell**. Flash it to the
**`recovery`** partition, that leaves Wear OS on `boot` completely untouched
boot it, dump what you want, and you are done.

> **You only need `boot.img`.** AsteroidOS also ships a root filesystem image,
> but flashing it is **not necessary** just to get adb working. The kernel alone
> gives you the rooted shell, which is all a backup needs.

1. Download the AsteroidOS **hoki** `asteroid-hoki-boot.img` from
   <https://release.asteroidos.org/2.1/hoki/>

2. With the watch in fastboot mode (crown + lower button, ~10 s) and the
   bootloader unlocked, flash it to `recovery`:

```sh
fastboot flash recovery asteroid-hoki-boot.img
```

3. On the watch, use the fastboot menu: navigate to **`Recovery mode`** by pressing
   the side buttons and select it by **holding the crown (middle) button**. You
   cannot select it over fastboot.

4. Confirm you have a rooted shell:

```sh
adb devices
adb shell id        # should report uid=0(root)
```

#### Dump the partitions

First, list what is there and how the names map to block devices:

```sh
adb shell ls -l /dev/block/bootdevice/by-name/
```

Then pull the ones worth keeping. `boot` and `recovery` are the two this
firmware touches; the rest are cheap insurance:

```sh
mkdir -p hoki-stock-backup && cd hoki-stock-backup

adb shell "dd if=/dev/block/bootdevice/by-name/boot" > boot.img
```

> **`persist` is the one that is genuinely irreplaceable.** It holds
> per-device calibration data. `system` and `vendor` are large and can be
> re-obtained from a stock firmware package, so skip them if you are short on
> space, but always take `persist`, `boot` and `recovery`.

Verify the dumps are real before trusting them, a truncated or zero-length file
is worse than no backup:

```sh
ls -l *.img
```

Each should be a plausible size (tens of MB for `boot`/`recovery`, larger for
`system`), and none should be 0 bytes.

#### Restoring later

```sh
fastboot flash boot boot.img
fastboot flash recovery recovery.img
```

> Note that AsteroidOS's `boot.img` is now sitting in your `recovery`
> partition. If you go on to install this firmware to `boot`, remember that
> Wear OS is what normally restores the stock recovery image, with Wear OS
> gone, whatever you last flashed to `recovery` stays there. Flash your
> `recovery.img` backup back if you want the original.

### 5. Flash

#### Option A permanent install (`boot` partition)

This is the normal install. It replaces Wear OS.

```sh
fastboot flash boot owf-fossil-gen6.img
fastboot reboot
```

The watch boots into OpenWatchFace.

#### Option B temporary install (`recovery` partition)

For trying the firmware **without** removing Wear OS.

```sh
fastboot flash recovery owf-fossil-gen6.img
```

Then, on the watch, use the fastboot menu: **navigate to `Recovery mode` with
the side buttons and select it by holding the crown (middle) button.**

> **`fastboot boot` does not work on this device.** There is no RAM-boot on the
> Gen 6, `fastboot boot <img>` transfers only a partial image and never runs
> it. It is not a marginal-USB problem and a smaller image will not fix it.
> `fastboot boot recovery` is not a thing either (`boot` takes an image, not a
> partition name). **Flashing is the only way to run an image.**
>
> This is Gen 6 specific `fastboot boot` *should* work on the Gen 4.

> **The temporary install is genuinely temporary.** While Wear OS is still
> installed on `boot`, it **rewrites the `recovery` partition back to the stock
> recovery image on every boot.** So your firmware survives in `recovery` only
> until Wear OS next boots. That is a feature if you are just testing, and a
> trap if you expected it to stay: if you want it to persist, flash `boot`
> (Option A).

---

## Using it

### Reading the log over USB

Once you boot up the new firmware, **the USB connection becomes a serial console** 
a CDC-ACM device that streams the firmware log, much like a UART. It is not `adb`.

```sh
cat /dev/ttyACM0
```

The whole log is replayed from the start of the ring buffer when you connect,
so you get everything printed since boot even if you plugged in late. This is
the primary way to see what the firmware is doing.

> The watch has **no exposed UART pad**, which is why the log goes over USB.
> On the hardware there is a UART block, but it is disabled see
> `PLAT_UART_DISABLED` in the board header.

### First run

- **Set the clock** by connecting to WiFi, or build once with `FORCE_TIME_SET`.
- **Storage** is a raw window inside `userdata`, mounted as a FAT volume, no
  microSD slot on this device. Notification history, WiFi credentials and
  battery logs live there.

---

## Build flags

Only relevant if you are building yourself (Part 1, Option B). The full
reference is in [`BUILD-GEN6.md`](../../fossil-port/BUILD-GEN6.md); this is the
practical subset.

### Load-bearing flags (do not omit)

| Flag | What happens without it |
|---|---|
| `-DWDOG_TRACE` | The only thing that compiles `wdog_pet()` into the main loop. Without it nothing pets the watchdog and **the watch reboots ~30 s into every boot.** |
| `-DPLAT_I2C_RETEST` | Without it the board header sets `PLAT_I2C_DISABLED`, the QUP never initialises and **touch is completely dead.** |

### Recommended defaults

| Flag | What it does |
|---|---|
| `-DDISPLAY_BISECT` | Selects the watchdog stage-timeout table used by every image proven on hardware. |
| `-DTOUCH_DIAG` | Paints one diagnostic colour **only if the touch probe fails**, magenta = QUP/clock, red = chip mute + L13 rail off, green = rail fine so suspect pins/protocol, yellow = rail unreadable. Inert when touch works. |

### Logging / debugging flags

The port carries a lot of instrumentation from bring-up. It is all **silent by
default**, add a flag to bring it back. **Failures always print regardless;**
only the running commentary is gated.

| Flag | Brings back |
|---|---|
| `-DBOOT_DIAG` | eMMC/SDHCI init, clock (`gcc-*`) bring-up, USB PHY + controller registers, I²C QUP version, framebuffer/panel/TLMM takeover, touch probe + FW version, the image/ramlog/timer banner |
| `-DLV_DIAG` | the per-10 s `LV` / `LV2` / `LV3` render, touch and SPMI census |
| `-DDSI_DIAG` | the per-10 s `DSI arm=` register dump |
| `-DSLEEP_DIAG` | deep-sleep feasibility probe, PSCI report, MPM census, IMEM post-mortem, warm-boot selftest, `CPU_ON` entry test |
| `-DUSB_DIAG` | the 5 s USB heartbeat (`portsc`, `ccs`, `spd`, …) use when enumeration itself is broken |
| `-DTOUCH_LOG` | one line per tap plus a 10 s loop-timing line |
| `-DOWF_SLEEP_VERBOSE` | the shared app's `[sleep] rails cut / no timer wake / no IMU session` lines |

Notes worth knowing:

- **`DSI_DIAG` is special.** The `DSI arm=` line still prints **by itself**
  whenever recovery count, DCS drain timeouts, or a latched CLS/timeout error is
  non-zero. A display regression announces itself; only the healthy case is silent.
- **`SLEEP_DIAG` has side effects**, not just extra printing: it runs the
  warm-boot restore selftest, and the `CPU_ON` test leaves a second CPU core
  powered and parked for the rest of the boot. Fine for investigation, wasteful
  otherwise, do not ship an image with it on.
- **Never add an unconditional print to a per-frame path.** It wraps the 64 KB
  ramlog faster than the 1 Hz flush can drain it and the log becomes one line
  repeated forever. Accumulate and emit at ~1 Hz or slower.

A quiet boot is roughly 20 lines and a sleep is two.

> **Currently you cannot actually run a quiet build** see the known issue in
> [Build](#build). Omitting these flags produces an image that boot-loops. The
> flags are listed here so you know what each one brings back, and for when the
> regression is fixed.

---

## Troubleshooting fastboot

| Symptom | Cause / fix |
|---|---|
| `fastboot devices` shows nothing | **Charger orientation.** Attach it so the cable exits the **left** side (the side with no buttons). Then clean the pads with isopropyl alcohol, then try the other orientation. |
| Enumerates intermittently, commands hang | Same cause, dirty or partially-contacting pads. Clean and re-seat. |
| Every fastboot command hangs after you interrupted one | **Never kill `fastboot` mid-transfer.** `pkill fastboot`, or a `timeout` firing, leaves stale bytes in the host USB buffer and desyncs the protocol. Fix with a USB port reset rather than more killing: find the device via `lsusb \| grep 18d1:d00d` → `/dev/bus/usb/<bus>/<dev>`, then issue a `USBDEVFS_RESET` ioctl on it. Unplugging and replugging the puck also clears it. |
| `fastboot boot` does nothing | Expected. It does not work on the Gen 6, flash instead. |
| Flashed `recovery`, watch still boots Wear OS | You must pick **Recovery mode** from the on-watch fastboot menu (select with the crown). You cannot select it over fastboot. |
| Firmware in `recovery` disappeared | Wear OS restored the stock recovery image on its next boot. Expected see [Option B](#option-b--temporary-install-recovery-partition). |
| Watch boot-loops immediately after flashing your own build | **Known issue.** You built without the logging flags. Rebuild with `-DBOOT_DIAG -DLV_DIAG -DDSI_DIAG -DSLEEP_DIAG -DOWF_SLEEP_VERBOSE` added see [Build](#build). Your image is not corrupt and nothing is missing from it. |
| Watch reboots ~30 s into every boot | You built without `-DWDOG_TRACE`. |
| Touch completely dead | You built without `-DPLAT_I2C_RETEST`. |
| Watch seems dead / hung | Hold crown + lower side button ~10–15 s for fastboot, or hold power to switch off. You cannot brick it. |

---

## Device-specific notes

- **Always-on display is not a feature** of this firmware.
- **No IMU on this unit.** The QMI8658 is not detected, so step counting is
  disabled.
- **The display is driven by taking over the bootloader's MDP5 pipe** rather
  than initialising the panel from scratch, which is why the boot splash
  transitions seamlessly into the firmware.
- **One core, on purpose.** The other three A53s are left powered down; the
  firmware is a single-core FreeRTOS system.
