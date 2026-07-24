#!/usr/bin/env bash
# ============================================================================
#  apply_tuya_package_patches.sh
#
#  Re-applies OpenWatchFace's edits to the Arduino "tuya_open" core package.
#  These live in the installed package (NOT in libtuyaos.a / the SDK source),
#  so a package install/update silently reverts them. Run this after any
#  install or update of the tuya_open board package.
#
#  Two edits (the only real OWF changes to the package — verified):
#    1. cores/tuya_open/<ver>/cores/tuya_open/tuya_app_main.c
#         Arduino main-thread stack 1024*4 (4K) -> 1024*32 (32K).
#         4K overflows -> silent PANIC when building the menu / quick_shade
#         (LVGL SW render + deep flex layout run on this thread).
#    2. tools/vendor-T5/<ver>/flags/include_tuya_open.txt
#         Remove the vendor-LVGL include paths (src/liblvgl/v9...). We run our
#         OWN LVGL from the sketch library; leaving the vendor LVGL on the
#         include path makes its headers collide with ours.
#
#  Idempotent: safe to run repeatedly. It edits in place and reports what it
#  changed (or that the edit was already present).
#
#  Usage:
#     ./apply_tuya_package_patches.sh                 # auto-detect package dir
#     ./apply_tuya_package_patches.sh /path/to/packages/tuya_open
# ============================================================================
set -euo pipefail

# ---- locate the tuya_open package root -------------------------------------
PKG="${1:-}"
if [ -z "$PKG" ]; then
  # Common Arduino15 locations (Windows via WSL/Git-Bash, Linux, macOS).
  for c in \
    "$HOME/AppData/Local/Arduino15/packages/tuya_open" \
    "${LOCALAPPDATA:-}/Arduino15/packages/tuya_open" \
    "/c/Users/$USER/AppData/Local/Arduino15/packages/tuya_open" \
    "$HOME/.arduino15/packages/tuya_open" \
    "$HOME/Library/Arduino15/packages/tuya_open" ; do
    if [ -n "$c" ] && [ -d "$c" ]; then PKG="$c"; break; fi
  done
fi
if [ -z "$PKG" ] || [ ! -d "$PKG" ]; then
  echo "ERROR: tuya_open package dir not found. Pass it as the first argument." >&2
  exit 1
fi
echo "[patch] package root: $PKG"

# ---- edit 1: main-thread stack 4K -> 32K -----------------------------------
MAIN_C="$(find "$PKG/hardware" -path "*cores/tuya_open/tuya_app_main.c" 2>/dev/null | head -1 || true)"
if [ -z "$MAIN_C" ]; then
  echo "[patch] WARN: tuya_app_main.c not found — skipping stack edit." >&2
else
  if grep -q "1024 \* 32" "$MAIN_C"; then
    echo "[patch] stack: already 32K ($MAIN_C)"
  elif grep -q "THREAD_CFG_T thrd_param = {1024 \* 4," "$MAIN_C"; then
    # Replace the stack field and append an explanatory comment.
    sed -i 's#THREAD_CFG_T thrd_param = {1024 \* 4, THREAD_PRIO_1, thread_name};#THREAD_CFG_T thrd_param = {1024 * 32, THREAD_PRIO_1, thread_name}; // 32K: OpenWatchFace runs LVGL SW render + deep flex/menu construction on this thread; 4K overflowed -> silent PANIC on menu/quick_shade. Was 1024*4.#' "$MAIN_C"
    echo "[patch] stack: 4K -> 32K applied ($MAIN_C)"
  else
    echo "[patch] WARN: stack line not in expected form — inspect manually: $MAIN_C" >&2
  fi
fi

