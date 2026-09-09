#!/usr/bin/env python3
"""extract-dtb.py — pull the appended DTB(s) out of a stock Fossil boot.img.

Why this exists: HARDWARE.md open questions #1, #2 and #4 all resolve to "read
the stock device tree". The shipping Fossil DTB is NOT in the public kernel
tree, so the panel init table, the touch bus/address, and the board-id aboot
selects can only come from the device's own boot.img.

Usage:
    python3 extract-dtb.py stock-boot.img [-o outdir]

Then decompile a DTB to source:
    dtc -I dtb -O dts -o firefish.dts outdir/dtb-00.dtb

What it does:
  1. Parses the Android boot image header (v0, page_size from the header) to
     find the kernel region — the same format tools/mkbootimg_v0.py writes.
  2. Scans the kernel region for DTB magic (0xd00dfeed) and carves out each
     one using the totalsize field in its header. msm8909 devices ship a
     *concatenated* set of DTBs (the zImage-dtb / QCDT convention) and aboot
     picks by board-id, so expect several.
  3. Also handles a QCDT ("DTBH"/"QCDT") table if present, reporting the
     platform/variant/soc ids that aboot matches against.

No external dependencies (no libfdt) — it only needs the header fields.
"""
import argparse
import os
import struct
import sys

DTB_MAGIC = 0xD00DFEED
BOOT_MAGIC = b"ANDROID!"
QCDT_MAGIC = b"QCDT"
DTBH_MAGIC = b"DTBH"


def parse_boot_img(data):
    """Return (kernel_bytes, page_size) for an Android boot image."""
    if not data.startswith(BOOT_MAGIC):
        return None, None
    # boot_img_hdr v0: magic[8], kernel_size, kernel_addr, ramdisk_size, ...
    kernel_size, _kernel_addr, ramdisk_size, _ramdisk_addr, \
        second_size, _second_addr, _tags_addr, page_size = \
        struct.unpack_from("<8I", data, 8)
    if page_size == 0:
        return None, None
    n_pages = lambda sz: (sz + page_size - 1) // page_size
    kernel_off = page_size
    kernel = data[kernel_off:kernel_off + kernel_size]
    print(f"boot.img: page_size={page_size} kernel={kernel_size} "
          f"ramdisk={ramdisk_size} second={second_size}")
    # note where a QCDT table would live (after kernel+ramdisk+second)
    dt_off = page_size * (1 + n_pages(kernel_size) + n_pages(ramdisk_size)
                          + n_pages(second_size))
    return kernel, (page_size, dt_off)


def scan_dtbs(blob, base_desc=""):
    """Yield (offset, dtb_bytes) for every FDT found in blob."""
    out = []
    off = 0
    while True:
        idx = blob.find(struct.pack(">I", DTB_MAGIC), off)
        if idx < 0:
            break
        if idx + 8 > len(blob):
            break
        totalsize = struct.unpack_from(">I", blob, idx + 4)[0]
        # sanity: a real DTB is bigger than its header and fits in the blob
        if 64 < totalsize <= len(blob) - idx:
            out.append((idx, blob[idx:idx + totalsize]))
            off = idx + totalsize
        else:
            off = idx + 4
    if out:
        print(f"  found {len(out)} DTB(s){base_desc}")
    return out


def report_qcdt(data, dt_off):
    """If a QCDT/DTBH table is present, print the board-ids aboot matches."""
    if dt_off is None or dt_off + 8 > len(data):
        return
    magic = data[dt_off:dt_off + 4]
    if magic not in (QCDT_MAGIC, DTBH_MAGIC):
        return
    version, num = struct.unpack_from("<II", data, dt_off + 4)
    print(f"QCDT table at 0x{dt_off:x}: version={version} entries={num}")
    # v1/v2/v3 entry layouts differ; print the common leading ids
    entry_size = {1: 20, 2: 24, 3: 28}.get(version)
    if not entry_size:
        print("  (unknown QCDT version, skipping entry decode)")
        return
    for i in range(min(num, 64)):
        eo = dt_off + 12 + i * entry_size
        if eo + entry_size > len(data):
            break
        vals = struct.unpack_from("<%dI" % (entry_size // 4), data, eo)
        print(f"  entry {i}: platform_id=0x{vals[0]:x} variant_id=0x{vals[1]:x} "
              f"soc_rev=0x{vals[2]:x}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="stock boot.img (or a raw kernel/dtb blob)")
    ap.add_argument("-o", "--outdir", default="dtb-out")
    args = ap.parse_args()

    with open(args.image, "rb") as f:
        data = f.read()
    print(f"read {len(data)} bytes from {args.image}")

    kernel, meta = parse_boot_img(data)
    if kernel is None:
        print("not an Android boot image — scanning the whole file for DTBs")
        search, dt_off = data, None
    else:
        page_size, dt_off = meta
        report_qcdt(data, dt_off)
        # DTBs may be appended to the kernel (zImage-dtb) or in the dt area
        search = data          # scan everything; carving is magic-driven
        del page_size

    dtbs = scan_dtbs(search)
    if not dtbs:
        print("no DTB found. If the kernel is gzip-compressed, gunzip it first:")
        print("  (zImage carries a compressed payload; the appended DTB is")
        print("   usually AFTER it and should still be visible here)")
        return 1

    os.makedirs(args.outdir, exist_ok=True)
    for i, (off, blob) in enumerate(dtbs):
        path = os.path.join(args.outdir, f"dtb-{i:02d}.dtb")
        with open(path, "wb") as f:
            f.write(blob)
        print(f"  wrote {path}  (offset 0x{off:x}, {len(blob)} bytes)")

    print("\nnext: decompile and grep for the panel + touch nodes:")
    print(f"  dtc -I dtb -O dts -o out.dts {args.outdir}/dtb-00.dtb")
    print("  grep -n 'mdss-dsi-panel-name\\|on-command\\|raydium\\|i2c@78b' out.dts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
