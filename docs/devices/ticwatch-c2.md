# Mobvoi TicWatch C2 / C2+ (skipjack) install guide

| | |
|---|---|
| SoC | Qualcomm **APQ8009W** (Snapdragon Wear 2100) quad Cortex-A7, run bare-metal in AArch32. Cores 0 and 1 run the firmware (core 1 renders frame pushes and idles in WFI, with its own usage figure in the Power app); cores 2 and 3 stay off. Deep sleep = full cluster power collapse + RPM XO shutdown, **about 6 mA measured** |
| Display | 360×360 round EDO AMOLED, MSM DSI **command mode** (MDP3 DMA_P pipe takeover) |
| Touch | FocalTech FTS (I²C, BLSP1 QUP5 @ `0x38`), interrupt-latched on TLMM GPIO 13 |
| Buttons | **One** the single side pusher (PM8916 KPDPWR). No crown, the bezel is decorative |
| RTC | PM8916 PMIC RTC (battery-backed, read over SPMI) |
| Radios | **WCN3620** (WCNSS/Pronto): WiFi WPA2 + DHCP + NTP + HTTP, BLE via NimBLE (iPhone pairing + ANCS notifications). HTTPS via mbedTLS (SoC hardware RNG), proven: over-the-air updates download, verify and install. The radio stays resident and idle through deep sleep (Pronto sleeps on its own; a PAS shutdown left a ghost RPM master holding the PA rail) |
| IMU | ST **LSM6DS3** on bit-banged SPI (GPIO 8–11), hardware pedometer for the step counter |
| Heart rate | PixArt **PAH8011** on I²C (GPIO 6/7). Detected and configured, but **it never streams samples** on any unit so far; the Heart app opens and reads nothing |
| Power | PM8916 over SPMI (fuel gauge, button, vibrator). Charging is an external **SMB231**, USB presence is read from the USB PHY |
| Storage | eMMC via sdhci-msm. NVS (Preferences) + FFat + `/owf-log.txt` inside the `userdata` partition, **working**. First boot formats a FAT32 volume there, so Wear OS `/data` is gone from then on |
| Toolchain | **Bare-metal + FreeRTOS** no Arduino IDE |
| Status | **Daily-usable**: display, touch, button, vibration, storage, WiFi, BLE, steps, USB log console, power-off, idle load ≈ 0 % |

> **This is not an Arduino build.** There is no IDE, no board package and no
> Upload button. The firmware is a bare-metal ARM image with its own FreeRTOS
> runtime, compiled by a shell script and run with `fastboot` as an Android boot
> image.