# ---- edit 2: strip vendor-LVGL include paths -------------------------------
INC="$(find "$PKG/tools" -path "*vendor-T5/*/flags/include_tuya_open.txt" 2>/dev/null | head -1 || true)"
if [ -z "$INC" ]; then
  echo "[patch] WARN: include_tuya_open.txt not found — skipping include strip." >&2
else
  if grep -q "src/liblvgl/v9" "$INC"; then
    # Back up once (preserve the pristine list the first time we touch it).
    [ -f "$INC.owf-bak" ] || cp "$INC" "$INC.owf-bak"
    # Remove every include line pointing at the vendor LVGL tree (order-independent).
    sed -i '\#src/liblvgl/v9#d' "$INC"
    echo "[patch] includes: vendor-LVGL paths removed ($INC)"
  else
    echo "[patch] includes: vendor-LVGL paths already absent ($INC)"
  fi
fi

# ---- edit 3: restore the BLE host header missing from the public include dir ----
#  The T5 package's nimble/include/tuya_ble.h #includes "tuya_ble_mempool.h", but
#  that header ships only under nimble/host/ (private, NOT installed), so any sketch
#  that includes ble_hs.h (e.g. OpenWatchFace's Tuya BLE port) fails with
#  "tuya_ble_mempool.h: No such file or directory". We vendor a copy under
#  patches/tuya-ble-missing-headers/ and drop it into the package's nimble/include/
#  so the public BLE headers become self-contained. Idempotent.
HDR_SRC="$(dirname "$0")/tuya-ble-missing-headers/tuya_ble_mempool.h"
NIMINC="$(find "$PKG/tools" -path "*vendor-T5/*/src/tal_bluetooth/nimble/include" -type d 2>/dev/null | head -1 || true)"
if [ -z "$NIMINC" ]; then
  echo "[patch] WARN: nimble/include dir not found - skipping BLE header restore." >&2
elif [ ! -f "$HDR_SRC" ]; then
  echo "[patch] WARN: vendored tuya_ble_mempool.h not found at $HDR_SRC - skipping." >&2
elif [ -f "$NIMINC/tuya_ble_mempool.h" ]; then
  echo "[patch] ble header: tuya_ble_mempool.h already present ($NIMINC)"
else
  cp "$HDR_SRC" "$NIMINC/tuya_ble_mempool.h"
  echo "[patch] ble header: tuya_ble_mempool.h restored ($NIMINC)"
fi

