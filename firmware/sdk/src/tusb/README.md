# TinyUSB port (`USB_STACK=tinyusb`)

Phase 1 of the TinyUSB plan: the APXD debug device on TinyUSB, at parity with `src/usb.c`.

    make BOARD=silvergear-3222882 EXAMPLE=printer-camera-demo   # TinyUSB is the default on both boards
    make EXAMPLE=usb-debug                            # Action camera (BOARD=action-vision is the default board): TinyUSB, output in build/
    make USB_STACK=legacy EXAMPLE=usb-debug           # the old driver (src/usb.c) on the Action: builds in build-legacy/
    make FORCE_FULL_SPEED=1 BUILD=build-tinyusb-fs EXAMPLE=usb-debug

TinyUSB is the default for both boards. The Silvergear's has cold-booted from SPI on real hardware (2026-11-08). **The Action's became the default on 2026-11-10** after extensive RAM-launch testing there (debug-link
soak, mass storage, UVC, UAC, SD card, hot-pull; research/action-vision-compat.md); no TinyUSB image has been cold-booted from SPI on the Action yet, so the first SPI install of a default build is the untested step.
`USB_STACK=legacy` still builds `src/usb.c`. Nothing outside `src/tusb/`, the Makefile and `third_party/tinyusb` (read-only) is involved.

## Files

| File | Role |
| --- | --- |
| `tusb_config.h` | `CFG_TUSB_MCU=OPT_MCU_NONE`; supplies `TUP_DCD_ENDPOINT_MAX=7` (EP0 + EP1..6, measured in `research/usb-musb.md`), high speed, no dedicated HW FIFO, all stock classes off |
| `dcd_apx.c` | Device controller driver: TinyUSB's MUSB EP0 state machine ported onto SPR accessors, plus the PHY bring-up from `src/usb.c`. Polled; `dcd_int_handler()` is the entry point |
| `usb_tusb.c` | Debug-mode descriptors (identical to `src/usb.c`), the APXD bulk-pipe class driver, `tusb_time_millis_api()` from `apx_ticks()`, and the existing `apx_usb_*` API |

Why not TinyUSB's `dcd_musb.c`: it dereferences a memory-mapped register struct; this core is reached through byte SPRs.
Why not the stock vendor class: `apx_usb_send()` posts the caller's buffer without a copy and `apx_usb_receive()` hands out
one packet at a time, which is what `src/debug.c` expects, and it uses less RAM.

## Hardware behaviour carried over from `src/usb.c`