> **You do not have to overwrite Wear OS.** **`fastboot boot` works on this
> watch** it loads the firmware into RAM and runs it, writing nothing to the
> boot partition. A power cycle returns you to stock. See
> [RAM boot](#option-a--ram-boot-recommended-nothing-is-written). The one
> storage layer (since v87) sees that `userdata` still holds a Wear OS volume
> and stays read-only, so nothing is written; settings just do not persist
> across a RAM boot.

> ### Why this port is short
>
> The C2 is the **same silicon as the Fossil Gen 4** APQ8009W, same GIC, same
> 19.2 MHz timer, same MDP3 and DSI host, same GCC, same SPMI, same PM8916,
> same WCN3620 radio. All of that was written and proven on the Gen 4, and the
> drivers key off the SoC tier rather than a board name, so they picked this
> watch up unchanged. What is genuinely different is the parts list: a
> different panel, a FocalTech touch controller instead of Raydium, an
> external charger, one button instead of three and a crown, sensors that the
> Gen 4 unit does not have, and a 360×360 round screen that needed its own UI
> tier.

---

## Before you start

- **Reboot to fastboot** see [Enter fastboot](#2-enter-fastboot).
- **Emergency fastboot** see: https://xdaforums.com/t/how-to-ticwatch-e2-s2-open-fastboot-recovery-unlock-lock-bootloader-twrp.4374321/
- **Hold the button** the watch switches off. The Power app's power-off button
  does a real PMIC shutdown too; the button or a charger wakes it.
- **EDL (9008) + QFIL** the last-resort unbrick. A deep-discharged battery
  also shows up as `QHSUSB__BULK` on USB; that is not a brick, charge it on the
  cradle for an hour and long-press.

Because the normal way to run this firmware is a RAM boot, a bad image costs
you a power cycle rather than a recovery operation.

> **Never flash `aboot`, `sbl1`, `tz`, `rpm`, `modem` or `persist`.** Only
> `boot` is ours. The firmware **reads the radio image out of the stock
> `modem` partition at runtime** (a FAT16 volume holding `wcnss.mdt/.b0N`), and
> `persist` holds the WiFi calibration and MAC addresses. Wipe either and the
> radios are gone.

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
and it is identical here. This whole port was developed from WSL.

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

A prebuilt image has **no WiFi calibration blob and no MAC address** in it;
WiFi will bring the radio up and then fail at `HAL_START`. That is expected,
and the fix is a build of your own with your watch's blob embedded (see
[WiFi needs your watch's NV blob](#wifi-needs-your-watchs-nv-blob)).

### Option B build it yourself

#### Prerequisites

| Thing | Where / how |
|---|---|
| ARM toolchain | `arm-none-eabi-gcc` on your `PATH` (Arm GNU 14.2.Rel1 in use) |
| Python3 | used by the image packer and the NV blob generator |
| LVGL | this repo's `libraries/lvgl`, or set `LVGL_DIR` to it |
| DTB | `snapdragon-port/dtbs/skipjack-stock.dtb` **in this repo, and the packer picks it up by default** |
| NV blob | optional, for WiFi: `snapdragon-port/firmware/c2/wcnss_nv.c` generated from *your* watch (below) |

#### Build

```sh
cd snapdragon-port/baremetal
export LVGL_DIR=$PWD/../../libraries/lvgl

# 1. compile + link
CFLAGS_EXTRA="-DWDOG_TRACE -DSLEEP_NO_WDOG -DSYS_PC_8909 -DSYS_PC_STAGE=6 -DL2_SAW_AP_ENABLE -DSYS_PC_XO_SHUTDOWN" sh build-owf-image-c2.sh

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

> **The other five flags are the sleep.** `-DWDOG_TRACE` alone boots and runs
> but sleeps at ~45 mA with the core merely clock-gated. The full set above is
> what the release images use; each flag is explained under
> [Build flags](#build-flags).

> **Where the C2's device tree came from.** The C2 is a 3.18-era appended-DTB
> device, so there is no `dtbo` blob to pull off the eMMC even with root.
> The tree in this repo is the real one, extracted from a stock boot partition
> with `tools/extract-dtb.py`. A tree compiled from Mobvoi's published kernel
> source (`dtbs/skipjack-fromsource.dts`) is kept for reference; it differs
> from the stock tree in exactly one line, and that line matters, see the
> C2+ note below. If you want your own watch's tree instead,
> extract it from your `boot` partition dump with `tools/extract-dtb.py`
> (the `/sys/firmware/fdt` node is not readable without root on Wear OS).

#### WiFi credentials

[`OpenWatchFace/OpenWatchFace.ino`](../../OpenWatchFace/OpenWatchFace.ino)
has `WIFI_SSID` / `WIFI_PASS` defines near the top. Fill them
in for a build that joins at boot. **An image built with credentials in it
contains your password in clear text. Do not share it.** A network can otherwise
be joined on the watch itself: **Settings -> WiFi & BLE -> Available networks ->
Scan**, tap any network in the list and type its password on the on-screen
keyboard. Sharing one over BLE from the phone app still works too.
#### Publishing an image: strip your MAC first

`tools/mk-wcnss-nv.sh` bakes **your** watch's MAC into `wcnss_mac[]`, and that
array is compiled into the `.img`. Publish such a build and every watch that
flashes it transmits your address: your device identifier turns up on strangers'
networks, and two of them on one network fight over ARP and DHCP.

Build release images with `OWF_PUBLIC=1`:

```sh
OWF_PUBLIC=1 CFLAGS_EXTRA="..." sh build-owf-image-c2.sh
```

That removes the `wcnss_mac` symbol from the object entirely — not merely
ignoring it at runtime, which would leave the bytes in the binary for anyone to
extract — and `wlan_mac()` then derives a **locally-administered** MAC per
device from the eMMC CID: unique to each watch, stable across reboots, and
nothing to do with whoever built the image. The boot log says which it used:

```
wlan: MAC 02:xx:xx:xx:xx:xx (derived per device from the eMMC CID)
```

The NV table itself stays in the image, and that is fine: it is board
calibration data, byte-identical across every unit compared so far (C2, C2+ and
S2 are the same 31723 bytes; the Gen 4 differs in 26 bytes of TX power table),
and it contains no MAC — that lives in `/persist/wifimac.ini`, separately.
Whether you may redistribute the vendor's NV table at all is a licensing
question, the same one that applies to the `wcnss.mdt` radio firmware.

BLE needs nothing: its address is a random static one generated on first boot
and kept in NVS, so it is already per device.

#### WiFi needs your watch's NV blob

The WCN3620 will not start its WLAN HAL without the radio's NV calibration
table, and the MAC address lives next to it. Both are in the watch's `persist`
partition and are pulled from the root shell you get in
[step 4](#4-strongly-recommended-back-up-the-stock-partitions-first):

```sh
adb pull /persist/WCNSS_qcom_wlan_nv.bin
adb pull /persist/wifimac.ini
sh snapdragon-port/tools/mk-wcnss-nv.sh WCNSS_qcom_wlan_nv.bin wifimac.ini c2
```

That writes `snapdragon-port/firmware/c2/wcnss_nv.c`, which the build script
compiles in when it exists. `snapdragon-port/firmware/` is gitignored because the
file contains your MAC address. Without it the build prints
`no firmware/c2/wcnss_nv.c - NV download will be skipped` and WiFi is off;
BLE shares the same radio firmware and the same blob.

---

## Part 2 Running it

### 1. Connect the watch

Put the watch on its charging dock and connect it to the host. If it does not
enumerate, clean the dock pads and the contacts on the watch back with
isopropyl alcohol and re-seat it the magnets will happily hold the dock in a
position that charges but does not make data contact.

### 2. Enter fastboot

From Wear OS with USB debugging enabled (Settings → System → About → tap the
build number, then Developer options → ADB debugging) then allow the connection
on the watch:

```sh
adb reboot bootloader
fastboot devices        # should list the watch
```

### 3. Unlock the bootloader

Required once, before you can flash **or RAM-boot** anything.

```sh
fastboot oem unlock
```

Then it will prompt you again if you are certain with a new command to fill in.

```sh
fastboot flashing unlock
```

**This wipes user data**, which is expected. Confirm on the watch if it prompts.

> **Do not re-lock the bootloader afterwards.** A locked bootloader may refuse
> to boot an unsigned image and you lose the ability to flash a fix. I take no
> responsibility if you brick your hardware or lose data.

### 4. Strongly recommended: back up the stock partitions first

**Do this before you flash anything.** Stock images for this watch are hard to
come by, and once `boot` is overwritten the original is gone for good. This
step is read-only on the watch.

Wear OS is not rooted, so you need a root shell. The practical route is to
**RAM-boot TWRP**, which touches no partition. The Fossil Gen 4 Firefish image
works fine. Firefish TWRP boots on this watch and is what was used on the C2
during development. aboot checks the board-id, so a TWRP image that does not
match is refused safely rather than run.

```sh
fastboot boot twrp-firefish.img
adb wait-for-device && adb shell id      # expect uid=0(root)
```

This TWRP has no `by-name` links, so partitions are addressed as
`/dev/block/mmcblk0pN`. The map is the same on every unit seen so far:

| Partition | Device | Size |
|---|---|---|
| `boot` | `mmcblk0p33` | 32 MiB |
| `recovery` | `mmcblk0p32` | |
| `system` | `mmcblk0p34` | 1.25 GiB |
| `vendor` | `mmcblk0p35` | |
| `persist` | `mmcblk0p24` | |
| `userdata` | `mmcblk0p37` | 1.77 GiB |
| `modem` | `mmcblk0p1` | FAT16, holds the radio firmware |

Confirm with `adb shell "ls -l /dev/block/platform/*/by-name/"` if your TWRP
has the links, or `adb shell "cat /proc/partitions"`. Then dump:

```sh
mkdir -p c2-stock-backup && cd c2-stock-backup
adb shell "dd if=/dev/block/mmcblk0" > c2-emmc-full.img        # whole eMMC, ~3.6 GB
adb shell "dd if=/dev/block/mmcblk0p33" > c2-boot.img
adb shell "dd if=/dev/block/mmcblk0p24" > c2-persist.img
adb pull /persist/WCNSS_qcom_wlan_nv.bin
adb pull /persist/wifimac.ini
```

Check the sizes (`ls -l`, nothing may be 0 bytes) and compare md5s against
`adb shell "md5sum /dev/block/mmcblk0p33"` before trusting them.
`snapdragon-port/tools/dump-c2.sh` does the same per partition and writes a
`RESTORE.md` next to the images.

> **`persist` and `modem` are the irreplaceable ones**: per-device radio
> calibration, MAC addresses and the WCNSS firmware the port loads at runtime.
> `system` is large and can be re-obtained from a stock package.

### 5. Run it

#### Option A RAM boot (recommended, nothing is written)

```sh
fastboot boot owf-ticwatch-c2.img
```

The firmware runs immediately. The boot partition is untouched and a power
cycle returns the watch to Wear OS.

#### Option B permanent install (`boot` partition)

Only when you want the firmware to survive a reboot. **This replaces Wear OS.**
Either from fastboot:

```sh
fastboot flash boot owf-ticwatch-c2.img
fastboot reboot
```

or from the TWRP root shell, which is how the C2 in this project was installed:

```sh
adb push owf-ticwatch-c2.img /tmp/owf.img
adb shell "dd if=/tmp/owf.img of=/dev/block/mmcblk0p33"
adb reboot
```

The image is about 2.5 MiB and the partition is 32 MiB, so the `dd` needs no
padding. **Restore Wear OS** the same way with your `c2-boot.img`.

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
so this is the log. Identical consecutive lines are collapsed into one
`[xN suppressed]` marker.

The firmware also writes `/owf-log.txt` on the FFat volume, and a **blackbox**
region in `userdata` replays the previous boot's log on the next one, which is
what you read after a crash.

### The UI is sized for this screen

At 360×360 the C2 is the smallest round panel in the fleet, and the shared UI
was authored for a 410×502 reference. The firmware derives a
`BOARD_SCREEN_ROUND_SMALL` tier from the panel height, and the watchface dial,
launcher grid, app columns and player transport all pick their own sizes from
it.

### First run

- **Storage leaves Wear OS alone.** Since v87 the firmware checks `userdata`
  before claiming it. If it still holds a Wear OS volume (ext4, f2fs, or an
  encrypted volume with its crypto footer), storage stays **read-only for that
  boot**: settings live in RAM only, nothing is written, and the log says
  `storage: userdata holds ... (Wear OS) - NOT touching it`. That is what you
  want for a RAM boot. For a real install, erase the partition once and the
  next boot creates the firmware's own volume:

  ```sh
  fastboot erase userdata
  ```
- **Storage formats itself after that.** The first boot on an erased
  `userdata` prints `storage: userdata @...` and formats a FAT32 volume there;
  from then on Preferences, the log file and the Files app all persist.
- **Time** comes from NTP as soon as WiFi connects, otherwise set it by hand.
- **WiFi** the WiFi app scans and joins WPA2 networks; NTP, the HTTP weather
  feed and HTTPS (mbedTLS with the SoC's hardware RNG) all work. Over-the-air
  updates are confirmed end to end since 1.5.0 (check, download with resume,
  SHA-256 verify, write to `boot`, reboot).
- **BLE** the watch advertises as `WatchFace-XXXX` (last two bytes of the WiFi
  MAC). Pairing with an iPhone uses the on-watch passkey; ANCS notifications
  mirror **one way**, watch dismisses reach the phone but a phone-side clear
  never removes anything on the watch.
- **One button.** There is no crown and no second pusher; the bezel does not
  rotate. Navigation is touch plus that single button. The Power app's
  power-off is a real PMIC shutdown.
- **Steps** come from the LSM6DS3's hardware pedometer. **Heart rate** does
  not: the PAH8011 answers on the bus and accepts its configuration but has
  never produced a sample or lit its LEDs, on this watch or the S2. Parked.
- **Deep sleep is the real thing since v199.** Two idle minutes (or a double
  tap on the pusher) collapse the whole CPU cluster through the SoC's SAW
  sequence and TrustZone; the RPM applies the sleep set and enters XO
  shutdown; the watch wakes on the pusher or the PMIC RTC alarm. Measured with
  the STC3117 coulomb counter: **about 6 mA average asleep** (4.20 V to 4.16 V
  over a three-hour sleep), the same range as stock Wear OS idling with its
  screen off. The wake log prints
  `rpm-masters[post-resume] APSS shutdowns=N xo=N` as proof the RPM took part
  and `sleep-gauge: soc a -> b over N s = avg X mA` for the real figure (needs
  a minute or more on battery; a rising SOC after a long rest is the gauge
  re-basing itself from the cell voltage, and the line says so instead of
  printing a current). USB re-enumerates after a wake. The Power app's
  Draw graph and line are the measured gauge current on this watch.
- **Updates arrive over WiFi, confirmed.** WiFi & BLE → Check for updates asks
  GitHub for the latest release (tag compared numerically against the build's
  version) and, when it carries `owf-ticwatch-c2-skipjack-<version>.img`,
  Install update downloads it (resuming with HTTP ranges if the link stalls),
  verifies the SHA-256 from the release's `SHA256SUMS`, checks the boot header
  and writes it into `boot`, header block last. After the first cable flash the
  USB link is only needed for the log. The release flow is in the
  [README](../../README.md#software-updates-over-the-air).

---

## Build flags

Everything is **silent by default**; failures always print.

### Recommended (what the release images are built with)

```
CFLAGS_EXTRA="-DWDOG_TRACE -DSLEEP_NO_WDOG -DSYS_PC_8909 -DSYS_PC_STAGE=6 -DL2_SAW_AP_ENABLE -DSYS_PC_XO_SHUTDOWN"
```

| Flag | What it does |
|---|---|
| `-DWDOG_TRACE` | Pets the APPS watchdog from the main loop. **Load-bearing**: without it the watch warm-resets a few seconds into every boot. |
| `-DSLEEP_NO_WDOG` | Deep sleep is one uninterrupted collapse until the button or the RTC alarm, with the watchdog stopped for its duration. Without it the sleep is chopped into 15 s chunks to pet the dog, each one a full wake. |
| `-DSYS_PC_8909` | The **system power collapse**: L2/cluster off through the SAW sequence, TrustZone warm boot, RPM sleep set. This is the whole difference between ~45 mA and ~6 mA asleep. |
| `-DSYS_PC_STAGE=6` | The cluster level to use: 6 = the kernel's `l2-pc` (RPM handshake, sleep set applied). 7 = `l2-gdhs` (cluster off, no RPM handshake, ~37 mA) is the fallback if 6 ever misbehaves on a unit. |
| `-DL2_SAW_AP_ENABLE` | Leaves the L2 SAW in its retention mode while awake, as the shipped kernel does between sleeps. Saves a few mA of awake-idle current. |
| `-DSYS_PC_XO_SHUTDOWN` | Drops the crystal vote from the sleep set so the RPM can enter XO shutdown / Vdd-min. Measured on the C2: 37 mA without it, **about 6 mA** with it. |

Since v199 all three Wear 2100 watches run **dual core** by default (core 1
renders and idles in WFI; it is handed to TrustZone with the hotplug flag
before every collapse). `-DNO_SMP_CPU1` builds the single-core variant.

### Load-bearing

| Flag | What happens without it |
|---|---|
| `-DWDOG_TRACE` | Nothing pets the APPS watchdog and **the watch warm-resets a few seconds into every boot.** |

### Do **not** pass

| Flag | Why not |
|---|---|
| `-DDISPLAY_BISECT` | Its stage table was proven on the **Gen 6** and silently disables the part of the watchdog staircase you would need here. |
| `-DTSENS_ENABLE` | The die-temperature block is opt-in because enabling it **hard-crashes the Power app** on this SoC. |
| `-DWCNSS_CORNER_VOTES` | The RPM "corner" votes reset the SoC on this PMIC. The radio comes up fine without them. |

### Experimental / measurement flags

None of these are in the release images. They exist for bring-up and for
measuring the sleep floor.

| Flag | Effect |
|---|---|
| `-DNO_SMP_CPU1` | Single core: core 1 is never booted. Frame pushes run synchronously. Slower UI, no power difference asleep. |
| `-DSYS_PC_STAGE=7` | The `l2-gdhs` cluster level instead of `l2-pc`: no RPM handshake, ~37 mA asleep. Fallback only. |
| `-DSYS_PC_XO_PARK` | Parks the CPU clock on the 19.2 MHz crystal before the collapse instead of staying at 400 MHz on GPLL0. The kernel stays at its 400 MHz safe rate; this was the old behaviour and it made TrustZone's wake time out. Keep off. |
| `-DSLEEP_FLOOR` | The RPM active-set "ladder" (DDR/PLL/LDO/CX votes measured one by one on a cable). Costs ~50 s awake before every collapse and every one of its steps measured 0 mA, so it stays off. `-DSLEEP_FLOOR_SKIP=<mask>` skips steps. |
| `-DSLEEP_BATT_DIAG` | On the C2/S2: suspends the charger input during sleep so the STC3117 reads the cell current with a cable attached. Measurement only. |
| `-DSPM_NO_PMIC_DATA` | Skips programming the L2 SAW's PMIC_DATA words. The kernel writes them; the bootloader leaves them at zero and the pc/gdhs sequences then send zeros to the rail controller and the wake never returns. Bisect flag only. |
| `-DSPM_NO_L2_VDD_INIT` | Skips the SAW voltage-control init (VCTL / PMIC_DATA_3 = the CPU rail's VSET). Same warning: this is what stock's spm-regulator does at probe and the wake needs it. |
| `-DSMP_PARK_CPU23` | Tries to boot cores 2 and 3 into TrustZone power collapse. Resets the C2 on release. Do not pass. |
| `-DTZ_RPM_IRQS_FORCE` | Force-enables TrustZone's two RPM interrupts in the GIC. Stock TZ never uses them during cluster sleep; no effect. |
| `-DSYS_PC_WARM_RESET_DIAG` | Makes a PS_HOLD drop a WARM PMIC reset so DDR/IMEM breadcrumbs survive. The setting persists in the PMIC and **breaks USB enumeration on every later boot** until restored. Do not pass. |
| `-DSLEEP_PAS_KILL_RADIO` | The old sleep path that shut Pronto down through PAS before sleeping. It leaves a ghost RPM master holding the 3.3 V PA rail; the radio now idles resident instead. Do not pass. |
| `-DPC_TRACE` | One flash write per power-collapse breadcrumb during the first attempts of a boot. Bring-up only. |
| `-DUSB_LOG_V2` / `-DUSB_IRQ_WAKE` | The reworked USB console (tail-first replay, host commands) and USB-as-wake-source. Both broke the live log when tried; off. |

### Diagnostics

The release images carry no diagnostic flag. The log then carries errors,
failures and one-line milestones only (radio up, IP lease, BLE connect,
storage mounted, DDR size, the sleep entry/exit census, update progress).

| Flag | Brings back |
|---|---|
| `-DLOG_VERBOSE` | the step-by-step narration: WCNSS bring-up, SMEM/SCM probing, the WPA2 handshake, scan results, the 10 s load census, SMP and power-collapse dumps, each suspend cycle, BLE MTU/notify traces |
| `-DHR_DIAG` | the PAH8011 register snapshot while the Heart app brings the sensor up |
| `-DBOOT_DIAG` | MDSS clock bring-up, the DMA_P splash probe, framebuffer geometry, TLMM mux, touch probe |
| `-DSMEM_DIAG` | the SMEM table and RPM ping, the first things to read when the radio does not start |
| `-DWIFI_DIAG` | every WCNSS bring-up step (PAS, SMD channels, NV download, HAL start) |
| `-DBT_TRACE` | HCI-level tracing of the NimBLE transport |
| `-DBT_DIAG` | a raw HCI reset / advertise test **instead of** the NimBLE host, never together with a build you want BLE from |
| `-DSLEEP_DIAG` | the suspend/resume path and wake sources |
| `-DUSB_DIAG` | the 5 s USB heartbeat (`portsc`, `ccs`, `spd`, …) use when enumeration itself is broken |
| `-DSENSOR_SCAN` | I²C/SPI bus census of the sensor buses at boot |
| `-DFB_COLORTEST` | a four-band R/G/B/W test pattern held for 5 s before LVGL starts |
| `-DLV_DIAG` | the per-10 s LVGL render / touch census |
| `-DPLAT_STORAGE_NOWRITE` | read-only storage: nothing is formatted or written to `userdata` |

> **Never add an unconditional print to a per-frame path.** It wraps the 64 KB
> ramlog faster than the 1 Hz flush can drain it, and the log becomes one line
> repeated forever.

---

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `fastboot devices` shows nothing | Dock contact. Clean the pads with isopropyl alcohol and re-seat it charges long before it makes data contact. |
| USB shows `QHSUSB__BULK` / 9008 | Deep-discharged battery, not a brick. Charge on the cradle for an hour, then long-press. |
| `fastboot boot` fails with *dtb not found* | The image was packed without a DTB. Re-run `tools/mk-bootimg-c2.sh`, which appends the in-repo skipjack DTB by default. Nothing ran, nothing was damaged. |
| Watch resets a few seconds into every boot | You built without `-DWDOG_TRACE`. |
| Screen stays black but the watch is clearly alive | aboot handed over a dark panel. The takeover path inherits the bootloader's display state; unlike the Gen 4 there is **no** blind DSI fallback here. |
| WiFi log ends at `HAL_START` with no response | No NV blob embedded. Generate `firmware/c2/wcnss_nv.c` from your watch's `persist` and rebuild. |
| `wcnss: no modem partition` / `cannot read WCNSS` | The `modem` partition was wiped or is not FAT. Restore it from your backup; the radio firmware lives there. |
| Every fastboot command hangs after you interrupted one | **Never kill `fastboot` mid-transfer.** It leaves stale bytes in the host USB buffer and desyncs the protocol; unplug and replug to clear it. |
| Watch seems dead / hung | Hold the button to switch off, then re-enter fastboot. A RAM-booted image cannot brick anything. |

---

## Device-specific notes

- **The panel node in the kernel source says 400×400 and is wrong for this
  unit.** `fb_mdp3.c` auto-detects geometry from what the bootloader actually
  programmed into DMA_P, which is 360×360. The 400×400 node is the
  **TicWatch S2's** panel, which is why the S2 has [its own page](ticwatch-s2.md)
  and why a C2 image must not be run on an S2.
- **The display is driven by taking over the bootloader's MDP3 DMA_P pipe**
  rather than initialising DSI from scratch, which is why the boot splash
  transitions seamlessly into the firmware.
- **The sensors are behind the modem in Wear OS** (Qualcomm SNS over QMI).
  This port never boots the modem; it drives the LSM6DS3 and PAH8011 directly
  on the buses the modem owned, with the register protocol recovered from the
  modem DSP's own driver.
- **USB presence is read from the USB PHY**, not the PMIC: VBUS on this board
  goes to the SMB231 charger, so the PM8916's `usbin-valid` never asserts.
- **How the sleep works on this SoC.** The bootloader hands over every SAW
  (power-sequencer) register at zero; the firmware programs them the way the
  shipped kernel does (config, delay, PMIC data words, and the CPU rail's
  voltage code into the L2 SAW), hands core 1 to TrustZone with the hotplug
  flag, votes the sleep set to the RPM (panel, touch, eMMC, USB and the
  gauge/charger rails kept in low-power mode, crystal released), then issues
  TERMINATE_PC with the GDHS flag from core 0. TrustZone warm-boots core 0 on
  the PMIC interrupt. The reference for all of it was a rooted stock C2+ over
  adb; the findings are in `snapdragon-port/notes/C2PLUS-FINDINGS.md`.
- **The pusher occasionally needs a second press to wake the C2**; the Gen 4
  never does. Open item, on the PMIC interrupt side.
- **The C2+ is the same board with 1 GB of RAM, and it needs the stock
  tree.** Its socinfo reports platform subtype 5 where the C2 reports
  0x105, and aboot wants an exact `qcom,board-id` match. The stock tree
  carries both ids, `<8 0x105 8 0x05>`, so one C2 image boots both watches;
  the from-source tree carries only the first and the C2+ refuses it with
  "dtb not found". The same applies to any TWRP you `fastboot boot`: the
  public skipjack TWRP has the one-id tree. `tools/mk-multidtb-boot.py`
  repacks a boot image with a sweep of candidate ids, which is how the C2+
  was first booted into TWRP. The firmware itself does not care about the
  extra RAM; its memory map stays inside the first 128 MB.
