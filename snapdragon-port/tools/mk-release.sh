#!/bin/sh
# mk-release.sh — build the three publishable Wear 2100 images in one go.
#
# Usage: sh snapdragon-port/tools/mk-release.sh <version>          # e.g. 1.5.1
#        sh snapdragon-port/tools/mk-release.sh <version> c2       # one board
#
# Produces snapdragon-port/baremetal/release/owf-<OTA key>-<version>.img plus a
# SHA256SUMS, named exactly as ota_check.h expects the GitHub release assets to
# be ("owf-" BOARD_OTA_KEY "-" tag ".img").
#
# TWO THINGS THIS DOES THAT A HAND-RUN BUILD FORGETS:
#
#  1. OWF_PUBLIC=1. Without it the builder's own WiFi MAC, baked into
#     firmware/<board>/wcnss_nv.c by mk-wcnss-nv.sh, is compiled into the image,
#     and every watch that flashes the release transmits THAT address. With it
#     the wcnss_mac symbol is stripped and each watch derives its own
#     locally-administered MAC from its eMMC CID.
#  2. It then GREPS THE PACKED IMAGE for the MAC bytes and refuses to write the
#     release if it finds them. A build flag you can forget is a bug waiting to
#     happen; a check that fails the build is not.
#
# The WCNSS NV calibration table is still embedded (WiFi does not start without
# it). That is board data, contains no MAC, and is byte-identical across every
# unit compared so far -- but whether you may redistribute the vendor's table is
# a licensing question you should answer for yourself before publishing.
set -e
VER="$1"; ONLY="$2"
[ -n "$VER" ] || { sed -n 2,8p "$0"; exit 1; }

HERE=$(cd "$(dirname "$0")" && pwd)
BM="$HERE/../baremetal"
REL="$BM/release"
FLAGS="-DWDOG_TRACE -DSLEEP_NO_WDOG -DSYS_PC_8909 -DSYS_PC_STAGE=6 -DL2_SAW_AP_ENABLE -DSYS_PC_XO_SHUTDOWN -DSLEEP_QUIESCE"
mkdir -p "$REL"
cd "$BM"

# Warn early rather than after three builds: the version the images REPORT comes
# from device_info.h, and an OTA compares it numerically against the release tag.
DEV=$(sed -n 's/.*DEVICE_VERSION *"\([^"]*\)".*/\1/p' ../../OpenWatchFace/device_info.h | head -1)
[ "$DEV" = "$VER" ] || echo "[rel] WARNING: device_info.h says DEVICE_VERSION \"$DEV\", building release $VER"

build_one() {
  board="$1"; key="$2"; packer="$3"; binp="$4"; outp="$5"
  [ -z "$ONLY" ] || [ "$ONLY" = "$board" ] || return 0
  echo "[rel] === $board -> owf-$key-$VER.img"
  OWF_PUBLIC=1 CFLAGS_EXTRA="$FLAGS" sh "build-owf-image-$board.sh"
  # $binp is deliberately UNQUOTED: the Gen 4 packer takes two words (bin + dtb).
  # shellcheck disable=SC2086
  sh $packer $binp
  cp "$outp" "$REL/owf-$key-$VER.img"
}

build_one gen4 fossil-gen4-firefish "tools/mk-bootimg.sh"    "build/gen4-owf/owf.bin ../dtbs/firefish-stock.dtb" "build/gen4/owf-boot.img"
build_one c2   ticwatch-c2-skipjack "tools/mk-bootimg-c2.sh" "build/c2-owf/owf.bin"                             "build/c2/owf-boot.img"
build_one s2   ticwatch-s2-tunny    "tools/mk-bootimg-s2.sh" "build/s2-owf/owf.bin"                             "build/s2/owf-boot.img"

# --- the gate: no builder MAC may appear in anything we are about to publish ---
python3 - "$REL" "$VER" "$HERE/../firmware" <<'PY'
import sys, os, re, glob, hashlib
rel, ver, fw = sys.argv[1:4]
macs = {}
for c in glob.glob(os.path.join(fw, '*', 'wcnss_nv.c')):
    m = re.search(r'wifimac\.ini: ([0-9A-Fa-f]{12})', open(c).read())
    if m: macs[os.path.basename(os.path.dirname(c))] = bytes.fromhex(m.group(1))
    m2 = re.search(r'wcnss_mac\[6\] *= *\{([^}]*)\}', open(c).read())
    if m2:
        b = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m2.group(1))]
        if len(b) == 6: macs[os.path.basename(os.path.dirname(c)) + ':sym'] = bytes(b)
imgs = sorted(glob.glob(os.path.join(rel, f'*-{ver}.img')))
if not imgs:
    sys.exit("[rel] FAILED: no images produced")
bad = False
for p in imgs:
    d = open(p, 'rb').read()
    hit = [k for k, v in macs.items() if v in d]
    print(f"[rel] {os.path.basename(p):40s} {len(d):8d} bytes  baked-MAC: {hit if hit else 'none'}")
    if hit: bad = True
if bad:
    sys.exit("[rel] REFUSING TO PUBLISH: a builder MAC is present in the image(s) above. "
             "Was OWF_PUBLIC=1 honoured by the build script?")
with open(os.path.join(rel, 'SHA256SUMS'), 'w') as f:
    for p in imgs:
        f.write(hashlib.sha256(open(p, 'rb').read()).hexdigest() + "  " + os.path.basename(p) + "\n")
print("[rel] clean. SHA256SUMS written to", os.path.join(rel, 'SHA256SUMS'))
PY
echo "[rel] release $VER ready in $REL"
