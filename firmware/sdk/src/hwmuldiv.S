/*
 * hwmuldiv.S - hardware multiply and divide behind libgcc's entry points.
 *
 * -mappotech forces software multiply and divide, so the compiler calls libgcc's __mulsi3 (a shift-and-add loop) and the
 * __udivsi3/__divsi3/__umodsi3/__modsi3 bit loops. The JT5295 implements l.mul, l.div and l.divu: the stock firmware uses them
 * (~1900 times) and a probe run on the camera checked them against the software results (examples/muldiv-test repeats that), about 4x faster for a multiply
 * and 12x for a divide in a loop. Defining the symbols here makes the linker take these instead of libgcc's archive members, so
 * every `*`, `/` and `%` in the SDK, FatFs, picolibc and applications becomes a call to a three-instruction function.
 *
 * Built into libappotech.a by default (make HW_MULDIV=0 for the software routines again). The Makefile also passes -u for each
 * symbol so the member is pulled in even when the only references are compiler-generated libcalls from LTO.
 * Division by zero is not handled, as with the software versions. l.div truncates toward zero like C.
 *
 * Must stay in the main runtime window: SDK code that runs before an SPI image's overlay is copied in calls these.
 */
    .nodelay
    .text

    .global __mulsi3
    .type __mulsi3,@function
__mulsi3:
    l.mul r11,r3,r4
    l.jr r9
    l.nop
    .size __mulsi3,.-__mulsi3

    .global __udivsi3
    .type __udivsi3,@function
__udivsi3:
    l.divu r11,r3,r4
    l.jr r9
    l.nop
    .size __udivsi3,.-__udivsi3

    .global __divsi3
    .type __divsi3,@function
__divsi3:
    l.div r11,r3,r4
    l.jr r9
    l.nop
    .size __divsi3,.-__divsi3

    .global __umodsi3
    .type __umodsi3,@function
__umodsi3:
    l.divu r11,r3,r4
    l.mul r11,r11,r4
    l.sub r11,r3,r11
    l.jr r9
    l.nop
    .size __umodsi3,.-__umodsi3

    .global __modsi3
    .type __modsi3,@function
__modsi3:
    l.div r11,r3,r4
    l.mul r11,r11,r4
    l.sub r11,r3,r11
    l.jr r9
    l.nop
    .size __modsi3,.-__modsi3
