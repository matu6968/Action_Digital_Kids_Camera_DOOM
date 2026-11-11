# DOOM for the Action kids cameras (AppoTech JT5295, OpenRISC): Vision (default) and Silvergear (thermal printer).
#
#   make                              Action Vision:   build/doom.elf + build/doom.manifest.json
#   make BOARD=silvergear-3222882     Silvergear:      build/silvergear-3222882/doom.elf + doom.manifest.json
#   make clean [BOARD=...]
#
# The toolchain is the patched or1k-appotech-elf GCC, see ../docs/TOOLCHAIN.md. It is looked up in ../toolchain/install
# first and in ../../appotech-sdk/toolchain/install second; override with PREFIX=/path or CROSS=/path/or1k-appotech-elf-.
#
# Run it from RAM over USB (safe, nothing is written to flash):   ../tools/launch.sh [BOARD]
# Install it for cold boot (overwrites the camera firmware):      see ../docs/FLASHING.md

ROOT    := $(abspath ..)
PREFIX  ?= $(firstword $(wildcard $(ROOT)/toolchain/install $(ROOT)/../appotech-sdk/toolchain/install))
CROSS   ?= $(PREFIX)/bin/or1k-appotech-elf-
CC      := $(CROSS)gcc
AR      := $(CROSS)gcc-ar
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size

BOARD   ?= action-vision
BOARDS  := action-vision silvergear-3222882
ifeq ($(filter $(BOARD),$(BOARDS)),)
$(error BOARD must be one of: $(BOARDS))
endif
# Action Vision keeps the historical build/ directory; every other board gets build/<board>/.
BUILD   := $(if $(filter action-vision,$(BOARD)),build,build/$(BOARD))
SDK     := sdk
BOARDDIR:= boards/$(BOARD)
GBA     := GBADoom
PORT    := port

# Where the SDK's peripheral/USB/printer code lives. 1 = in the extra bank, placed by object name (that code then has to stay out of LTO);
# 0 = in the 128 KiB runtime window with everything else ("compact", the SDK's ram-compact.ld idea), all of it LTO. Compact keeps the SPI
# image small: a file-backed segment in the extra bank costs the packed image the ~0.5 MiB zero gap below it, which the Action's 2 MiB
# flash cannot spare (engine overlay + embedded WAD). The Silvergear's printer code does not fit the runtime window.
EXTRA_CODE ?= $(if $(filter action-vision,$(BOARD)),0,1)
ifeq ($(filter $(EXTRA_CODE),0 1),)
$(error EXTRA_CODE must be 0 or 1)
endif

# SDK options (see sdk/README.md for what they mean)
OPT_SDK  ?= -Os
HEAP_SIZE ?= 12288
USB_VID  ?= 0x1908
USB_PID  ?= 0x3319
CPU_MHZ  ?= 144
# 1 = print frame/timing statistics through the USB log ring every few seconds
DOOM_STATS ?= 1
# USB: TinyUSB (the SDK default). The SDK's polled legacy stack is not built here. Mass storage (SD card as a USB drive) costs ~9 KB
# of extra-bank RAM and is off by default.
APX_USB_MSC ?= 0

