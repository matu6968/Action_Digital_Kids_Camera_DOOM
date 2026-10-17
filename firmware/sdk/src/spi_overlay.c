/* Copies the overlay segments of an SPI image (see apxtool/src/apxtool/spi_boot.py) from flash into the
 * codec-RAM bank. The BootROM only loads the main window; the LVGL port's code and assets live above it. */
#include <appotech/spi.h>
#include <appotech/soc.h>
#include <appotech/debug.h>
#include <string.h>
#define TABLE 0x1fc000u
#define MAGIC 0x4f585041u /* 'APXO' */
#define BANK_START 0x022e0000u
#define BANK_END 0x02400000u
#define CHUNK 32768u
#ifndef APX_OVERLAY_PTR
#define APX_OVERLAY_PTR(address) ((uint8_t *)(uintptr_t)(address))
#endif
extern unsigned apx_boot_from_spi(void);
static uint32_t crc32(uint32_t crc,const uint8_t *data,size_t length) {
    static const uint32_t nibble[16]={0x00000000,0x1db71064,0x3b6e20c8,0x26d930ac,0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,
                                      0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c};
    crc=~crc;
    while(length--) { crc^=*data++;crc=nibble[crc&15]^(crc>>4);crc=nibble[crc&15]^(crc>>4); }
    return ~crc;
}
int apx_spi_overlay_load(void) {
    if(!apx_boot_from_spi())return APX_SPI_OK; /* a USB launch already loaded every segment */
    int e=apx_spi_init();if(e)return e;
    uint32_t header[4];
    e=apx_spi_read(TABLE,header,sizeof(header));if(e)return e;
    if(header[0]==0xffffffffu)return APX_SPI_OK; /* erased: this image has no overlay */
    if(header[0]!=MAGIC||header[1]!=1||!header[2]||header[2]>15)return APX_SPI_INVALID;
    uint32_t entries[15*4];
    e=apx_spi_read(TABLE+16,entries,header[2]*16);if(e)return e;
    if(crc32(0,(const uint8_t *)entries,header[2]*16)!=header[3])return APX_SPI_INVALID;
    for(uint32_t i=0;i<header[2];++i) {
        uint32_t offset=entries[4*i],address=entries[4*i+1],length=entries[4*i+2],crc=0;
        if(!length||address<BANK_START||address>=BANK_END||length>BANK_END-address||offset>=TABLE||length>TABLE-offset)return APX_SPI_INVALID;
        uint8_t *destination=APX_OVERLAY_PTR(address);
        for(uint32_t done=0;done<length;) {
            uint32_t count=length-done>CHUNK?CHUNK:length-done;
            e=apx_spi_read(offset+done,destination+done,count);if(e)return e;
            crc=crc32(crc,destination+done,count);done+=count;
            apx_debug_poll();apx_maintenance(); /* USB and the watchdog stay serviced during the ~1 s copy */
        }
        if(crc!=entries[4*i+3])return APX_SPI_HARDWARE;
        apx_cache_flush_range(destination,destination+length);
    }
    /* The copied range may hold code: drop any instruction-cache lines that predate it. Only the
     * instruction cache is touched; the data cache was written back above. */
    if(apx_spr_read(0x2000)&1) { apx_spr_write(0x2000,apx_spr_read(0x2000)&~1u);apx_spr_write(0x2008,0);apx_spr_write(0x2000,apx_spr_read(0x2000)|1u); }
    return APX_SPI_OK;
}

/* First 4 KiB-aligned flash offset past this image (BootROM load block plus overlay segments); everything from
 * there up to APX_SPI_OVERLAY_TABLE is free for application data. 0 if the flash cannot be read. */
uint32_t apx_spi_image_end(void) {
    if(apx_spi_init())return 0;
    uint32_t header[11];
    if(apx_spi_read(0,header,sizeof(header)))return 0;
    uint32_t end=0x2200u+header[10]*512u; /* load sector count lives at +0x28; the image starts after the 8 KiB bootstrap */
    uint32_t table[4];
    if(!apx_spi_read(TABLE,table,sizeof(table))&&table[0]==MAGIC&&table[1]==1&&table[2]&&table[2]<=15) {
        uint32_t entries[15*4];
        if(!apx_spi_read(TABLE+16,entries,table[2]*16)&&crc32(0,(const uint8_t *)entries,table[2]*16)==table[3]) {
            for(uint32_t i=0;i<table[2];++i)if(entries[4*i]+entries[4*i+2]>end)end=entries[4*i]+entries[4*i+2];
        }
    }
    return (end+4095u)&~4095u;
}
