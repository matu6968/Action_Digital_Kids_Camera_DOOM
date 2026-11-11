# The SPI flasher / rescue payload that `apxtool flash` and `apxtool rom-rescue` upload to the camera (sources vendored from
# appotech-sdk/boot, see ../python/VENDORED.md). One position-independent image: it is linked at the SRAM base and again at
# the DDR base, and the build fails unless the two binaries are byte-identical, which is the proof it can be uploaded to either pool.
#
#   make                       build/flasher.bin   (what tools/rescue.sh, tools/install.sh and `apxtool flash` use)
#   make silvergear-restore    build/silvergear-restore.bin / silvergear-full-restore.bin (Silvergear stock restore, see docs/FLASHING.md)
ROOT   := $(abspath ../..)
PREFIX ?= $(firstword $(wildcard $(ROOT)/toolchain/install $(ROOT)/../appotech-sdk/toolchain/install))
CROSS  ?= $(PREFIX)/bin/or1k-appotech-elf-
OUT    := build
SRAM_BASE := 0x2000
DDR_BASE  := 0x02180000
FLAGS := -Os -g -mappotech -fno-delayed-branch -ffreestanding -fno-builtin -fno-pic -fno-jump-tables -ffunction-sections -fdata-sections -fstack-usage -Wall -Wextra -Werror
LINK  := -nostdlib -Tflasher.ld -Wl,--gc-sections,--build-id=none
all: $(OUT)/flasher.bin
silvergear-restore: $(OUT)/silvergear-restore.bin $(OUT)/silvergear-full-restore.bin
$(OUT):
	mkdir -p $@
$(OUT)/entry.o: flasher.S | $(OUT)
	$(CROSS)gcc $(FLAGS) -c $< -o $@
$(OUT)/flasher.o: flasher.c | $(OUT)
	$(CROSS)gcc $(FLAGS) -c $< -o $@
$(OUT)/silvergear-restore.o: flasher.c | $(OUT)
	$(CROSS)gcc $(FLAGS) -DSILVERGEAR_BOOT_RESTORE -c $< -o $@
$(OUT)/silvergear-full-restore.o: flasher.c | $(OUT)
	$(CROSS)gcc $(FLAGS) -DSILVERGEAR_BOOT_RESTORE -DSILVERGEAR_FULL_RESTORE -c $< -o $@
# $(call payload,<name>) links <name>.o at both pool bases and requires identical binaries
define payload
$(OUT)/$(1).elf: $(OUT)/entry.o $(OUT)/$(2).o flasher.ld
	$(CROSS)gcc $(FLAGS) $(LINK) -Wl,--defsym=FLASHER_BASE=$(SRAM_BASE),-Map,$(OUT)/$(1).map $(OUT)/entry.o $(OUT)/$(2).o -o $$@
$(OUT)/$(1)-ddr.elf: $(OUT)/entry.o $(OUT)/$(2).o flasher.ld
	$(CROSS)gcc $(FLAGS) $(LINK) -Wl,--defsym=FLASHER_BASE=$(DDR_BASE) $(OUT)/entry.o $(OUT)/$(2).o -o $$@
$(OUT)/$(1).bin: $(OUT)/$(1).elf $(OUT)/$(1)-ddr.elf
	$(CROSS)objcopy -O binary $(OUT)/$(1).elf $$@
	$(CROSS)objcopy -O binary $(OUT)/$(1)-ddr.elf $(OUT)/$(1)-ddr.bin
	cmp $$@ $(OUT)/$(1)-ddr.bin || { echo "$(1) is not position independent"; rm -f $$@; exit 1; }
endef
$(eval $(call payload,flasher,flasher))
$(eval $(call payload,silvergear-restore,silvergear-restore))
$(eval $(call payload,silvergear-full-restore,silvergear-full-restore))
clean:
	rm -rf $(OUT)
