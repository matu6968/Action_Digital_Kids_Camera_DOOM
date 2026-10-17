#ifndef APPOTECH_SPI_H
#define APPOTECH_SPI_H
#include <stddef.h>
#include <stdint.h>
#define APX_SPI_CAPACITY 0x200000u
#define APX_SPI_PAGE 256u
#define APX_SPI_SECTOR 4096u
enum apx_spi_error {APX_SPI_OK,APX_SPI_INVALID,APX_SPI_TIMEOUT,APX_SPI_HARDWARE,APX_SPI_BUSY};
struct apx_spi_info {uint32_t jedec,bytes_read,dma_transfers,timeouts;uint8_t initialized,status;};
/* Independent stock controller sequence; static guarded 4KiB bounce buffer.
 * Synchronous, bounded, cooperative, non-reentrant. No mapped-XIP access.
 * Geometry verified for this board's 5e4015 (2MiB) device only. */
int apx_spi_init(void);
int apx_spi_read(uint32_t address,void *buffer,size_t length);
int apx_spi_status(uint8_t *status);
/* Destructive C APIs: explicit calls only, no erase/program at initialization.
 * Program cannot cross a page; erase requires a sector-aligned address.
 * APXD commands 2/3 require an APXF marker per destructive request. */
int apx_spi_program(uint32_t address,const void *buffer,size_t length);
int apx_spi_erase_sector(uint32_t address);
void apx_spi_get_info(struct apx_spi_info *info);
/* SPI-booted images with overlay segments (packed by apxtool/spi_boot.py, e.g. the LVGL port's code in the
 * codec-RAM bank) must call this before running any code or reading any data placed there. Verifies a CRC-32
 * per segment. No-op for USB launches and for images without overlays. Returns an apx_spi_error. */
int apx_spi_overlay_load(void);
/* Flash map of an SPI-booted image: [0, 0x2200) BootROM header and bootstrap, then the load block and any overlay
 * segments, both ending at apx_spi_image_end(); [apx_spi_image_end(), APX_SPI_OVERLAY_TABLE) is free for application
 * data; the sector at APX_SPI_OVERLAY_TABLE holds the overlay table; the last two sectors are the diagnostic logs. */
#define APX_SPI_OVERLAY_TABLE 0x1fc000u
uint32_t apx_spi_image_end(void); /* 4 KiB aligned, 0 if the flash could not be read */
void apx_spi_shutdown(void);
uint32_t apx_spi_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *size);
#endif
