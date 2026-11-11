#ifndef APPOTECH_USB_MSC_H
#define APPOTECH_USB_MSC_H
#include <stdbool.h>
#include <stdint.h>

/* Storage behind APX_USB_MODE_MSC (USB_STACK=tinyusb, APX_USB_MSC=1).
 *
 * The callbacks run inside apx_usb_poll(), on the application's stack, one command at a time. A callback may block (the
 * SD driver does) but must not call apx_usb_set_mode(). While one blocks the USB side NAKs, which the host tolerates.
 * Block counts are in `block_size` units (512 for everything the SDK ships). */
struct apx_usb_msc_disk {
    uint32_t block_count;
    uint16_t block_size;
    bool writable;                  /* false: the host sees a write-protected disk and WRITE(10) is refused */
    int (*read)(void *context, uint32_t lba, void *buffer, uint32_t blocks);        /* 0 = ok */
    int (*write)(void *context, uint32_t lba, const void *buffer, uint32_t blocks); /* 0 = ok; may be NULL when !writable */
    int (*ready)(void *context);    /* optional: 0 = medium present and ready, nonzero = "not ready, medium not present" */
    int (*flush)(void *context);    /* optional: make everything written so far durable (0 = ok). Called after every WRITE(10), on SYNCHRONIZE CACHE and on eject */
    void *context;
};

/* Choose what the host sees. NULL (the default) is the built-in RAM disk below. The structure is copied. */
void apx_usb_msc_set_disk(const struct apx_usb_msc_disk *disk);

/* Present the SD card, READ-ONLY: the host sees a write-protected disk and WRITE(10) is refused, so the card is never
 * written. Call it before apx_usb_set_mode(APX_USB_MODE_MSC). It unmounts the SDK's FatFs volume (nothing else may use the
 * card while the host owns it), initialises the card and takes its size from it. Returns 0, or a negative apx_sd_result
 * (no card, unsupported size: more than 2 TiB), or -1 if FatFs files are still open. The card can be removed and
 * re-inserted while the mode is active; the host is told the medium changed. Reads go through apx_sd_read(): one block per
 * command, the SD driver's speed, about 0.5 MB/s. */
int apx_usb_msc_use_sd(void);
/* The same, but the host may WRITE the card: WRITE(10) goes to apx_sd_write() (multi-block CMD25 streaming, so sequential writes run at the card's speed),
 * and every write command, SYNCHRONIZE CACHE and eject end with apx_sd_sync(), so a completed command is programmed. Same return values. The card is
 * written by whatever the host does to the disk: it must not be mounted by the camera at the same time (this call unmounts FatFs), and the host's
 * filesystem is its own business (unmount/eject before unplugging; a pulled card or cable can lose the last write, as with any USB disk). */
int apx_usb_msc_use_sd_writable(void);

/* Built-in RAM disk: 512 KiB FAT12 in the free DDR at 0x02000000 (below the runtime window, which starts at 0x02180000),
 * holding README.TXT. Volatile: the host's writes live until the camera resets. Reformatted by this call; it is also
 * done lazily the first time the RAM disk is presented. */
void apx_usb_msc_ramdisk_format(void);

/* Number of times the host has read / written a block since the mode was entered (diagnostics). */
uint32_t apx_usb_msc_blocks_read(void);
uint32_t apx_usb_msc_blocks_written(void);

#endif
