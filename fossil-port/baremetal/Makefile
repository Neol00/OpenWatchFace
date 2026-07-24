# fossil-port bare-metal build.
#
#   make                    # PLATFORM=qemu (default): ELF for qemu-system-arm
#   make PLATFORM=gen4      # flat binary + gzip for Android boot.img packing
#   make run                # build for qemu and boot it (Ctrl-A X to quit)
#
# Toolchain: arm-none-eabi-gcc (Arm GNU toolchain). Windows: install from
# developer.arm.com or `winget install Arm.GnuArmEmbeddedToolchain`;
# qemu via `winget install SoftwareFreedomConservancy.QEMU`.

PLATFORM ?= qemu
CROSS    ?= arm-none-eabi-

ifeq ($(PLATFORM),qemu)
  PLAT_DEF  := PLAT_BOARD_QEMU_VIRT
  LINK_BASE := 0x40008000
else ifeq ($(PLATFORM),gen4)
  PLAT_DEF  := PLAT_BOARD_FOSSIL_GEN4
  LINK_BASE := 0x80008000
else
  $(error unknown PLATFORM '$(PLATFORM)' (qemu|gen4))
endif

BUILD := build/$(PLATFORM)

CFLAGS := -mcpu=cortex-a7 -marm -mfpu=neon-vfpv4 -mfloat-abi=hard \
          -ffreestanding -fno-builtin -nostdlib -O2 -g \
          -Wall -Wextra -D$(PLAT_DEF) -Iplatform
ASFLAGS := $(CFLAGS)
LDFLAGS := -nostdlib -Wl,--build-id=none -Wl,--defsym=LINK_BASE=$(LINK_BASE) \
           -T platform/linker.ld -Wl,-Map=$(BUILD)/owf.map

SRC_C := main.c platform/console.c platform/ramlog.c platform/timer.c \
         platform/uart_pl011.c platform/uart_msm.c \
         platform/msm_dsi.c platform/dsi_panel.c platform/msm_mdp3.c platform/msm_i2c.c platform/touch_raydium.c
SRC_S := platform/startup.S
OBJ   := $(patsubst %.c,$(BUILD)/%.o,$(SRC_C)) $(patsubst %.S,$(BUILD)/%.o,$(SRC_S))

all: $(BUILD)/owf.elf $(BUILD)/owf.bin
ifeq ($(PLATFORM),gen4)
all: $(BUILD)/owf.bin.gz
endif

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(ASFLAGS) -c $< -o $@

$(BUILD)/owf.elf: $(OBJ) platform/linker.ld
	$(CROSS)gcc $(CFLAGS) $(LDFLAGS) $(OBJ) -lgcc -o $@
	$(CROSS)size $@

$(BUILD)/owf.bin: $(BUILD)/owf.elf
	$(CROSS)objcopy -O binary $< $@

$(BUILD)/owf.bin.gz: $(BUILD)/owf.bin
	gzip -9 -k -f $<

run: all
	qemu-system-arm -M virt,highmem=off -cpu cortex-a7 -m 512M -nographic \
	    -kernel $(BUILD)/owf.elf

clean:
	rm -rf build

.PHONY: all run clean