- The EP0 SETUP packet is read from memory at `0x8000`; FIFO data goes one byte per SPR access (`0xa080 + 16*ep`).
- Bulk endpoints are serviced from their CSRs, not from the interrupt flags. EP0 runs on the INTRTX bit 0 flag or an RxPktRdy/SetupEnd/SentStall in CSR0.
- SET_ADDRESS is latched after its status stage; SETUPEND cancels it (upstream's driver does not clear its pending address on SETUPEND).
- The endpoint FIFO is treated as shared between IN and OUT: TXCSRH bit 5 is set only while an IN packet is loaded and sent (`APX_USB_SHARED_FIFO`, default 1). Whether the FIFO really is shared is **not verified**; this only reproduces what the old driver did.
- EP0 stall writes `0x60` (ServicedRxPktRdy | SendStall).
- Only EP1 (512 bytes) can be opened. EP2..EP6 exist but their FIFO sizes are unmeasured, so `dcd_edpt_open()` refuses them (`fifo_limit[]`). Isochronous endpoints are not implemented yet.
- `apx_usb_poll()` guards against re-entry: a callback that waits (and so calls `apx_usb_poll()` through `service()`) must not re-enter the stack.

## Found on hardware

A host halt and clear-halt on EP1 OUT aborts the posted receive and nothing re-posted it, so APXD stopped answering until the next bus
reset. `apx_usb_poll()` now re-arms the receive whenever it is idle. A USB reset (`pyusb` `dev.reset()`) recovers a device stuck that way.

## Results (Action Vision camera, RAM launch, 2026-11-08)

`tools/test_usb.py` (descriptors and other-speed, transfer widths and 64/512/4096 boundaries, malformed frames, protected RAM, endpoint halt
recovery, EP0 stall recovery, then a verified bulk soak):

| Build | Speed | Result | Soak |
| --- | --- | --- | --- |
| legacy (`src/usb.c`) | high | passed | 20,291 cycles, 89.0 MB in 60 s |
| tinyusb | high | passed | 17,564 cycles, 77.0 MB in 60 s (about 13% fewer than legacy) |
| tinyusb, `FORCE_FULL_SPEED=1` | full | passed | 4,728 cycles, 20.7 MB in 30 s |

`apxtool debug bootrom` and `launch.sh` also work from the TinyUSB image. Size: +9.2 KB text, +0.8 KB bss over legacy (`usb-debug`).

## Placement (added for the Silvergear cold-boot test)

`printer-camera-demo` on the Silvergear overflowed the 128 KB runtime window by 9.8 KB with TinyUSB, so `linker/ram.ld` places the TinyUSB and `src/tusb`
objects in the extra-RAM bank (`.usb_code`/`.usb_tables`, like the other driver groups). That needs the objects built without LTO; the Makefile adds
`$(TUSB_OBJECTS)` to `LTO_PLACED_OBJECTS`. The Action `usb-debug` RAM test above ran before this change and has not been repeated with it. On the Silvergear, `printer-camera-demo` with this layout passed the same test from a RAM launch (4,458 cycles in 30 s, legacy 5,172).

## Not done / not verified

- No host model test for `dcd_apx.c` yet (the old `tests/usb_model.c` covers only `src/usb.c`).
- **Cold SPI boot: passed once, on the Silvergear, with a trace image** (owner flashed it; build `c577eb9b`, enumerates at high speed, trace shows the same timeline as legacy; details in `research/silvergear/spi-install-plan.md`). The production image (watchdog on, build `1880dd1d`) passes `tools/test_usb.py` from SPI and, per the owner, cold-boots and renumerates on three replugs, so the Silvergear defaulted to TinyUSB first. **Not yet: a cold SPI boot on the Action**, which is nevertheless on TinyUSB by default since 2026-11-10 (every Action test so far was a RAM launch; see the Makefile comment). 
- `tools/test_usb.py` resets a trace-built SPI image (it re-arms the watchdog that a trace build stops). Run it on production images or RAM launches.
- Suspend/resume counters and remote wakeup are untested.
- The flight-recorder hooks and `APX_USB_ENUM_TIMEOUT_TICKS` are ported (first poll, first SETUP, address applied, configured, PHY wait, PHY register list) but only built, never run.

## Phase 2: USB modes and mass storage

`apx_usb_set_mode(APX_USB_MODE_DEBUG | APX_USB_MODE_MSC)` (`include/appotech/usb.h`) drops off the bus, swaps the descriptor set and reconnects; the host sees a
new device each time (about half a second). Mass storage needs `APX_USB_MSC=1` and is available only on the TinyUSB stack (the legacy stack returns -1).

**Modes are opt-in per example, so a demo pays only for what it uses.** An example lists what it needs in `examples/<name>/features.mk` (today only
`usb-modes` has one: `APX_USB_MSC ?= 1`); otherwise MSC is off and the image carries none of its code. The command line still wins
(`make EXAMPLE=... APX_USB_MSC=1`). Measured on the Silvergear: `usb-debug` is 84.7 KB; with MSC forced on it is 96.0 KB (+11 KB); presenting the SD card adds
another 8 KB (the SD driver), and none of FatFs or its 483 KB Unicode tables. The SD-backed disk lives in its own object (`msc_sd.c`) and the FatFs unmount it
needs is a weak reference, so an image that never calls `apx_usb_msc_use_sd()` never links the SD driver or FatFs. (An earlier version had a strong reference
that dragged FatFs and the tables into every MSC image, 641 KB; found while checking the cost of a demo that does not need SD at all.)
MSC now fits the Silvergear's `printer-camera-demo` (see the memory map note below); UVC will need more.

**Build:** the TinyUSB glue, stack and class drivers are compiled with LTO and partial-linked (`gcc -r -flinker-output=nolto-rel`) into one object, `usb_all.o`,
so the code can still be placed in extra RAM by file name (`linker/ram.ld`, `.usb_code`), which whole-program LTO would not allow. That saves about 1 to 1.8 KB
against building it without LTO. Whole-program LTO saved 5.5 KB but would put the code in the runtime window, which is the region with no room.

**Memory map note:** "extra RAM" is a linker region inside the 4 MB DDR (`0x02220000`, 768 KiB), not SRAM. The NOLOAD application buffers (`.large_buffer`, 229 KB in
`printer-camera-demo`) used to sit in it and left 4 KB free; they now live in their own 512 KiB `LOW_RAM` window at `0x02080000` (free DDR below the runtime
window; costs no flash, it is NOLOAD), which leaves the Silvergear's `printer-camera-demo` **239,656 bytes** of extra RAM free. MSC then fits it (+11 KB, 231 KB free), but
the *runtime window* was the tight spot (2,120 bytes free), so the zero-initialised statics that only the CPU touches (TinyUSB's `.bss`, the debug link's buffers and the log ring) now
live in a NOLOAD `.extra_bss` section in extra RAM (cleared by `apx_runtime_init()` next to `.bss`): `printer-camera-demo` has **16,040 bytes** of runtime window and **225,604 bytes** of extra RAM free.
MSC's static buffers go there too, so enabling it no longer eats the runtime window. The RAM disk of `usb-modes` keeps `0x02000000..0x0207ffff`,
below the new window. The loader treats the window like any other reservation (see apxtool's README, `--reclaim-low-ram`).

Both modes use only EP1, so mass storage needed no new FIFO knowledge. The MSC PID differs so apxtool and `tools/launch.sh` never mistake MSC mode for the debug
link. There is no serial number string (Linux does not need one; Windows will treat every unit alike). In MSC mode the host cannot send APXD requests, so
the application switches back itself (a timer or a key); otherwise only a reset returns the debug link.

Storage is pluggable (`include/appotech/usb_msc.h`, `apx_usb_msc_set_disk`). The default is a 512 KiB FAT12 RAM disk at `0x02000000` (free DDR below the runtime
window) holding README.TXT, formatted on first use. It is volatile and survives mode switches but not a reset. `apx_usb_msc_use_sd()` presents the SD card instead, read-only
(below).

`examples/usb-modes` cycles debug (8 s) -> MSC (120 s) -> debug; `tools/test_usb_msc.py` tests the disk while it is exposed.

### Results (Silvergear, RAM launch of `usb-modes`, 2026-11-08)
- Enumerates at 480 Mbps; 1024 x 512 byte blocks, write protect off; the kernel recognises the FAT12 volume and the desktop automounts it.
- `tools/test_usb_msc.py`: 20 of 20 (O_DIRECT reads/writes: bulk equals sector-at-a-time, 12 size/alignment write cases from 1 to 800 sectors, neighbours untouched,
  file copy in and out through mtools, fsck clean, then the kernel's own FAT driver writes and reads back 200 KB, fsck clean).
- Throughput (RAM disk, one 512-byte block per callback, polled PIO): read about 2.4 MB/s, write about 2.0 MB/s. This is the bandwidth ceiling of the current USB data path
  (one byte per SPR access, one block per callback), not of the SD card. Raising it is Phase 4 work.
- Debug -> MSC -> debug round trips worked repeatedly (APXD answered in each debug window); the kernel log shows no USB errors, resets or I/O errors.
- Test pitfall (cost me one false alarm): the desktop automounts the disk a second or two after it appears; raw writes under a mounted filesystem corrupt it. An earlier
  version of the test wrote 127 sectors at offset 0 (a bad `seek=` past the end of the disk, ignored by uutils dd), overwrote the FATs and root directory under the
  live mount, and the file manager then showed a directory entry with a garbage name. `tools/test_usb_msc.py` unmounts first and checks it was not remounted.

### SD card as the mass-storage disk, read-only (2026-11-08)
`apx_usb_msc_use_sd()` unmounts the SDK's FatFs volume, initialises the card and presents it with `writable = false`: the host sees a write-protected disk, `WRITE(10)`
is refused, and the SD write path is never called. A card that disappears reads as "medium not present"; when it comes back the host gets one UNIT ATTENTION
(medium may have changed) and re-reads the capacity. Read errors answer MEDIUM ERROR. `examples/usb-modes` takes it with `make EXAMPLE=usb-modes EXAMPLE_DEFS=-DAPX_MODES_SD=1`
(new `EXAMPLE_DEFS` make variable) and falls back to the RAM disk if there is no usable card.

Verified on the Silvergear with its 1.84 GiB SDSC card (MBR, FAT16, 132 files), RAM launch, `tools/test_usb_msc_sd.py` (strictly read-only, one `O_RDONLY` open) 16/16:
capacity exact; the kernel sets `ro=1`; the first, a middle and the very last sector equal the debug link's reads; a 1 MiB range read twice and as 256 single sectors is identical;
the partition mounts read-only as "APX SDK"; `SDKTEST.BIN` and a 977 KB photo are bit-identical to copies fetched over the debug link; `fsck.vfat -n` clean (60,343 clusters).
The SD driver's own counters after the session: **59,817 reads, 0 writes, 0 errors, 0 retries**, matching the MSC layer's. Kernel log: no USB, SCSI or FAT errors.
Throughput then: 0.4 to 0.6 MB/s sequential read (the old driver's one CMD17 per sector). After the Phase 4 SD driver (CMD18 streaming, 48 MHz high speed; research/sd.md): **1.88 MB/s**, now limited by the USB FIFO path (2.4 MB/s from RAM), and `tools/test_usb_msc_sd.py` still 16/16.

### Not done / not verified
- Writing to the card through mass storage. The write path is not implemented: it needs the owner's explicit approval, and the SDHC write path is only model-tested.
- Hot removal and re-insertion of the card during a session (needs hands on the camera); the UNIT ATTENTION logic is untested.
- SDHC/SDXC cards (the test card is 2 GB SDSC).
- Action camera: MSC was only built there, never run.
- Windows and macOS hosts; hot removal during a transfer; `START STOP UNIT` eject behaviour beyond marking the medium absent.
- Cold SPI boot with a mode switch (the Silvergear production image still runs `printer-camera-demo`, which does not switch modes).

**Cost of `apx_usb_poll()` and how the SD driver calls it.** One poll costs about 11-12 us with TinyUSB (`dcd_int_handler` 4.9 us, `tud_task_ext` 1.9 us, maintenance 0.35 us), against 3.1 us
for the legacy stack. USB register access itself is cheap (about 0.1 us per SPR), so the cost is software. The SD driver used to call `service()` (this poll, the buttons, the watchdog) on every spin
of a status wait and once per sector, which cost it 0.5 MB/s. `src/sd.c` now spins on the status register and services every 256th spin and every 8th sector (well under a millisecond apart):
the sequential single-sector rate went from 4.10 to 4.64 MB/s and the 512-sector rate from 4.60 to 5.27 MB/s, MSC read from 1.30 to 1.96 MB/s. Layout changes (partial LTO, code or bss placement) moved the results by
about 10% in either direction, so compare numbers measured from the same build only.

**Poll optimisations (2026-11-08).** `dcd_int_handler` caches the INDEX register (`ep_select` skips a select of the endpoint already selected; the cache is invalidated by `reset_state()`),
reads only the low byte of INTRTX/INTRRX (endpoints 1..6), and visits an endpoint only while it has a transfer armed or is halted (`pipe_t.halted` set by `dcd_edpt_stall`, cleared by `dcd_edpt_clear_stall`/open,
so the core's STALLED flag still gets cleared). `apx_usb_poll()` calls `tud_task_ext()` only when `tud_task_event_ready()`. One poll went from about 12.4 us to about 7 us. A USB SPR read costs about 150 ns
(all registers alike), so what remains is mostly instruction fetch from DDR, not register traffic. With that, `sd-bench` reaches 5.56 MB/s (512 sectors in one call), 5.26 MB/s (512 x 1 sequential),
above the legacy stack's 5.19/5.12. MSC over USB stays about 1.9-2.0 MB/s (SD) and 2.4/2.0 MB/s (RAM disk): that is the PIO data path, not the poll.

**Packet RAM path for EP1 (2026-11-08).** The core's FIFO RAM is memory mapped at `0x8000` (see `research/usb-musb.md`, section "Packet RAM"). `dcd_apx.c` stores IN packets into the
EP1 IN buffer (`0x8040`/`0x8240`, alternating, `0x9844` bit 20 picks it) with 32-bit stores, starts them with `0x9844 = length | 1 << 14` plus TxPktRdy, and loads OUT packets from the OUT buffer
(`0x8240`/`0x8440`, `0x9844` bit 26) with 32-bit loads. 512 bytes cost about 8 us instead of 49 us. Switch: `APX_USB_PACKET_RAM` (default 1; 0 = byte-wise FIFO SPR everywhere). Results on the Silvergear:
APXD debug link 1.21 -> 1.47 MB/s (`test_usb.py`, all 5 checks), RAM-disk mass storage read 2.42 -> 3.35 MB/s and write 2.04 -> 3.19 MB/s (`test_usb_msc.py`, 20/20), SD read 1.93 -> 1.69 MB/s
(`test_usb_msc_sd.py`, 16/16; each sector is read from the card synchronously before its packet is queued, so the card's 98 us per sector adds to the per-packet cost: a one-sector read-ahead is the next step).
`tests/usb_model.c` only models the legacy driver; this path is covered by the live tests.

**SD read-ahead (2026-11-08):** SD-backed mass storage sequential read 1.69 -> 2.54 MB/s with the driver's one-sector read-ahead (see `research/sd.md`); RAM disk is 3.35 MB/s, so about 0.8 MB/s of the SD gap remains (per-sector copy, the SCSI command start).

## Camera composite device: UVC + mass storage (APX_USB_UVC=1, `APX_USB_MODE_CAMERA`, 2026-11-08)

`examples/uvc-camera` is the Silvergear's two sensors as two USB webcams next to the SD card (read-only) as a disk, in one composite device (VID:PID 1908:3317, class EF/02/01 with IADs, like stock's UVC
configuration). **The host picks the camera by opening one of two video devices**: interfaces 0/1 are video function 0 = REAR sensor ("JT5295 Rear Camera", EP2), interfaces 2/3 are video function 1 = FRONT
sensor (EP3), interface 4 is mass storage (EP1). **The descriptors follow the board's sensor count (`APX_CAMERA_COUNT`)**: the Action Vision has one sensor and so one video function ("JT5295 Camera", interfaces 0/1, EP2), mass storage is interface 2 and the microphone 3/4, which is how a normal webcam looks (`tools/test_uvc.py REPORT.json 1` tests that layout). On Linux they appear as two /dev/video nodes (plus metadata nodes) named "... Rear" / "... Front". If both are opened the sensors are served in turn, one second each
(a sensor switch takes about 200 ms): measured 5.4 + 5.2 fps. Differences from stock: **bulk instead of isochronous streaming** (the packet RAM has no room for 3 x 1024-byte iso packets, and Linux uvcvideo takes
bulk), no status interrupt endpoint, the microphone is a separate UAC function (next section), one MJPEG format with two frame sizes (640x480 default, 320x240), 15 fps offered. Needs `APX_USB_UVC=1` (declared by the example's `features.mk`; it forces
`APX_USB_MSC=1`); the TinyUSB video class costs about 9 KB of extra RAM.

(Why two functions and not one function with a selector unit: TinyUSB's video class only handles the camera/processing unit requests of its own descriptors, and uvcvideo's input selection needs a selector unit; two functions
is what TinyUSB supports (`CFG_TUD_VIDEO=2`, like its video_capture_2ch example) and every client already knows how to pick a video device.)

- `src/tusb/uvc.c` + `include/appotech/usb_uvc.h`: `apx_usb_uvc_send_frame(stream, jpeg, bytes)` queues a JPEG, with `_busy()`, `_streaming()`, `_frame_index()`, `_session()`, `_frame_interval_us()` per stream. A frame goes out as payloads
  of the negotiated size (up to 16 KB; 2-byte UVC header + data), sent straight from the application's buffer (see the zero-copy paragraph below). No zero-length packet ends a payload, and uvcvideo ends a bulk payload only at a short packet or at the negotiated size, so a frame whose last payload is exactly 512/1024/... bytes long cannot be told from the first part of a longer one, and
  would merge with the next frame header (seen once in ~300 frames): `uvc.c` pads such a frame with one zero byte after EOI (the caller needs one spare byte after the JPEG).
- Bulk has no "stop streaming" signal: after the client quits, the frame in flight stays queued and the next client may receive the tail of it as a partial first frame (no start or end of image; real clients drop it).
  The example stops serving a stream when a frame has been queued for 0.5 s without finishing (`watch_host()`), drops finished frames of such a stream, and resumes on progress or a new VS_COMMIT (`apx_usb_uvc_session()`).
- Pipeline (`examples/uvc-camera/main.c`): three stages side by side in one cooperative loop. **Capture**: the camera's scaled path (it keeps the sensor running; a native `apx_camera_capture_start()` restarts it each
  frame and takes 233 ms) writes NV21 at the committed size into one of two frame buffers (one in the low window, one in codec RAM); **encode**: the hardware JPEG engine (7 ms, 2 ms of it the call) turns the other
  one into one of two 128 KiB JPEG buffers; **send**: a finished JPEG goes out while the next ones are made. **The frame rate does not change with the overlap: 12.4-13 fps at 640x480 and 12.1-12.4 fps at 320x240 either way.**
  The capture itself takes 80 ms (the sensor's frame interval as configured by the stock register table; sensor 0x39, MCLK 24 MHz), cache maintenance in the capture calls is under 1 ms, and arming the next capture
  immediately after the previous one finishes changed nothing, so the sensor is the limit. Faster would mean changing the sensor's frame timing registers (the table is stock's, undocumented).
- Hardware, Silvergear, both sensors: frames 5-80 KB, all decode (libjpeg; ffmpeg prints "overread" notes on some). Reading the SD card through the disk at 1.4-1.9 MB/s during streaming does not disturb the video.
- Test: `tools/test_uvc.py` (41 checks on the Action, ~50 on the Silvergear: enumeration, the video devices and their formats, both sizes on both cameras, every frame a complete decodable JPEG, both cameras open at once, disk reads during streaming, restarts
  with the other size and camera). Needs v4l2-ctl and ffmpeg.
- **Camera controls and frame rate (2026-11-09)**: each video function also has a camera terminal and a processing unit with auto-exposure mode, auto-exposure priority, exposure time, brightness, contrast, gain and
  white balance auto, backed by the sensor's registers (research/camera-controls.md has the register map and the measurements). `uvc.c` carries a driver in front of TinyUSB's video class for them
  (`apx_uvc_driver`, registered as an application driver); the values live in the camera driver (`apx_camera_controls_*`) and are written to the sensor by `apx_camera_service()` from the example's main loop.
  Frame intervals from 33 ms to 1 s are offered (hosts round to the 33.3 ms grid); the committed interval paces the example's frames (`apx_usb_uvc_frame_interval_us()`), and with auto-exposure priority off it also
  caps the exposure so the frame fits. Measured: 29 fps at 320x240 and at 640x480 for a request of 30; 15 and 10 fps are met. The video send path was the limit at 640x480 (2.5 MB/s: an 89 KB frame took 35 ms); it now sends at 7-12 MB/s (next paragraph), so quality 90 fits at 30 fps. The example still keeps each
  stream's JPEG quality (`apx_jpeg_set_encode_quality`, 15..90) inside a per-frame byte budget (committed interval x `APX_UVC_SEND_BYTES_PER_S` x 85%) for slower links.
  The sensor's frame time follows its exposure: indoor light needs 80 ms (12.4 fps), which is why the original 12.4 fps was not a fixed sensor rate.
- **Zero-copy frame sending (2026-11-09)**: `uvc.c` does not use TinyUSB's video streaming code for the frame data. TinyUSB copied every payload into its own buffer first, and a 2 KB copy of just-encoded
  (cold) data costs 230 us on this CPU, which was the 4.5 MB/s ceiling (research/usb-musb.md). Payloads now go out straight from the application's buffer: the 2-byte UVC header is written in place just before the
  data (for later payloads over the two bytes before the chunk, which are already sent), so **`apx_usb_uvc_send_frame()` needs two writable bytes before the JPEG** (examples start the JPEG 32 bytes into its buffer)
  and one spare byte after it; payloads are up to 16 KB (`CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE`, the negotiation limit). TinyUSB still does probe/commit and the descriptors; `uvc_xfer_cb` handles the payload
  completions, a new COMMIT gives the frame in flight up after its current payload, and `apx_uvc_poll()` gives up a frame whose payload vanished (endpoint halted). `examples/uvc-bench` measures the path with fixed
  frames (no camera): 11.6 MB/s, byte-exact (`-DAPX_BENCH_VERIFY=1` re-fills the pattern per frame so the host can check it). The DCD also loads two packets per refill and its unaligned reads are one load per word.
- Not done: status endpoint, isochronous video streaming, more frame sizes/formats, saturation / sharpness / hue / manual white balance temperature (no sensor register for them was found).

## UAC microphone (APX_USB_UAC=1, 2026-11-09)

The camera composite also carries a USB Audio Class 1 microphone like stock's: mono PCM16, **32 kHz** (the rate the SoC's microphone DMA runs at here), isochronous IN **EP4 (0x84)**, wMaxPacketSize 128 (nominal 64 bytes per
millisecond), bInterval 4 (high speed) / 1 (full speed). Interfaces: 0/1 video 0, 2/3 video 1, 4 mass storage, 5 audio control (terminal 1 microphone, **feature unit 2 with mute, volume and automatic gain control**, terminal 3 USB streaming), 6 streaming (alt 0 off, alt 1 on). Linux shows it as the ALSA card "JT5295 SDK Camera".
Needs `APX_USB_UAC=1` (`examples/uvc-camera/features.mk` sets it; it forces UVC + MSC). Files: `src/tusb/uac.c` (a small class driver of its own instead of TinyUSB's audio class),
`include/appotech/usb_uac.h` (`apx_usb_uac_streaming/free/write/get_stats`), iso support in `dcd_apx.c` (`apx_dcd_iso_activate/deactivate`, see research/usb-musb.md for the packet RAM layout it forced).

- **Packets**: `feed()` moves samples from a 2048-sample ring (64 ms) into 64-byte packets. The size is regulated from the ring level (32 samples nominal, +1 below the low mark, up to +32 above the target) because the
  host clock and the microphone clock differ and a late poll leaves several milliseconds behind; a packet never exceeds the endpoint's maximum. Nothing is sent until the ring holds 8 ms (`PREFILL`): an endpoint that answers
  polls with silence right at SET_INTERFACE made Linux capture fail with EIO. After an underrun it re-primes.
- **The endpoint must be closed properly when the host selects alt 0.** In this TinyUSB version `TUP_DCD_EDPT_ISO_ALLOC` is defined unless the port defines `TUP_DCD_EDPT_CLOSE_API`, so `usbd_edpt_close()` does nothing. A packet still queued when the host leaves the
  interface never completes and TinyUSB keeps the endpoint BUSY and CLAIMED for ever: the next start cannot send, the capture fails with EIO after 44 bytes (the WAV header). Linux opens the stream several times per
  `arecord` (alt 1/0 cycles), so this showed up as "the second capture always fails". Fix: `apx_dcd_iso_deactivate()` posts a failed completion event for the pending transfer (`dcd_event_xfer_complete`), which makes usbd clear
  the busy state. This also was the cause of the intermittent EIO when a capture started while a video stream was starting.
- **Microphone source** (`examples/uvc-camera/main.c`, `src/microphone.c`): the DMA is one-shot and never wraps and has no position register, but the samples are written progressively (32 per millisecond). The example fills the
  buffer with a sentinel (-32768), scans for the samples that have replaced it, forwards them to the ring, and when a 64 KiB buffer is full starts the next one (continuous mode: `apx_mic_capture_start_continuous`,
  `apx_mic_rearm`). Cache invalidation must cover whole 16-byte lines (8 samples) or the sentinel scan reads stale data. **Every (re)start produces a DC step** (about -1100 counts relaxing over
  ~500 samples; the microphone path's DC remover starts from zero, also with the analog stage left on): the first `MIC_SKIP` = 512 samples of a buffer are dropped and bridged with a straight line, which leaves one 16 ms smoothed
  spot per 1.024 s. `APX_UVC_TONE=1` streams a test tone instead (used to prove the transport).
- **Handover between DMA buffers (measured 2026-11-09): the microphone clock is exact, the gaps were ours.** One 32768-sample buffer takes 33,555 ticks of the 32.768 kHz clock = 32,000.3 Hz (`mi.ticks`). The first version
  prepared the next buffer (32 K sentinel stores, cache clean + invalidate of 64 KiB) after the previous one finished: a fixed 3.3 ms (109 ticks) per handover, which the core's sample FIFO cannot bridge, so about 106 samples
  were lost per second and the stream looked like 31,895 Hz (a 0.33% pitch error, and a click). Now the idle buffer is prepared a little at a time while the other records (`mic_prepare()`, 2048 samples per call, each cleaned
  and invalidated) and the handover is `apx_mic_rearm_prepared()` (registers only): latency 3 ticks average, 13 maximum, and the stream runs at 31,995 Hz against the tick clock over 60 s (the rest is those 3 ticks). The
  source's service routine is registered with `apx_usb_uac_set_service()` and so also runs inside `apx_delay_ms()` and the camera waits, where a stalled main loop used to leave the handover waiting.
  With the data at the true rate no resampling is needed: the endpoint is declared asynchronous (bmAttributes 0x05), the host takes the packet sizes it gets, and the ring regulation absorbs the small difference between
  the microphone clock and the host's frame clock. An underrun (the 16 ms the first 512 samples of a buffer are skipped) sends nothing instead of a silence packet, so no samples are invented.
- **Level stage** (`apx_usb_uac_write()`, in `uac.c`; `apx_usb_uac_write_raw()` bypasses it): the source hands in the raw samples. A DC blocker (one pole, corner about 20 Hz, state kept times 256: a plain integer state has a
  dead zone that left 50 counts of offset), then mute, then the gain. The host's mixer shows "Mic Capture Switch", "Mic Capture Volume" (0..+45 dB, 1 dB steps) and "Auto Gain Control" (UAC1 feature unit,
  `feature_control()`). With AGC off the volume is the gain. With AGC on it is the highest gain the AGC may use (the default is 36 dB; Linux showed the slider at its maximum, 45, right after enumeration although the device reports 36). The AGC keeps
  the peak of the signal at about 9000 (-11 dBFS): the gain falls at once when a peak would pass that (and a sample over 30000 is clamped and drops the gain immediately), rises at about 17 dB per second, is not raised while
  the signal's peak is below a gate of 96 counts (a quiet room's noise peaks reach about 70, speech 100..250 close to the camera), and in that silence comes back up to the start gain of 18 dB, so a clap does not leave the
  microphone deaf. Levels measured on this unit at raw (unamplified) scale: quiet room rms about 9, speech 15..60 close to the camera, a clap clips raw ±4096 at once, so no single fixed gain suits both (the old fixed x8 clipped every
  clap and left speech at rms 150..500). The AGC settings are constants in `uac.c` (`AGC_TARGET`, `AGC_GATE`, ...); an unusual room may want other values. A replay of the recorded session through a Python model of the stage
  showed claps held below the limiter and the gain recovering to x8 between them; speech tuning on more voices is open.
- **The main loop must keep servicing USB while it waits**: `apx_delay_ms()` and the camera's bus wait call `apx_delay_service()` (weak in runtime.c, `apx_usb_poll()` with TinyUSB). The longest remaining gap is the ~200 ms camera
  select. The example sends one video stream at a time (the iso packets and the bulk stream share the packet RAM).
- Hardware, Silvergear, 2026-11-09: tone and microphone capture are glitch free with both video streams and disk reads running (20 s, 640000 of 640000 samples); repeated captures and a capture started together with a video
  stream (4/4) work; `tools/test_uvc.py` 39/39 (microphone checks include the three mixer controls, volume scaling 18 -> 36 dB, no DC offset, mute = digital silence, AGC not running away), debug link 5/5, RAM-disk MSC 20/20, SD read-write 18/18, host models and the build matrix as before. The recorded signal is a plain
  microphone signal (rms ~160 in a quiet room); tuning on more voices and rooms and Windows/macOS are not done.

## Writable SD over USB (2026-11-08)

`apx_usb_msc_use_sd_writable()` (include/appotech/usb_msc.h) presents the SD card to the host read-write: `WRITE(10)` goes to `apx_sd_write()` (CMD25 streaming, see research/sd.md), and the disk's `flush` hook (`apx_sd_sync()`) runs after every
WRITE(10) (`tud_msc_write10_complete_cb`, which TinyUSB calls after the status is sent), on SYNCHRONIZE CACHE (10) (answered in `tud_msc_scsi_cb`) and on eject, so a finished command is programmed on the card. `usb-modes` built with
`EXAMPLE_DEFS=-DAPX_MODES_SD=2` does it. Measured on the 2 GB SDSC card: 16 MiB written with fsync 1.29 MB/s, read back 2.62 MB/s (`tools/test_usb_msc_sd_rw.py`, 18/18, fsck clean). Not tested: hot removal of the card or cable during a write, other cards, Windows/macOS.
