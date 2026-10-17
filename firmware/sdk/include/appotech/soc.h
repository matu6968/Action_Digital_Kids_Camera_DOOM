#ifndef APPOTECH_SOC_H
#define APPOTECH_SOC_H
#include <stdint.h>
#include <stddef.h>
#define APX_RAM_BASE 0x02000000u
#define APX_RAM_END 0x02400000u
#define APX_RUNTIME_BASE 0x02180000u
#define APX_RUNTIME_END 0x021a0000u
#define APX_STACK_BASE 0x02200000u
#define APX_STACK_END 0x02204000u
static inline void apx_cpu_relax(void) { __asm__ volatile("l.nop"); }
static inline uint8_t apx_mmio_read8(uintptr_t address) { return *(volatile uint8_t *)address; }
static inline uint16_t apx_mmio_read16(uintptr_t address) { return *(volatile uint16_t *)address; }
static inline uint32_t apx_mmio_read32(uintptr_t address) { return *(volatile uint32_t *)address; }
static inline void apx_mmio_write8(uintptr_t address,uint8_t value) { *(volatile uint8_t *)address=value; }
static inline void apx_mmio_write16(uintptr_t address,uint16_t value) { *(volatile uint16_t *)address=value; }
static inline void apx_mmio_write32(uintptr_t address,uint32_t value) { *(volatile uint32_t *)address=value; }
static inline uint32_t apx_spr_read(uint32_t spr) {
    uint32_t value;
    __asm__ volatile("l.mfspr %0,%1,0" : "=&r"(value) : "r"(spr) : "memory");
    return value;
}
static inline void apx_spr_write(uint32_t spr, uint32_t value) {
    __asm__ volatile("l.mtspr %0,%1,0" :: "r"(spr), "r"(value) : "memory");
}
/* Cooperative hardware maintenance; watchdog policy determines whether to feed. */
void apx_maintenance(void);
uint32_t apx_ticks(void);
/* Deadlines must be less than 2^31 ticks ahead; service ticks every <0.5 s. */
static inline int apx_deadline_expired(uint32_t deadline) { return (int32_t)(apx_ticks()-deadline)>=0; }
void apx_delay_ms(uint32_t milliseconds);
void apx_runtime_init(void);
/* Vendor-verified range maintenance for DDR DMA reads; 16-byte lines.
 * Returns -1 for a range outside DDR. Does not permit CPU writes to a source
 * while a peripheral owns it. The caller must wait for DMA completion. */
int apx_dma_clean(const void *buffer,size_t length);
/* Receive DMA: caller must own complete cache lines (aligned start/length).
 * Use before DMA and after completion; do not access the buffer during DMA. */
int apx_dma_invalidate(void *buffer,size_t length);
/* Write back and drop all cache lines overlapping [start,end) (data cache window starts at APX_RAM_BASE). */
void apx_cache_flush_range(const void *start,const void *end);
_Noreturn void apx_bootrom(void);
#endif