# --- sources ---------------------------------------------------------------------------------------------------------
# libappotech.a is an archive: only the members the link actually references end up in the image. The SDK sources the DOOM image
# never calls (camera, JPEG, microphone, LVGL glue ...) therefore cost nothing; they are compiled anyway so that a new use is one
# reference away. Per board / per USB stack choices:
DISPLAY_SRC := $(if $(filter silvergear-3222882,$(BOARD)),display_st7789.c,display.c)
# the printer's render/photo/reference files include the Silvergear's board .inc files and are only built there (as in the SDK Makefile)
ONLY_SILVERGEAR := printer_render.c photo_prepare.c printer_reference.S
SDK_C       := $(filter-out usb.c display.c display_st7789.c $(if $(filter silvergear-3222882,$(BOARD)),,$(ONLY_SILVERGEAR)),$(notdir $(wildcard $(SDK)/src/*.c))) $(DISPLAY_SRC)
SDK_S       := $(filter-out startup.S $(if $(filter silvergear-3222882,$(BOARD)),,$(ONLY_SILVERGEAR)),$(notdir $(wildcard $(SDK)/src/*.S)))
SDK_OBJS    := $(patsubst %.c,$(BUILD)/sdk/%.o,$(SDK_C)) $(patsubst %.S,$(BUILD)/sdk/%.o,$(SDK_S))
FATFS_OBJS  := $(BUILD)/sdk/ff.o $(BUILD)/sdk/ffunicode.o
# TinyUSB glue + stack: one unit, partial-linked into usb_all.o under LTO so the linker script can place it by name (see sdk Makefile).
TINYUSB     := $(SDK)/third_party/tinyusb/src
TUSB_UNIT   := $(BUILD)/tusb/dcd_apx.o $(BUILD)/tusb/usb_tusb.o $(BUILD)/tusb/msc_disk.o $(BUILD)/tusb/tusb.o $(BUILD)/tusb/tusb_fifo.o $(BUILD)/tusb/usbd.o
TUSB_SEPARATE :=
ifeq ($(APX_USB_MSC),1)
TUSB_UNIT   += $(BUILD)/tusb/msc_device.o
TUSB_SEPARATE += $(BUILD)/tusb/msc_sd.o
endif

# Engine: everything except the GBA-only IWAD blob and the Maxmod audio layer (replaced by port/i_sound_jt5295.c).
ENGINE_SKIP := $(GBA)/source/doom_iwad.c $(GBA)/source/i_audio.c
ENGINE_SRCS := $(filter-out $(ENGINE_SKIP),$(wildcard $(GBA)/source/*.c))
# Board-specific port files: input_<board>.c is the key mapping, print_<board>.c the print feature (stub where there is no printer).
PORT_ALL    := $(wildcard $(PORT)/*.c) $(wildcard $(PORT)/*.S)
PORT_SRCS   := $(PORT_ALL)
DOOM_OBJS   := $(patsubst %,$(BUILD)/doom/%.o,$(notdir $(ENGINE_SRCS) $(PORT_SRCS)))
VPATH       := $(GBA)/source:$(PORT)

LIBC    := $(PREFIX)/or1k-appotech-elf/lib/libc.a
LIBGCC  := $(shell $(CC) -print-libgcc-file-name)

BUILD_ID := $(shell { cat $(SDK)/src/*.c $(SDK)/src/*.S $(SDK)/src/*.inc $(SDK)/src/*.h $(SDK)/src/tusb/* $(SDK)/include/appotech/*.h \
    $(SDK)/boards/$(BOARD)/* $(SDK)/third_party/fatfs/ff.c $(SDK)/third_party/fatfs/ffconf.h \
    $(PORT)/* $(BOARDDIR)/doom_board.h linker/doom.ld $(GBA)/source/*.c $(GBA)/include/*.h Makefile; \
    printf '%s\n' '$(BOARD)' '$(OPT_SDK)' '$(HEAP_SIZE)' '$(EXTRA_CFLAGS)' '$(USB_VID)' '$(USB_PID)' '$(CPU_MHZ)' '$(DOOM_STATS)' '$(APX_USB_MSC)' '$(EXTRA_CODE)'; } | sha256sum | cut -c1-16)

COMMON := -mappotech -fno-delayed-branch -fno-pic -ffunction-sections -fdata-sections -fno-common -Wall -Wextra
SDKDEFS := -DAPX_NO_STORAGE_COMMAND -DAPX_BUILD_ID='"$(BUILD_ID)"' -DAPX_USB_VID=$(USB_VID) -DAPX_USB_PID=$(USB_PID) -DAPX_USB_FORCE_FULL_SPEED=0 \
    -DAPX_CPU_MHZ=$(CPU_MHZ) -DAPX_SPI_TRACE=0 -DAPX_TRACE_KEEP_WDT=0 -DAPX_LAUNCH_SETTLE_TICKS=0 -DAPX_LAUNCH_TRACE_MODE=0 \
    -DAPX_NO_CACHE_ENABLE=0 -DAPX_USB_ENUM_TIMEOUT_TICKS=0 -DAPX_SPI_TRACE_RING_ONLY=0 -DAPX_LVGL_INSTRUMENT=0 \
    -DAPX_LAUNCH_READS=0 -DAPX_LAUNCH_WDT_STOP=0 '-DAPX_LAUNCH_READ_LIST=0xd800' \
    -DAPX_USB_TINYUSB=1 -DAPX_USB_MSC=$(APX_USB_MSC) -DAPX_USB_UVC=0 -DAPX_USB_UAC=0
# src/tusb holds tusb_config.h; TinyUSB's own headers are -isystem so only our code is held to -Werror
SDKINC  := -I$(SDK)/include -I$(SDK)/boards/$(BOARD) -I$(SDK)/third_party/fatfs -I$(SDK)/src/tusb -isystem $(TINYUSB)
SDK_CFLAGS := -g $(OPT_SDK) $(COMMON) -Werror $(SDKINC) $(SDKDEFS)
# Objects the linker script places by file name must stay out of LTO (same list as the SDK's own Makefile).
SDK_LTO_CFLAGS := $(SDK_CFLAGS) -flto -fuse-linker-plugin
LTO_PLACED := uart adc battery spi printer printer_render photo_prepare camera camera_files audio audio_files microphone microphone_files jpeg jpeg_files ffunicode
LTO_PLACED_OBJS := $(if $(filter 1,$(EXTRA_CODE)),$(addprefix $(BUILD)/sdk/,$(addsuffix .o,$(LTO_PLACED))) $(TUSB_SEPARATE))
LD_SCRIPT := $(if $(filter 1,$(EXTRA_CODE)),linker/doom.ld,$(BUILD)/doom-compact.ld)

EXTRA_CFLAGS ?=
ENGINE_CFLAGS := $(EXTRA_CFLAGS) -g -O2 -std=gnu11 $(COMMON) -I$(GBA)/include -I$(PORT) -I$(BOARDDIR) $(SDKINC) \
    -Wno-switch-outside-range -Wno-unused -Wno-sign-compare -Wno-incompatible-pointer-types -Wno-implicit-fallthrough \
    -Wno-missing-field-initializers -Wno-old-style-declaration -Wno-enum-conversion -Wno-uninitialized -Wno-maybe-uninitialized -Wno-switch -Wno-unused-parameter -Wno-array-bounds -Wno-stringop-overflow -DDOOM_STATS=$(DOOM_STATS) \
    -DAPX_BUILD_ID='"$(BUILD_ID)"'

# APX_BUILD_ID is baked into a few objects; rebuild exactly those when the id changes (stamp file keeps its mtime otherwise).
.DEFAULT_GOAL := all
.PHONY: all clean info FORCE
FORCE:
$(BUILD)/buildid.stamp: FORCE | $(BUILD)
	@echo $(BUILD_ID) > $@.tmp; if cmp -s $@.tmp $@; then rm $@.tmp; else mv $@.tmp $@; fi
$(BUILD)/sdk/debug.o $(BUILD)/sdk/trace.o $(BUILD)/doom/main.c.o: $(BUILD)/buildid.stamp
all: $(BUILD)/doom.manifest.json

$(BUILD) $(BUILD)/sdk $(BUILD)/doom $(BUILD)/tusb:
	mkdir -p $@

# SDK objects (named <stem>.o, which is what linker/doom.ld matches) ---------------------------------------------------
$(BUILD)/sdk/%.o: $(SDK)/src/%.c | $(BUILD)/sdk
	$(CC) $(if $(filter $@,$(LTO_PLACED_OBJS)),$(SDK_CFLAGS),$(SDK_LTO_CFLAGS)) -MMD -MP -c $< -o $@
$(BUILD)/sdk/%.o: $(SDK)/src/%.S | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/sdk/startup.o: $(SDK)/src/startup.S | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) -c $< -o $@
$(BUILD)/sdk/ff.o: $(SDK)/third_party/fatfs/ff.c | $(BUILD)/sdk
	$(CC) $(SDK_LTO_CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/sdk/ffunicode.o: $(SDK)/third_party/fatfs/ffunicode.c | $(BUILD)/sdk
	$(CC) $(SDK_CFLAGS) -MMD -MP -c $< -o $@

# TinyUSB: glue (src/tusb) and the stack's own files; the stack is third party, so its warnings do not fail the build.
$(BUILD)/tusb/%.o: $(SDK)/src/tusb/%.c | $(BUILD)/tusb
	$(CC) $(SDK_LTO_CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/tusb/tusb.o: $(TINYUSB)/tusb.c | $(BUILD)/tusb
	$(CC) $(SDK_LTO_CFLAGS) -Wno-error -MMD -MP -c $< -o $@
$(BUILD)/tusb/tusb_fifo.o: $(TINYUSB)/common/tusb_fifo.c | $(BUILD)/tusb
	$(CC) $(SDK_LTO_CFLAGS) -Wno-error -MMD -MP -c $< -o $@
$(BUILD)/tusb/usbd.o: $(TINYUSB)/device/usbd.c | $(BUILD)/tusb
	$(CC) $(SDK_LTO_CFLAGS) -Wno-error -MMD -MP -c $< -o $@
$(BUILD)/tusb/msc_device.o: $(TINYUSB)/class/msc/msc_device.c | $(BUILD)/tusb
	$(CC) $(SDK_LTO_CFLAGS) -Wno-error -MMD -MP -c $< -o $@
$(BUILD)/tusb/msc_sd.o: $(SDK)/src/tusb/msc_sd.c | $(BUILD)/tusb
	$(CC) $(SDK_CFLAGS) -MMD -MP -c $< -o $@
# linker/doom.ld places this code in the extra bank by object name, which LTO would erase: partial-link the LTO objects into one ordinary object.
$(BUILD)/sdk/usb_all.o: $(TUSB_UNIT) | $(BUILD)/sdk
	$(CC) $(SDK_LTO_CFLAGS) -r -nostdlib -nostartfiles -flinker-output=nolto-rel -o $@ $^

$(BUILD)/libappotech.a: $(SDK_OBJS) $(BUILD)/sdk/usb_all.o $(TUSB_SEPARATE)
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/libfatfs.a: $(FATFS_OBJS)
	rm -f $@ && $(AR) rcs $@ $^

# Engine + port objects (placed into EXTRA_RAM / CODEC_RAM by linker/doom.ld through the build/doom/ path) ---------------
$(BUILD)/doom/%.c.o: %.c | $(BUILD)/doom
	$(CC) $(ENGINE_CFLAGS) $(if $(filter i_main.c,$(notdir $<)),-Dmain=doom_main) -MMD -MP -c $< -o $@
$(BUILD)/doom/%.S.o: %.S | $(BUILD)/doom
	$(CC) $(ENGINE_CFLAGS) -c $< -o $@

# Link -----------------------------------------------------------------------------------------------------------------
# compact layout: the same script without the extra-bank code blocks (the sections fall through to .text/.rodata in the runtime window)
$(BUILD)/doom-compact.ld: linker/doom.ld | $(BUILD)
	sed -e '/EXTRA_CODE_BEGIN/,/EXTRA_CODE_END/d' $< > $@
$(BUILD)/doom.elf: $(BUILD)/sdk/startup.o $(DOOM_OBJS) $(BUILD)/libappotech.a $(BUILD)/libfatfs.a $(LD_SCRIPT) $(LIBC) $(LIBGCC)
	$(CC) $(SDK_LTO_CFLAGS) -Wl,-u,stdout,-u,sbrk,-u,__mulsi3,-u,__udivsi3,-u,__divsi3,-u,__umodsi3,-u,__modsi3 -nostartfiles -T$(LD_SCRIPT) \
	    -Wl,-e,_start,--gc-sections,--build-id=none,--defsym=APX_HEAP_SIZE=$(HEAP_SIZE),-Map,$(BUILD)/doom.map -o $@ \
	    $(BUILD)/sdk/startup.o $(DOOM_OBJS) -Wl,--start-group $(BUILD)/libappotech.a $(BUILD)/libfatfs.a -Wl,--end-group
	$(OBJDUMP) -d $@ > $(BUILD)/doom.disasm
	$(SIZE) $@

$(BUILD)/doom.manifest.json: $(BUILD)/doom.elf ../tools/manifest.py ../tools/check_clock_elf.py
	python3 ../tools/check_clock_elf.py $< --output $(BUILD)/doom.clock-check.json
	python3 ../tools/manifest.py $< --prefix "$(PREFIX)" --build-id $(BUILD_ID) --vid $(USB_VID) --pid $(USB_PID) \
	    --opt=$(OPT_SDK) --lto=1 --heap $(HEAP_SIZE) --full-speed 0 --cpu-mhz $(CPU_MHZ) --board $(BOARD)

info:
	@echo "BOARD=$(BOARD)"; echo "BUILD=$(BUILD)"; echo "PREFIX=$(PREFIX)"; echo "CROSS=$(CROSS)"; echo "BUILD_ID=$(BUILD_ID)"; echo "engine sources: $(words $(ENGINE_SRCS))"

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/sdk/*.d) $(wildcard $(BUILD)/doom/*.d) $(wildcard $(BUILD)/tusb/*.d)
