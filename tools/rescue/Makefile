# SRAM payload for the forced BootROM loader (tools/rescue.sh). Same toolchain as the firmware.
PREFIX ?= $(firstword $(wildcard $(CURDIR)/../../toolchain/install $(CURDIR)/../../../appotech-sdk/toolchain/install))
CROSS ?= $(PREFIX)/bin/or1k-appotech-elf-
OUT := build
FLAGS := -Os -g -mappotech -fno-delayed-branch -ffreestanding -fno-builtin -fno-pic -ffunction-sections -fdata-sections -Wall -Wextra -Werror
all: $(OUT)/rom-rescue.bin
$(OUT):
	mkdir -p $@
$(OUT)/rom-rescue.elf: rom-rescue.S rom-rescue.c rom-rescue.ld | $(OUT)
	$(CROSS)gcc $(FLAGS) -nostdlib -Trom-rescue.ld -Wl,--gc-sections,--build-id=none,-Map,$(OUT)/rom-rescue.map rom-rescue.S rom-rescue.c -o $@
	$(CROSS)size $@
$(OUT)/rom-rescue.bin: $(OUT)/rom-rescue.elf
	$(CROSS)objcopy -O binary $< $@
clean:
	rm -rf $(OUT)