# ---- edit 4: SM (pairing) config defaults in the public NimBLE headers ----
#  Phase B (ANCS) runs on a REBUILT libtuyaos.a with the Security Manager compiled
#  in (TY_HS_BLE_SM_SC/LEGACY/BONDING/MITM=1, KEY_DIST=3; built from the WSL
#  ~/TuyaOpen tree, see tuya-t5e1-ble-stack-facts in project memory). The sketch
#  compiles against the package's COPY of tuya_ble_cfg.h, so its defaults must
#  match the lib build - otherwise TY_HS_BLE_SM evaluates 0 in the sketch TU and
#  ble_sm_inject_io() silently becomes a no-op macro. Idempotent.
BLECFG="$(find "$PKG/tools" -path "*vendor-T5/*/src/tal_bluetooth/nimble/include/tuya_ble_cfg.h" 2>/dev/null | head -1 || true)"
if [ -z "$BLECFG" ]; then
  echo "[patch] WARN: tuya_ble_cfg.h not found - skipping SM header flips." >&2
elif grep -q "OWF ANCS" "$BLECFG"; then
  echo "[patch] ble sm cfg: already applied ($BLECFG)"
else
  sed -i \
    -e "s~^#define TY_HS_BLE_SM_SC (0).*~#define TY_HS_BLE_SM_SC (1) // OWF ANCS: SC pairing algorithms (was 0)~" \
    -e "s~^#define TY_HS_BLE_SM_LEGACY (0).*~#define TY_HS_BLE_SM_LEGACY (1) // OWF ANCS: legacy-pairing fallback (was 0)~" \
    -e "s~^#define TY_HS_BLE_SM_BONDING (0).*~#define TY_HS_BLE_SM_BONDING (1) // OWF ANCS: bonding (was 0)~" \
    -e "s~^#define TY_HS_BLE_SM_MITM (0).*~#define TY_HS_BLE_SM_MITM (1) // OWF ANCS: authenticated pairing (was 0)~" \
    -e "s~^#define TY_HS_BLE_SM_OUR_KEY_DIST (0).*~#define TY_HS_BLE_SM_OUR_KEY_DIST (3) // OWF ANCS: ENC+ID keys (was 0)~" \
    -e "s~^#define TY_HS_BLE_SM_THEIR_KEY_DIST (0).*~#define TY_HS_BLE_SM_THEIR_KEY_DIST (3) // OWF ANCS: ENC+ID keys (was 0)~" \
    -e "s~^#define TY_HS_BLE_ROLE_CENTRAL (0).*~#define TY_HS_BLE_ROLE_CENTRAL (1) // OWF ANCS: GATT client procedures (disc/write) gate on this (was 0)~" \
    "$BLECFG"
  echo "[patch] ble sm cfg: SM defaults flipped for the SM-enabled lib ($BLECFG)"
fi

# ---- check: the installed libtuyaos.a must be the SM-enabled rebuild ----------
#  A package install/update replaces libtuyaos.a with Tuya's stock build (no SM
#  algorithms) - pairing/ANCS then fails at runtime. The SM-enabled lib is built
#  in WSL: ~/TuyaOpen/apps/tuya.ai/your_robot_dog + tos.py build, then copied to
#  the package libs dir. This check only warns (the lib is too big to vendor here).
#
#  IMPORTANT (2026-07-10): the WSL tree needs SOURCE EDITS beyond the SM config
#  flips (grep "OWF ANCS" under src/tal_bluetooth/nimble/host/ to find them all):
#   1. ble_hs_hci_evt.c: Tuya's CUT_EVT size trim removes the ENCRYPT_CHG /
#      ENC_KEY_REFRESH / LE LT_KEY_REQ handlers from the HCI dispatch tables.
#      Without them pairing completes the whole SMP ceremony and then stalls at
#      encryption start (iOS disconnects after ~30s, no error anywhere). Both
#      guards must read `#if !TY_HS_BLE_CUT_EVT || TY_HS_BLE_SM`.
#   2. ble_hs.c + ble_hs_misc.c + ble_hs_priv.h: ble_hs_misc_restore_irks() was
#      disabled with `#if 0 // We dont need IRK and SM`. It loads stored peer IRKs
#      into the controller resolving list at host sync; without it a bonded iPhone
#      reconnecting under a rotated RPA after a watch REBOOT reads as "unbonded
#      peer" and iOS drops the link (svc disc error status=7). All three guards
#      become `#if TY_HS_BLE_SM`.
#   3. ble_hs_startup.c: ble_hs_pvcy_set_our_irk(NULL) was inside
#      `#if !TY_HS_BLE_CUT_ATT` (cut). It is the ONLY sender of LE Set Address
#      Resolution ENABLE + the local IRK/resolving-list init - without it the
#      restored peer IRKs (fix 2) sit in a DISABLED resolving list, so bonded
#      reconnects still read "unbonded peer" and iOS forces re-pairing every
#      session. Guard becomes `#if !TY_HS_BLE_CUT_ATT || TY_HS_BLE_SM`.
#   4. ble_att.c ble_att_rx_dispatch_entry_find(): remove the early-exit
#      `if (entry->bde_op > op) break;`. The fork split the ATT dispatch table
#      into role #if blocks, breaking the sort the early-exit assumed - every op
#      past a sort discontinuity was unreachable: Handle-Value NOTIFICATION 0x1B
#      (ALL inbound iOS/ANCS notifications) and MTU_REQ 0x02 were silently
#      dropped as "Rx Op Not Support" (PR_DEBUG = invisible).
#   5. ble_hs_pvcy.c ble_hs_pvcy_set_our_irk(): the clear/enable/re-add privacy
#      block only ran when the IRK CHANGED (`if (memcmp(...) != 0)`). Our sleep
#      path restarts BLE (ble_end/ble_begin) every suspend: the controller resets
#      (resolving list wiped, resolution disabled) but the lib statics survive, so
#      the restart skipped re-enabling address resolution -> bonded iPhone RPAs
#      stopped resolving after the first sleep, iOS "key missing", forget+re-pair
#      every session. Make the block unconditional.
#  If you ever rebuild from a FRESH TuyaOpen checkout, re-apply all of these.
#  Also note: the watch-side bond store (OpenWatchFace/tuya/ble_store_kv_tuya.h)
#  is the other half of pairing - without it every Pairing Request is rejected
#  (sm_err=8: no store callbacks). That one lives in the sketch, so it's safe.
TUYAOS_LIB="$(find "$PKG/tools" -path "*vendor-T5/*/libs/libtuyaos.a" 2>/dev/null | head -1 || true)"
if [ -n "$TUYAOS_LIB" ] && ! grep -q "ble_sm_sc_init" "$TUYAOS_LIB"; then
  echo "[patch] WARN: $TUYAOS_LIB has NO Security Manager (stock build?)." >&2
  echo "[patch]       Rebuild in WSL (~/TuyaOpen, robot_dog app, tos.py build) and copy" >&2
  echo "[patch]       .build/lib/libtuyaos.a over it, or BLE pairing/ANCS will fail." >&2
fi

echo "[patch] done."
