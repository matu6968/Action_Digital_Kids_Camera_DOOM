#ifndef APPOTECH_SD_H
#define APPOTECH_SD_H
#include <stdint.h>
#define APX_SD_SECTOR_SIZE 512u
enum apx_sd_result { APX_SD_OK=0, APX_SD_TIMEOUT=-1, APX_SD_CARD_ERROR=-2,
    APX_SD_RANGE=-3, APX_SD_UNSUPPORTED=-4, APX_SD_NOT_READY=-5, APX_SD_WRITE_PROTECTED=-6 };
struct apx_sd_info {
    uint64_t sectors;
    uint32_t ocr,rca,clock_hz,reads,writes,errors,last_status,last_command;
    uint8_t cid[16],csd[16]; /* Standard wire order, most significant byte first. */
    unsigned initialized,high_capacity,write_protected;
    uint32_t retries,last_failure_command,last_failure_status;
};
/* Independent native SD controller, polling and single-block RX/TX DMA.
 * One-bit bus at <=24 MHz; no erase or switch-function commands.
 * Waits service USB and maintenance, never recursively dispatch APXD commands.
 * FatFs and SD APIs must be called from one cooperative task.
 * Transient sector errors allow at most two controller/card reinitializations
 * and retries, verifying CID/capacity before replaying the same sector.
 * Errors/retries remain visible in info; persistent failures stop the card. */
int apx_sd_init(void);
int apx_sd_read(uint32_t sector,void *buffer,unsigned count);
/* Synchronous CMD24: copies each sector to owned aligned DMA memory and waits
 * for programming/READY_FOR_DATA before returning. Buffer must be in DDR.
 * Do not write raw sectors beneath a mounted filesystem. */
int apx_sd_write(uint32_t sector,const void *buffer,unsigned count);
int apx_sd_sync(void);
const struct apx_sd_info *apx_sd_get_info(void);
/* Change the idle controller clock, rounded down; no transfer may be active. */
int apx_sd_set_clock(unsigned maximum_hz);
void apx_sd_shutdown(void);
#endif
