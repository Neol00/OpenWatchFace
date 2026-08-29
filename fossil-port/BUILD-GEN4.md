# Building the Fossil Gen 4 (firefish / msm8909w) bare-metal firmware

The Gen 6's twin of this document is [`BUILD-GEN6.md`](BUILD-GEN6.md), and the
two ports deliberately share almost everything: the same runtime, the same
compat layer, the same app, the same watchdog staircase. Read that one for the
philosophy; this one records only what is different here, plus the facts this
watch has actually proven.

## Status

The real OpenWatchFace firmware **boots on the Gen 4 and puts its UI on the
glass**. What works, what is stubbed and what has never run is listed under
[Subsystem status](#subsystem-status).

## Prerequisites

- `arm-none-eabi-gcc` (any recent Arm GNU toolchain; 14.2.Rel1 was used).
- LVGL 9.5.0, the same tree the Arduino builds use. Point `LVGL_DIR` at it.
- `fastboot`, and a Gen 4 with an unlocked bootloader.
- **A soldered USB tap.** Unlike the Gen 6, the Gen 4 exposes no usable USB on
  its charger pads; the D+/D- lines have to be brought out by hand. That rig is
  fragile — it has torn off once already — so prefer changes that can be
  validated in one boot over ones that need a long test campaign.

## The canonical build

```sh
cd fossil-port/baremetal

# 1. compile + link the real firmware
CFLAGS_EXTRA="-DWDOG_TRACE -DBOOT_DIAG" \
LVGL_DIR=$HOME/Arduino/libraries/lvgl \
sh build-owf-image-gen4.sh

# 2. pack it into an Android boot image, WITH the stock DTB appended
sh tools/mk-bootimg.sh build/gen4-owf/owf.bin ../firefish-stock.dtb

# result: build/gen4/owf-boot.img — copy it to a descriptive name
cp build/gen4/owf-boot.img build/gen4-owf/owf-gen4-<TAG>.img
```

`build.sh gen4` is a different thing: it builds the QEMU-style **demo** target
(`ui_demo.c`), not the firmware. Useful for checking the runtime still links;
not what you flash.

### The DTB argument is not optional

aboot on msm8909 picks a device tree by board-id from a DTB **appended to the
kernel** (the `zImage-dtb` convention). Without one it rejects the image
outright with "dtb not found" — a safe, non-destructive refusal, but it means
`mk-bootimg.sh` called without its second argument silently produces an image
that cannot boot. `firefish-stock.dtb` is this watch's own dumped tree and is
appended verbatim; our payload self-relocates, so a DTB riding along behind it
disturbs nothing.

## Booting and recovery

**`fastboot boot` works on this watch** — this is the big operational
difference from the Gen 6, where it transfers a partial image and never runs
it. Every test cycle here can therefore be a RAM boot that touches no
partition:

```sh
fastboot boot fossil-port/baremetal/build/gen4-owf/owf-gen4-<TAG>.img
```

Nothing is written, and a power cycle returns the watch to stock. Flash the
`boot` partition only when you want the firmware to survive a reboot, and
never touch `aboot`, `sbl1`, `tz` or `rpm`.

Recovery ladder, unchanged: reboot → fastboot (button hold) → EDL (9008) via
QFIL as the last resort.

## Flags

### Load-bearing (the firmware is broken without them)

| Flag | What happens if you omit it |
|---|---|
| `-DWDOG_TRACE` | The only thing that compiles `wdog_pet()` into the main loop. aboot hands over with the APPS watchdog armed at an 11 s bark (`qcom,bark-time = 0x2af8`, confirmed in this watch's own DTB), so **without this the watch warm-resets into Wear OS a few seconds into every boot** — which looks exactly like "the image never ran". It also enables the `wdog_stage()` staircase, which is this watch's only debugger. |

### Diagnostics

| Flag | Where | Effect |
|---|---|---|
| `-DBOOT_DIAG` | everywhere | The boot commentary: MDSS clock bring-up, the DMA_P splash probe, framebuffer geometry, TLMM mux, touch probe. |
| `-DFB_COLORTEST` | `platform/fb_mdp3.c` | Four-band R/G/B/W test pattern with a border and a diagonal, held for 5 s before LVGL starts. Names the pack pattern, the stride and the column offset outright instead of leaving you to infer them from a photo of a watch face. This is what identified the R↔B swap. |
| `-DMDP3_PACK_BGR` | `platform/msm_mdp3.c` | Sends blue first instead of red. The default (RGB) is the hardware-proven one; this exists to flip back in one rebuild if a different panel variant needs it. |
| `-DFB_NO_MDP` | `platform/fb_mdp3.c` | Compiles out every display register access. LVGL still renders into the buffer; nothing reaches the glass. Bring-up safety. |
| `-DGEN4_DSI_INIT` | `platform/fb_mdp3.c` | Enables the blind DSI+panel bring-up as a fallback when aboot leaves the display dark. Off by default, on purpose — see below. |
| `-DTOUCH_DIAG` | `platform/touch_raydium.c` | Paints a colour only if the touch probe fails. Inert when touch works. |
| `-DLV_DIAG` | shared | Per-10 s LVGL render / touch census. |

### The watchdog staircase

With `-DWDOG_TRACE` and **without** `-DDISPLAY_BISECT`, every boot milestone
arms a distinct watchdog timeout, so a stopwatch reading from power-on names
the last milestone reached. The table is in `platform/msm_wdog.c`:

| Reading | Meaning |
|---|---|
| ~11–15 s | aboot's own watchdog: **our code never ran** |
| ~4 s | died building the MMU tables |
| ~6 s | died turning the MMU and caches on |
| ~10 s | `mmu_enable_flat()` returned |
| ~12 s | `uart_init` survived |
| ~18 s | `ramlog_init` survived |
| ~20 s | `gic_init` survived |
| ~22 s | vibrator + SPMI read survived |
| ~24 s | `xTaskCreate` OK |
| ~26 s | the app task ran (hung inside `setup()`) |
| never | it reached `loop()` and keeps petting — the firmware is alive |

**Do not pass `-DDISPLAY_BISECT` on this watch.** That table maps stages 2–8 to
"already proven, leave the watchdog alone" — proven on the *Gen 6*. On the Gen 4
it silently disables exactly the part of the staircase you need.

## Why the display path looks the way it does

`platform/fb_mdp3.c` never initialises DSI. aboot lights the panel for its
splash and drives it from MDP3's DMA_P engine over a command-mode DSI link;
that is the same engine, host and panel the firmware wants, so the port
reprograms **only the DMA_P block** — format, geometry, stride, buffer address
— and writes `DMA_P_START` once per frame. The DSI host, its 28 nm PHY, its
PLL and the panel's power-on command table are never touched, so none of them
can be got wrong.

The alternative was tried first and abandoned: a from-scratch DSI bring-up has
no observable failure modes on a watch with no console. Every mistake in it,
from a PHY timing to a page-select, produces one symptom — a black screen. The
Gen 6 spent its whole display campaign there and only got pixels after
inheriting the bootloader's state. The Gen 4 started from that conclusion, and
had its UI on the glass on the first boot that ran.

The blind path still exists behind `-DGEN4_DSI_INIT` (`msm_dsi.c` +
`dsi_panel.c`, whose command table is the real AUO h139 sequence decoded from
this watch's DTB) for the case where aboot hands over a dark display.

## Facts this watch has proven

| Fact | How it was learned |
|---|---|
| **`uart_init()` must not run.** `PLAT_UART_DISABLED` is now set in `boards/fossil_gen4.h`. | The first image that ran died instantly. BLSP1 UART1 at `0x78AF000` is the stock console, but nothing proves aboot leaves it clocked at handoff, and a read of a clock-gated MSM block never completes — the AHB transaction hangs and the boot dies there. Disabling it was the difference between "instant reboot into Wear OS" and a booting firmware. Nothing is lost: no UART pad is known to be reachable on this watch, and the ramlog keeps every `con_puts()`. |
| **The panel wants RGB pack order, not BGR.** | The DT declares color-order `rgb_swap_rgb` and `mdp3_ctrl.c` picks BGR for 8888 formats, so BGR was the reasoned guess — and it was wrong. Red rendered as blue and a bright blue as orange, with black and white correct: R↔B swapped and nothing else. The DT property evidently describes a swap the panel already performs. |
| **Panel geometry is 454×454**, at column offset 10. | The stock DTB's AUO h139 node, cross-checked against the Raydium node's display-coords `0x1c6`. The 390×390 in circulation is a community guess and is wrong. |
| **The MDSS, BLSP and SDCC clock register maps are identical to the Gen 6's.** | `clock-gcc-8909.c` defines the same `0x4D07C…0x4D098` MDSS block, the same `0x42004/0x42018/0x4201C` SDCC1 registers, and the same voted BLSP1 AHB bit; firefish's own DTB puts `gdsc_mdss` at `0x184D078`. Only the touch QUP differs (QUP5 here, QUP4 on the Gen 6) and the `mdp_clk_src` rate table. |
| **The APPS watchdog is the same block at the same address**, armed the same way. | `qcom,msm-watchdog` `reg = <0xb017000 0x1000>`, `qcom,bark-time = 0x2af8`, straight out of this watch's DTB. |

## Subsystem status

| Subsystem | State |
|---|---|
| Runtime (startup, MMU, GIC, arch timer, FreeRTOS, newlib) | Working on hardware. |
| Vibration (PM8916 vibrator over SPMI) | **Working.** The motor is on SPMI **slave id 1**, not 0 — both watches' device trees put `qcom,vibrator@c000` under `qcom,pm8916@1`, and sid 0 asks the arbiter for the wrong PPID. `vib_init()` now resolves the sid through the arbiter tables and checks EE ownership before writing. |
| Display (MDP3 DMA_P takeover) | **Working — the UI is on the glass.** Flush is blocking and full-frame; async DMA and partial-ROI are the perf follow-ups. |
| Touch (Raydium RM_TS on BLSP1 QUP5) | **Working on hardware.** |
| Crown (PixArt PAT9126 on the same bus) | **Working.** `platform/rot_pat9126.c` polls the optical sensor over I2C; `OpenWatchFace/crown_nav.h` maps it to scrolling, and to the quick-shade on the watchface. The wheel is on the **X** delta (measured, not the vendor driver's Y) — see `PLAT_CROWN_AXIS_X`. |
| Panel brightness (DCS 0x51) | **Working.** `platform/dsi_panel.c` sends DCS 0x51/0x53 over the existing command-mode link, and refuses if DSI is not enabled in command mode. |
| Storage (eMMC / NVS / FatFs / log file) | Stubbed in `platform/gen4_stubs.c`. Settings live for one session only. |
| USB CDC log console | **Working** — `cat /dev/ttyACM0`. The old note here (that msm8909w has a ULPI SNPS PHY and needed a port) was a guess and the device trees contradict it: `usb@78d9000` is phy-type <3> with a `phy_csr` window at 0x6c000, field-for-field the Gen 6's node. gcc_usb.c / usb_phy_msm.c / usb_ci.c are now shared verbatim, gated on `PLAT_HAVE_USB_CDC`. |
| Power (RPM, cpufreq, deep sleep) | Not started. |
| BLE / WiFi (WCNSS) | Not started. |

## Memory map

Image links at `0x80008000`; DDR is 512 MB at `0x80000000` (single bank —
`ramoops_region` at `0x9FF00000` pins the top).

`mmu.c` carves the `no-map` reservations out as Device+XN so the A7's
prefetcher cannot speculate into them and take an XPU violation — the failure
mode that reads as an unexplained reset seconds after boot, with nothing in the
log:

| Region | Range |
|---|---|
| `external_image` | `0x87B00000` + 5 MB |
| `modem_adsp` | `0x88000000` + 33 MB |
| `pheripheral` | `0x8A100000` + 5 MB |
| `ramoops` | `0x9FF00000` + 1 MB |

`splash_region@83000000` (+12 MB) is **not** carved out — it is a plain
reservation, and it is aboot's own framebuffer. The malloc arena
(`OWF_DDR_SAFE_END`) stops below it, so the heap can never grow into memory the
display engine might still be scanning.
