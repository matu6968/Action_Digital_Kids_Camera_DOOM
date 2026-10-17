/*
 * fixed_hw.h - 16.16 fixed point multiply from the JT5295's 32-bit l.mul.
 *
 * (a * b) >> 16 needs the high half of a 64-bit product, and this core has no multiply that produces it. Split both
 * operands into a signed high and an unsigned low 16-bit half:
 *
 *   a*b >> 16 = (ah*bh << 16) + ah*bl + al*bh + (al*bl >> 16)        (exact, modulo 2^32)
 *
 * Every partial product fits in 32 bits, so four l.mul do it. The result is truncated to 32 bits exactly like the
 * original (fixed_t)((int64)a*b >> 16).
 */
#ifndef FIXED_HW_H
#define FIXED_HW_H
#include <stdint.h>

static inline int32_t fixed_mul_hw(int32_t a, int32_t b)
{
    const int32_t ah = a >> 16, bh = b >> 16;
    const uint32_t al = (uint32_t)a & 0xffffu, bl = (uint32_t)b & 0xffffu;
    int32_t r1, r2, r3, r4;
    __asm__("l.mul %0,%1,%2" : "=r"(r1) : "r"(ah), "r"(bh));
    __asm__("l.mul %0,%1,%2" : "=r"(r2) : "r"(ah), "r"(bl));
    __asm__("l.mul %0,%1,%2" : "=r"(r3) : "r"(al), "r"(bh));
    __asm__("l.mul %0,%1,%2" : "=r"(r4) : "r"(al), "r"(bl));
    return (int32_t)(((uint32_t)r1 << 16) + (uint32_t)r2 + (uint32_t)r3 + ((uint32_t)r4 >> 16));
}
#endif
