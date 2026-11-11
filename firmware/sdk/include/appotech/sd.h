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
    unsigned high_speed;  /* 1 = the card is in CMD6 high-speed mode and clocked at 48 MHz */
    uint32_t streams;     /* open-ended CMD18 reads started (a stream serves many calls) */
    uint32_t wstreams;    /* open-ended CMD25 writes started */
    uint32_t last_write_status; /* CONTROL after the last written block: bits 22:21 = 10b is the card's "accepted" token, bit 16 a transfer error */
    unsigned bus_width;   /* data lines in use: 1 or 4 */
    unsigned bus_probe;   /* what the last four-bit attempt came to (enum apx_sd_wide); 0 = not tried (board limited to one line) */
    uint32_t bus_probe_detail; /* failure detail: for APX_SD_WIDE_MISMATCH byte index + 1 | four-bit byte << 8 | one-bit byte << 16; else the low byte of the driver error */
};
/* Four-bit bus probe results. The driver reads the SD status (ACMD13) on one line, switches the card (ACMD6) and the controller (CONTROL bit 2), reads it again and
 * requires the same bytes (and DAT_BUS_WIDTH = 4); any other outcome puts the card back on one line and remembers it, per card, so a board that does not wire
 * DAT1..DAT3 simply runs one bit wide. */
enum apx_sd_wide { APX_SD_WIDE_NOT_TRIED=0, APX_SD_WIDE_OK=1, APX_SD_WIDE_NARROW_READ=2, APX_SD_WIDE_REFUSED=3, APX_SD_WIDE_READ=4, APX_SD_WIDE_MISMATCH=5 };
/* Independent native SD controller, polling and single-block RX/TX DMA.
 * One data line, or four where the board allows it (APX_SD_BUS_WIDTH_MAX in board.h) and the card passes the probe above (info.bus_width). The clock is 144 MHz/(divider+1) with an 8-bit divider (measured). Cards are identified at 562.5 kHz, then run at
 * 24 MHz; a card that supports high speed (CMD6) is switched to it and clocked at 48 MHz (APX_SD_HIGH_SPEED=0 disables that).
 * Reads of more than one sector, or continuing the previous read, use an open-ended CMD18 that stays open between calls (one
 * DMA re-arm per sector); while it is open the next sector is already being received into a driver block (read-ahead, two 512-byte blocks,
 * one copy per sector), so a caller that does other work between sectors does not wait for the card; any other card operation stops it first. Reads go straight into a 16-byte aligned caller buffer.
 * Writes of more than one sector, or continuing the previous write, use an open-ended CMD25 that stays open between calls (one DMA arm and the card's busy
 * wait per sector, no command); an isolated sector is one CMD24. Each block's data-response token is checked. A write stream that is still open has not
 * necessarily been programmed: apx_sd_sync(), any read, and apx_sd_poll() (idle for 50 ms; apx_maintenance() calls it) close it. No erase command.
 * Waits service USB and maintenance, never recursively dispatch APXD commands.
 * FatFs and SD APIs must be called from one cooperative task.
 * Transient sector errors allow at most two controller/card reinitializations
 * and retries, verifying CID/capacity before replaying the same sector.
 * Errors/retries remain visible in info; persistent failures stop the card. */
int apx_sd_init(void);
int apx_sd_read(uint32_t sector,void *buffer,unsigned count);
/* Writes sectors from a DDR buffer (DMA straight from it when it is 16-byte aligned, otherwise through an owned bounce block). Returns when the card has
 * accepted every block (and, for a single CMD24 sector, finished programming it); a CMD25 stream stays open, see above: call apx_sd_sync() to make sure
 * everything is programmed. Do not write raw sectors beneath a mounted filesystem. */
int apx_sd_write(uint32_t sector,const void *buffer,unsigned count);
/* Close any open write stream and wait until the card has programmed everything. */
int apx_sd_sync(void);
/* Closes a write stream that has been idle; called by apx_maintenance() when the SD driver is linked. */
void apx_sd_poll(void);
const struct apx_sd_info *apx_sd_get_info(void);
/* Change the idle controller clock, rounded down; no transfer may be active. */
int apx_sd_set_clock(unsigned maximum_hz);
void apx_sd_shutdown(void);
#endif
