# Toolchain

The firmware is built with a **patched GCC 16.2.1 / binutils 2.47 / picolibc 1.8.12** that targets `or1k-appotech-elf`. The
patches live in `toolchain/patches/` and are pinned by hash in `toolchain/sources.lock.json`.

## What the patches change

`-mappotech` (the default of the build) means:

| | Upstream `or1k-elf` | `or1k-appotech-elf` |
|---|---|---|
| Endianness | big | **little** (`-mlittle-endian`, `-EL` for as/ld) |
| Branch delay slots | filled by the compiler | **never filled**: `-fno-delayed-branch`, the assembler is told `.nodelay`, `__OR1K_NODELAY__` is defined |
| Call return address | PC+8 | PC+4 |
| Multiply / divide | `l.mul`, `l.div` | **software** (`MASK_SOFT_MUL | MASK_SOFT_DIV`): `__mulsi3`, `__udivsi3`, ... from libgcc |
| Floating point | optional | software |
| libgcc | stock | `appotech-lib1funcs.S`, no-delay variants of the multiply/divide helpers |
| picolibc | - | native-endian profile, no-delay `setjmp`, integer `printf` with `long long` |

The soft multiply/divide are a conservative choice made before it was known that the chip implements `l.mul`/`l.div`. The DOOM
port keeps that compiler and overrides the libgcc entry points instead: [`firmware/sdk/src/hwmuldiv.S`](../firmware/sdk/src/hwmuldiv.S)
(part of the SDK, `make HW_MULDIV=0` turns it off there) defines `__mulsi3`, `__udivsi3`, `__divsi3`, `__umodsi3` and `__modsi3` with the hardware instructions. Because that object is linked
before libgcc, the archive members are never pulled in and every `*`, `/` and `%` in the engine, FatFs and picolibc becomes a call
to a three-instruction function. Measured on the camera with `tools/probe/muldiv-test` (700 known-answer vectors, then timing; loop and
call overhead included): multiply by a small / dense multiplier 1.47 / 3.29 us in software vs 0.24 us; divide by 7 5.4 us vs 0.26 us; divide by
1000003 2.5 us vs 0.26 us; modulo 5.4 us vs 0.38 us. So 6-13x for multiply and 10-20x for divide/modulo.
`FixedMul` goes further and inlines four `l.mul` (`firmware/port/fixed_hw.h`).
A real `-mhard-mul`/`-mhard-div` for `-mappotech` would remove the call overhead too; it needs a one-line change in
`or1k_option_override` and a rebuild of GCC, libgcc and picolibc.

## Building it

You need two upstream checkouts at the baselines in `toolchain/sources.lock.json`
(`gcc`: `71b50793e1e2b12b36baff23c1211e43b8c86ecd`, `binutils-gdb`: `6ce87bbc521cf46eaee9a1f7ef61cee2cdfb3e32`, both OpenRISC
branches of the GNU trees), plus the usual GCC build prerequisites (GMP, MPFR, MPC, flex, bison), Meson and Ninja.

```sh
export APPOTECH_COMPILERS=/path/with/gcc-and-binutils-gdb    # default: ../compilers next to this repository
python3 toolchain/build.py binutils
python3 toolchain/build.py gcc
python3 toolchain/build.py picolibc
python3 toolchain/build.py libgcc
```

The builder verifies the commits and the patch hashes, applies unapplied patches to the checkouts (it never switches branches or
commits), builds out of tree into `toolchain/build/` and installs into `toolchain/install/` - which is where
`firmware/Makefile` looks (it falls back to `../../appotech-sdk/toolchain/install`). `toolchain/export_patches.py` writes
intentional compiler changes back to `toolchain/patches/` and refreshes the hashes.

Check it:

```sh
toolchain/install/bin/or1k-appotech-elf-gcc -mappotech -Os -S -o - -x c - <<< 'int f(int a,int b){return a*b;}' | head
```

should show a `l.jal __mulsi3` followed by a `l.nop`, no delay-slot filling.
