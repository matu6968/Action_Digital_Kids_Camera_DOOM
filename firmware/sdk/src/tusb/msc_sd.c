/* The SD card as the mass-storage disk, read-only (apx_usb_msc_use_sd()).
 *
 * Kept in its own object on purpose: an application that never calls apx_usb_msc_use_sd() must not carry the SD driver, and above all
 * not FatFs and its 483 KB of Unicode tables. The FatFs unmount is a weak reference for the same reason: if FatFs is not linked
 * there is nothing mounted to release. */
#include "tusb.h"
#include <appotech/usb_msc.h>
#include <appotech/sd.h>
#include <appotech/soc.h>
#include <appotech/storage.h>

#pragma weak apx_storage_unmount /* resolved only if the application links the FatFs storage layer for its own reasons */

extern void apx_usb_msc_update_capacity(uint32_t blocks);

static uint32_t next_try;

static int sd_read(void *context, uint32_t lba, void *buffer, uint32_t blocks) {
  (void)context;
  return apx_sd_read(lba, buffer, blocks) == 0 ? 0 : -1;
}

/* Ready when the card is initialised. After a transfer error the driver stops the card; a removed card also reads as
 * stopped. Try to bring it back at most once a second (an init with no card waits for its command timeouts). A card that
 * comes back is reported to the host as a medium change by tud_msc_test_unit_ready_cb() in msc_disk.c. */
static int sd_ready(void *context) {
  (void)context;
  if (!apx_sd_get_info()->initialized) {
    if (!apx_deadline_expired(next_try)) return -1;
    next_try = apx_ticks() + 32768u;
    if (apx_sd_init() != 0) return -1;
  }
  const struct apx_sd_info *info = apx_sd_get_info();
  if (info->sectors == 0 || info->sectors > 0xffffffffu) return -1;
  apx_usb_msc_update_capacity((uint32_t)info->sectors); /* a different card may have a different size */
  return 0;
}

static int sd_write(void *context, uint32_t lba, const void *buffer, uint32_t blocks) {
  (void)context;
  return apx_sd_write(lba, buffer, blocks) == 0 ? 0 : -1;
}

static int sd_flush(void *context) {
  (void)context;
  return apx_sd_sync() == 0 ? 0 : -1;
}

static int use_sd(bool writable) {
  if (apx_storage_unmount && apx_storage_unmount() == FR_LOCKED) return -1; /* files are open: do not pull the card from under them */
  int result = apx_sd_init();
  if (result != 0) return result;
  const struct apx_sd_info *info = apx_sd_get_info();
  if (info->sectors == 0 || info->sectors > 0xffffffffu) return APX_SD_UNSUPPORTED;
  if (writable && info->write_protected) return APX_SD_WRITE_PROTECTED;
  const struct apx_usb_msc_disk sd = { .block_count = (uint32_t)info->sectors, .block_size = APX_SD_SECTOR_SIZE, .writable = writable,
                                       .read = sd_read, .write = writable ? sd_write : NULL, .ready = sd_ready, .flush = writable ? sd_flush : NULL, .context = NULL };
  apx_usb_msc_set_disk(&sd);
  next_try = apx_ticks();
  return 0;
}

int apx_usb_msc_use_sd(void) { return use_sd(false); }
int apx_usb_msc_use_sd_writable(void) { return use_sd(true); }
