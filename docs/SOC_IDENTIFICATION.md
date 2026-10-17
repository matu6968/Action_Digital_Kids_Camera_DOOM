# SoC identification: the AppoTech JT5295

The chip is marked **JT5295** (QFN64, next to a Zbit-compatible `25Q16ETJG` SPI flash; the board silkscreen reads
`XYX-001-5295C_V03`). It has no public datasheet. This page says how it was identified as an OpenRISC part with an AppoTech
(Buildwin) BootROM, and what is still inferred.

## USB identities

| State | VID:PID | Name | How to get there |
|---|---|---|---|
| Stock firmware, first | `0219:3280` | UVC webcam | camera plugged in |
| Stock firmware | `1908:3283` | `Buildwin Media-Player`, UVC + mass storage | after the stock firmware has enumerated |
| BootROM loader | `1908:3319` | `BLDR v1.00`, SCSI inquiry `BuildWin Video050Loader` | vendor SCSI command `0xda`, or forced (see FLASHING.md) |
| DOOM / SDK image | `1908:3319` | `JT5295 SDK Debug` | after a launch; vendor-class bulk interface, "APXD" protocol |

The Buildwin vendor SCSI commands (`0xcd` run code in the stock firmware, `0xda` reboot into the loader, `0xcb` read/write RAM
and call a function in the BootROM) were first documented in a [Habr article on a different Buildwin kids' camera](https://habr.com/ru/companies/ruvds/articles/779626/). They work the same here; `tools/python/apxtool/` implements them.

## CPU

* The stock flash image, the BootROM (64 KiB, mapped at `0x00100000`, dumped to `dumps/bootrom_0x00100000_64K.bin`) and the RAM
  window all disassemble as **OpenRISC 1000 (ORBIS32)** once each 32-bit word is read **little endian**; the common 32-bit
  OR1K toolchains are big endian. [`ghidra-appotech-jt5295`](https://github.com/matu6968/ghidra-appotech-jt5295) (a Ghidra language module) adds `OpenRISC:LE:32:default`.
* **No branch delay slots in practice.** A taken `l.j`/`l.jal`/`l.bf` skips the instruction after it, and `l.jal` links to PC+4.
  Compiler-generated code with delay slots returned garbage until the toolchain was told not to fill them
  (`-fno-delayed-branch`, which the patched GCC makes the default for `-mappotech`).
* **`l.mul`, `l.muli`, `l.div`, `l.divu` exist** (about 1900 uses in the stock image) and were checked against software results
  on the camera (`appotech-sdk/examples/doom-probe`). No `l.mulu`, `l.mac` or 64-bit result forms are used by the vendor.
* Custom `l.cust*` opcodes appear in the dump; this port does not use them.

## Memory and clocks

* RAM `0x02000000..0x023fffff` (4 MiB) answers to reads and writes; the lower 1.5 MiB (`0x02000000..0x0217ffff`) is usable by an
  SDK image after takeover (checked with a save/write/read/restore pattern test).
* The stock application runs from RAM copied out of flash at `0x2200 -> 0x02000000`; the SDK and this port load at `0x02180000`.
* PLL and counter evidence fits a **32.768 kHz crystal** reference (`0x9804` is a 14-bit counter of it) and a **96 MHz** loader
  clock, **144 MHz** after the SDK's PLL pair (`apx_clock_init`). The 144 MHz figure is inferred from relative throughput and
  audio/SD/UART timing; it was not measured with a scope.

## What is not known

* Whether the chip is a variant of a known AppoTech/Buildwin part. No other public documentation of the register map exists
  beyond what the SDK in `firmware/sdk/src/*.c` records (every register use is traced to a stock-firmware address in
  `appotech-sdk/research/`).
* Exceptions only partly reach software: faults the SDK catches (bus error, illegal instruction, ...) are recorded and reset the
  chip, but `l.sys`, `l.trap`, reserved opcodes and unmapped reads stall or reset without a trace.
* GPIO interrupts, four-bit SD, high-speed SD, and the camera sensor's full control are not used by this port.
