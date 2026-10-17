# DOOM for the Action kids camera (AppoTech JT5295, OpenRISC).
#
#   make                 build/doom.elf + build/doom.manifest.json
#   make clean
#
# The toolchain is the patched or1k-appotech-elf GCC, see ../docs/TOOLCHAIN.md. It is looked up in ../toolchain/install
# first and in ../../appotech-sdk/toolchain/install second; override with PREFIX=/path or CROSS=/path/or1k-appotech-elf-.
#
# Run it from RAM over USB (safe, nothing is written to flash):   ../tools/launch.sh
# Install it for cold boot (overwrites the camera firmware):      see ../docs/FLASHING.md

ROOT    := $(abspath ..)
PREFIX  ?= $(firstword $(wildcard $(ROOT)/toolchain/install $(ROOT)/../appotech-sdk/toolchain/install))
CROSS   ?= $(PREFIX)/bin/or1k-appotech-elf-
CC      := $(CROSS)gcc
AR      := $(CROSS)gcc-ar
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size

BUILD   := build
SDK     := sdk
BOARD   := boards/action-camera
GBA     := GBADoom
PORT    := port

# SDK options (see sdk/README in the SDK repository for what they mean)
OPT_SDK  ?= -Os
HEAP_SIZE ?= 12288
USB_VID  ?= 0x1908
USB_PID  ?= 0x3319
CPU_MHZ  ?= 144
# 1 = print frame/timing statistics through the USB log ring every few seconds
DOOM_STATS ?= 1

