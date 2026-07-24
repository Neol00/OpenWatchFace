#!/usr/bin/env python3
"""mkbootimg_v0.py — minimal Android boot image v0 packer (no dependencies).

Packs kernel+ramdisk exactly the way the Fossil Gen 4's aboot expects
(header v0, as used by its stock 3.18-era boot.img — parameters in
../../HARDWARE.md). Replaces AOSP's mkbootimg to avoid the dependency.

Usage:
  mkbootimg_v0.py --kernel k.gz --ramdisk r.gz --pagesize 2048 \
      --base 0x0 --kernel_offset 0x8000 --ramdisk_offset 0x2000000 \
      --tags_offset 0x1e00000 --cmdline "..." -o boot.img
"""
import argparse, hashlib, struct, sys

BOOT_MAGIC = b"ANDROID!"

def parse_int(s): return int(s, 0)

p = argparse.ArgumentParser()
p.add_argument("--kernel", required=True)
p.add_argument("--ramdisk", required=True)
p.add_argument("--pagesize", type=parse_int, default=2048)
p.add_argument("--base", type=parse_int, default=0)
p.add_argument("--kernel_offset", type=parse_int, default=0x8000)
p.add_argument("--ramdisk_offset", type=parse_int, default=0x2000000)
p.add_argument("--second_offset", type=parse_int, default=0xf00000)
p.add_argument("--tags_offset", type=parse_int, default=0x1e00000)
p.add_argument("--cmdline", default="")
p.add_argument("--board", default="")
p.add_argument("-o", "--output", required=True)
a = p.parse_args()

kernel = open(a.kernel, "rb").read()
ramdisk = open(a.ramdisk, "rb").read()
page = a.pagesize

cmdline = a.cmdline.encode()
if len(cmdline) > 512: sys.exit("cmdline too long (>512)")

sha = hashlib.sha1()
for blob in (kernel, ramdisk, b""):
    sha.update(blob)
    sha.update(struct.pack("<I", len(blob)))

hdr = struct.pack(
    "<8s10I16s512s32s1024s",
    BOOT_MAGIC,
    len(kernel),  a.base + a.kernel_offset,
    len(ramdisk), a.base + a.ramdisk_offset,
    0,            a.base + a.second_offset,   # no second-stage
    a.base + a.tags_offset,
    page,
    0, 0,                                     # header_version=0, os_version=0
    a.board.encode()[:16].ljust(16, b"\0"),
    cmdline.ljust(512, b"\0"),
    sha.digest().ljust(32, b"\0"),
    b"\0" * 1024,                             # extra_cmdline
)

def pad(f): f.write(b"\0" * ((page - f.tell() % page) % page))

with open(a.output, "wb") as f:
    f.write(hdr); pad(f)
    f.write(kernel); pad(f)
    f.write(ramdisk); pad(f)

print(f"wrote {a.output}: kernel={len(kernel)} ramdisk={len(ramdisk)} pagesize={page}")
