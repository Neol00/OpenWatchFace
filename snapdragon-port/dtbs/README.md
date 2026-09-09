# Device trees

Every flattened device tree the bare-metal port builds against or the docs
cite. The `.dtb` files are the exact blobs appended to our boot images (aboot
on msm8909 picks a tree by board-id from a DTB appended to the kernel); the
`.dts` files are their `dtc -I dtb -O dts` decompilations, kept so the board
headers can cite them by line.

| File | Device | Origin | Used by |
|---|---|---|---|
| `firefish-stock.dtb` / `.dts` | Fossil Gen 4 (firefish / ray), APQ8009W | dumped from the watch's stock `boot` partition | `tools/mk-bootimg.sh` (pass it as the second argument), `boards/fossil_gen4.h` |
| `skipjack-stock.dtb` / `.dts` | TicWatch C2 and C2+ (skipjack), APQ8009W | dumped from a stock `boot` partition (`tools/extract-dtb.py`); carries both board-ids, so it boots the C2+ too | `tools/mk-bootimg-c2.sh` default, `boards/ticwatch_c2.h` |
| `skipjack-fromsource.dtb` / `.dts` | TicWatch C2 | compiled from Mobvoi's published kernel source; differs from the stock tree in the board-id line only, kept for reference | docs |
| `tunny-stock.dtb` / `.dts` | TicWatch S2 / E2 (tunny), APQ8009W | dumped from the S2's stock `boot` partition | `tools/mk-bootimg-s2.sh` default |
| `sda429-hoki.dtb` | Fossil Gen 6 (hoki), SDA429W | the stock `dtbo`/boot tree, the one the Gen 6 image ships with | `tools/mk-bootimg-gen6.sh` (pass it as the second argument) |
| `sda429-hoki-DEVICE.dtb` | Fossil Gen 6 | the live tree read back from the running watch (`/sys/firmware/fdt`) | reference |
| `sda429-hoki-decompiled.dts` | Fossil Gen 6 | decompilation of `sda429-hoki.dtb`, fully flattened | `boards/fossil_gen6.h`, `platform/*.c` comments |

The Wear 2100 trees (Gen 4, C2, S2) describe the same SoC and PMIC; the SPM
(power sequencer) nodes, which the deep sleep is programmed from, are identical
in all three and differ from the kernel source's dtsi. Always use the device's
own dumped tree, not one compiled from source.