# --- sources ---------------------------------------------------------------------------------------------------------
SDK_SRCS  := $(filter-out $(SDK)/src/startup.S $(SDK)/src/image.c,$(wildcard $(SDK)/src/*.c $(SDK)/src/*.S))
SDK_OBJS  := $(patsubst $(SDK)/src/%,$(BUILD)/sdk/%.o,$(SDK_SRCS))
FATFS_OBJS:= $(BUILD)/sdk/ff.o $(BUILD)/sdk/ffunicode.o

# Engine: everything except the GBA-only IWAD blob and the Maxmod audio layer (replaced by port/i_sound_jt5295.c).
ENGINE_SKIP := $(GBA)/source/doom_iwad.c $(GBA)/source/i_audio.c
ENGINE_SRCS := $(filter-out $(ENGINE_SKIP),$(wildcard $(GBA)/source/*.c))
PORT_SRCS   := $(wildcard $(PORT)/*.c) $(wildcard $(PORT)/*.S)
DOOM_OBJS   := $(patsubst %,$(BUILD)/doom/%.o,$(notdir $(ENGINE_SRCS) $(PORT_SRCS)))
VPATH       := $(GBA)/source:$(PORT)

LIBC    := $(PREFIX)/or1k-appotech-elf/lib/libc.a
LIBGCC  := $(shell $(CC) -print-libgcc-file-name)

BUILD_ID := $(shell { cat $(SDK)/src/* $(SDK)/include/appotech/*.h $(SDK)/board/board.h $(SDK)/fatfs/ffconf.h \
    $(PORT)/* $(BOARD)/board.h linker/doom.ld $(GBA)/source/*.c $(GBA)/include/*.h Makefile; \
    printf '%s\n' '$(OPT_SDK)' '$(HEAP_SIZE)' '$(EXTRA_CFLAGS)' '$(USB_VID)' '$(USB_PID)' '$(CPU_MHZ)' '$(DOOM_STATS)'; } | sha256sum | cut -c1-16)

COMMON := -mappotech -fno-delayed-branch -fno-pic -ffunction-sections -fdata-sections -fno-common -Wall -Wextra
SDKDEFS := -DAPX_NO_STORAGE_COMMAND -DAPX_BUILD_ID='"$(BUILD_ID)"' -DAPX_USB_VID=$(USB_VID) -DAPX_USB_PID=$(USB_PID) -DAPX_USB_FORCE_FULL_SPEED=0 \
    -DAPX_CPU_MHZ=$(CPU_MHZ) -DAPX_SPI_TRACE=0 -DAPX_TRACE_KEEP_WDT=0 -DAPX_LAUNCH_SETTLE_TICKS=0 -DAPX_LAUNCH_TRACE_MODE=0 \
    -DAPX_NO_CACHE_ENABLE=0 -DAPX_USB_ENUM_TIMEOUT_TICKS=0 -DAPX_SPI_TRACE_RING_ONLY=0 -DAPX_LVGL_INSTRUMENT=0 \
    -DAPX_LAUNCH_READS=0 -DAPX_LAUNCH_WDT_STOP=0 '-DAPX_LAUNCH_READ_LIST=0xd800'
SDKINC  := -I$(SDK)/include -I$(SDK)/board -I$(SDK)/fatfs
SDK_CFLAGS := -g $(OPT_SDK) $(COMMON) -Werror $(SDKINC) $(SDKDEFS)
# Objects the linker script places by file name must stay out of LTO (same list as the SDK's own Makefile).
SDK_LTO_CFLAGS := $(SDK_CFLAGS) -flto -fuse-linker-plugin
LTO_PLACED := image uart adc battery spi camera camera_files audio audio_files microphone microphone_files jpeg jpeg_files ffunicode

EXTRA_CFLAGS ?=
ENGINE_CFLAGS := $(EXTRA_CFLAGS) -g -O2 -std=gnu11 $(COMMON) -I$(GBA)/include -I$(PORT) -I$(BOARD) $(SDKINC) \
    -Wno-switch-outside-range -Wno-unused -Wno-sign-compare -Wno-incompatible-pointer-types -Wno-implicit-fallthrough \
    -Wno-missing-field-initializers -Wno-old-style-declaration -Wno-enum-conversion -Wno-uninitialized -Wno-maybe-uninitialized -Wno-switch -Wno-unused-parameter -Wno-array-bounds -Wno-stringop-overflow -DDOOM_STATS=$(DOOM_STATS) \
    -DAPX_BUILD_ID='"$(BUILD_ID)"'

# APX_BUILD_ID is baked into a few objects; rebuild exactly those when the id changes (stamp file keeps its mtime otherwise).
.DEFAULT_GOAL := all
.PHONY: all clean info FORCE
FORCE:
$(BUILD)/buildid.stamp: FORCE | $(BUILD)
	@echo $(BUILD_ID) > $@.tmp; if cmp -s $@.tmp $@; then rm $@.tmp; else mv $@.tmp $@; fi
$(BUILD)/sdk/debug.c.o $(BUILD)/sdk/trace.c.o $(BUILD)/doom/main.c.o: $(BUILD)/buildid.stamp
all: $(BUILD)/doom.manifest.json

$(BUILD) $(BUILD)/sdk $(BUILD)/doom:
	mkdir -p $@

# SDK objects ----------------------------------------------------------------------------------------------------------
$(BUILD)/sdk/%.c.o: $(SDK)/src/%.c | $(BUILD)/sdk
	$(CC) $(if $(filter $*,$(LTO_PLACED)),$(SDK_CFLAGS),$(SDK_LTO_CFLAGS)) -MMD -MP -c $< -o $@
$(BUILD)/sdk/%.S.o: $(SDK)/src/%.S | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/sdk/startup.o: $(SDK)/src/startup.S | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/sdk/ff.o: $(SDK)/fatfs/ff.c | $(BUILD)/sdk
	$(CC) $(SDK_LTO_CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/sdk/ffunicode.o: $(SDK)/fatfs/ffunicode.c | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/libappotech.a: $(SDK_OBJS)
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/libfatfs.a: $(FATFS_OBJS)
	rm -f $@ && $(AR) rcs $@ $^

# Engine + port objects (placed into EXTRA_RAM / CODEC_RAM by linker/doom.ld through the build/doom/ path) ---------------
$(BUILD)/doom/%.c.o: %.c | $(BUILD)/doom
	$(CC) $(ENGINE_CFLAGS) $(if $(filter i_main.c,$(notdir $<)),-Dmain=doom_main) -MMD -MP -c $< -o $@
$(BUILD)/doom/%.S.o: %.S | $(BUILD)/doom
	$(CC) $(ENGINE_CFLAGS) -c $< -o $@

# Link -----------------------------------------------------------------------------------------------------------------
$(BUILD)/doom.elf: $(BUILD)/sdk/startup.o $(DOOM_OBJS) $(BUILD)/libappotech.a $(BUILD)/libfatfs.a linker/doom.ld $(LIBC) $(LIBGCC)
	$(CC) $(SDK_LTO_CFLAGS) -Wl,-u,stdout,-u,sbrk,-u,__mulsi3,-u,__udivsi3,-u,__divsi3,-u,__umodsi3,-u,__modsi3 -nostartfiles -Tlinker/doom.ld \
	    -Wl,-e,_start,--gc-sections,--build-id=none,--defsym=APX_HEAP_SIZE=$(HEAP_SIZE),-Map,$(BUILD)/doom.map -o $@ \
	    $(BUILD)/sdk/startup.o $(DOOM_OBJS) -Wl,--start-group $(BUILD)/libappotech.a $(BUILD)/libfatfs.a -Wl,--end-group
	$(OBJDUMP) -d $@ > $(BUILD)/doom.disasm
	$(SIZE) $@

$(BUILD)/doom.manifest.json: $(BUILD)/doom.elf ../tools/manifest.py ../tools/check_clock_elf.py
	python3 ../tools/check_clock_elf.py $< --output $(BUILD)/doom.clock-check.json
	python3 ../tools/manifest.py $< --prefix "$(PREFIX)" --build-id $(BUILD_ID) --vid $(USB_VID) --pid $(USB_PID) \
	    --opt=$(OPT_SDK) --lto=1 --heap $(HEAP_SIZE) --full-speed 0 --cpu-mhz $(CPU_MHZ)

info:
	@echo "PREFIX=$(PREFIX)"; echo "CROSS=$(CROSS)"; echo "BUILD_ID=$(BUILD_ID)"; echo "engine sources: $(words $(ENGINE_SRCS))"

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/sdk/*.d) $(wildcard $(BUILD)/doom/*.d)
