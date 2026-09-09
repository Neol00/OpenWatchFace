#!/usr/bin/env python3
"""mk-multidtb-boot.py — repack an Android boot image with MANY appended DTBs,
each a copy of one base tree with different aboot matching ids, so a watch
whose board-id / pmic / soc revision we do not know boots it anyway.

Why: aboot on msm8909 matches an appended DTB on qcom,msm-id (platform id +
soc rev), qcom,board-id (variant + subtype) and qcom,pmic-id, and refuses
the image with "dtb not found" otherwise. A new hardware revision (the
TicWatch C2+ with 1 GB) can differ in one of those while being the same
board. aboot scans EVERY appended tree, so appending one copy per candidate
id turns the guess into a sweep. The tree that wins is the one the device
then runs with, and the kernel prints which it got.

Usage:
  mk-multidtb-boot.py <in-boot.img> <base.dtb> <out-boot.img> [--keep-original]
Needs fdtput/dtc (device-tree-compiler) and tools/mkbootimg_v0.py next door.
"""
import os, struct, subprocess, sys, tempfile, shutil

HERE = os.path.dirname(os.path.abspath(__file__))

def fdt_set(dtb, prop, values):
    subprocess.run(["fdtput", "-t", "x", dtb, "/", prop] + [hex(v) for v in values], check=True)

def main():
    if len(sys.argv) < 4: sys.exit(__doc__)
    src, base, out = sys.argv[1:4]
    keep = "--keep-original" in sys.argv
    b = open(src, "rb").read()
    m, ks, ka, rs, ra, ss, sa, ta, ps = struct.unpack("<8sIIIIIIII", b[:40])
    assert m == b"ANDROID!"
    cmdline = b[64:64 + 512].split(b"\0")[0].decode()
    ko = ps; ro = ko + ((ks + ps - 1) // ps) * ps
    kernel = b[ko:ko + ks]; ramdisk = b[ro:ro + rs]
    # strip the appended DTB(s) from the kernel. A d00dfeed can occur by chance
    # inside the compressed zImage, so only accept a candidate whose FDT
    # totalsize runs exactly to the end of the kernel region (the last tree),
    # and walk back over any trees before it.
    if not keep:
        end = len(kernel); cut = None
        while True:
            found = None
            i = kernel.rfind(b"\xd0\x0d\xfe\xed", 0, end)
            while i >= 0:
                tot = struct.unpack(">I", kernel[i + 4:i + 8])[0]
                if i + tot == end: found = i; break
                i = kernel.rfind(b"\xd0\x0d\xfe\xed", 0, i)
            if found is None: break
            cut = found; end = found
        if cut is not None:
            print(f"stripping appended DTB(s) from kernel+0x{cut:x}")
            kernel = kernel[:cut]
    # aboot loads the kernel at ka and the ramdisk at ra: the DTB pile must not
    # reach the ramdisk.
    budget = (ra - ka) - len(kernel) - 0x10000
    tmp = tempfile.mkdtemp()
    variants = []
    # (msm-id list, board-id, pmic-id) candidates around the known 8909w set
    msm_sets = [[0x109, 0, 0x12d, 0], [0x109, 0x10000, 0x12d, 0x10000], [0x109, 0x20000, 0x12d, 0x20000]]
    pmic_sets = [[0x1000b, 0, 0, 0], [0x2000b, 0, 0, 0], [0x0000b, 0, 0, 0], [0x10009, 0, 0, 0]]
    # Candidate order = likelihood: memory-variant subtypes first, then other
    # variants, then PMIC / SoC revisions with the stock board id.
    cands = []
    for st in (0x100, 0x101, 0x102, 0x103, 0x104, 0x106, 0x107, 0x108, 0x109, 0x10a, 0x10b, 0x10c, 0x10d, 0x10e, 0x10f, 0x205, 0x305, 0x005, 0x000, 0x001):
        cands.append(([0x08, st], pmic_sets[0], msm_sets[0]))
    for v in (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x09, 0x0a, 0x0b, 0x10, 0x18, 0x20):
        cands.append(([v, 0x105], pmic_sets[0], msm_sets[0]))
    for pm in pmic_sets[1:]:
        cands.append(([0x08, 0x105], pm, msm_sets[0]))
    for ms in msm_sets[1:]:
        cands.append(([0x08, 0x105], pmic_sets[0], ms))
    cands.append(([0x08, 0x105], pmic_sets[1], msm_sets[1]))
    n = 0
    blob = b""
    for bid, pm, ms in cands:
        d = os.path.join(tmp, f"v{n:03d}.dtb"); shutil.copy(base, d)
        fdt_set(d, "qcom,board-id", bid); fdt_set(d, "qcom,pmic-id", pm); fdt_set(d, "qcom,msm-id", ms)
        one = open(d, "rb").read()
        if len(blob) + len(one) > budget:
            print(f"budget reached after {n} trees ({budget // 1024} KB between kernel and ramdisk)"); break
        blob += one; n += 1
    kpath = os.path.join(tmp, "kernel"); open(kpath, "wb").write(kernel + blob)
    rpath = os.path.join(tmp, "ramdisk"); open(rpath, "wb").write(ramdisk)
    base_addr = ka - 0x8000 if ka >= 0x8000 else 0
    subprocess.run([sys.executable, os.path.join(HERE, "mkbootimg_v0.py"),
                    "--kernel", kpath, "--ramdisk", rpath, "--pagesize", str(ps),
                    "--base", hex(base_addr), "--kernel_offset", hex(ka - base_addr),
                    "--ramdisk_offset", hex(ra - base_addr), "--tags_offset", hex(ta - base_addr),
                    "--cmdline", cmdline, "-o", out], check=True)
    print(f"{out}: {n} DTB variants appended ({len(blob)//1024} KB), kernel @{ka:#x} ramdisk @{ra:#x} tags @{ta:#x}")
    shutil.rmtree(tmp)

if __name__ == "__main__":
    main()
