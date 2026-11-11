/* Mass-storage backing for APX_USB_MODE_MSC: the TinyUSB MSC class callbacks over a pluggable disk
 * (include/appotech/usb_msc.h), with a built-in FAT12 RAM disk as the default.
 *
 * TinyUSB hands read10/write10 at most CFG_TUD_MSC_EP_BUFSIZE bytes at a time (512 here), at a byte offset inside the
 * transfer, so each callback covers part of one block; the code below handles any alignment.
 */
#include "tusb.h"
#include <appotech/usb_msc.h>
#include <string.h>

#if CFG_TUD_MSC

#define RAMDISK_BASE ((uint8_t *)(uintptr_t)0x02000000u)
#define RAMDISK_BLOCKS 1024u
#define RAMDISK_BLOCK_SIZE 512u

static struct apx_usb_msc_disk disk;
static bool use_ramdisk = true, ramdisk_ready;
static bool ejected, was_ready, ever_ready;
static uint32_t blocks_read, blocks_written;

/*--------------------------------------------------------------------+
 * RAM disk: FAT12, 1024 sectors, 1 sector per cluster, 64 root entries
 *--------------------------------------------------------------------*/

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }

/* FAT12 entry n holds the 12-bit value v. */
static void fat12_set(uint8_t *fat, unsigned n, unsigned v) {
  uint8_t *p = fat + n + n / 2;
  if (n & 1) { p[0] = (uint8_t)((p[0] & 0x0f) | (v << 4)); p[1] = (uint8_t)(v >> 4); }
  else { p[0] = (uint8_t)v; p[1] = (uint8_t)((p[1] & 0xf0) | (v >> 8)); }
}

void apx_usb_msc_ramdisk_format(void) {
  enum { RESERVED = 1, FAT_SECTORS = 3, FATS = 2, ROOT_ENTRIES = 64, ROOT_SECTORS = ROOT_ENTRIES * 32 / 512,
         FIRST_FAT = RESERVED, ROOT = RESERVED + FATS * FAT_SECTORS, DATA = ROOT + ROOT_SECTORS };
  static const char text[] = "JT5295 SDK USB mass storage.\r\nThis is a 512 KiB RAM disk; it is cleared when the camera resets.\r\n";
  uint8_t *d = RAMDISK_BASE;
  memset(d, 0, (size_t)RAMDISK_BLOCKS * RAMDISK_BLOCK_SIZE);

  uint8_t *boot = d; /* sector 0: BIOS parameter block */
  boot[0] = 0xeb; boot[1] = 0x3c; boot[2] = 0x90;
  memcpy(boot + 3, "APXSDK  ", 8);
  put16(boot + 11, RAMDISK_BLOCK_SIZE);
  boot[13] = 1; put16(boot + 14, RESERVED); boot[16] = FATS; put16(boot + 17, ROOT_ENTRIES);
  put16(boot + 19, RAMDISK_BLOCKS); boot[21] = 0xf8; put16(boot + 22, FAT_SECTORS);
  put16(boot + 24, 32); put16(boot + 26, 64); put32(boot + 28, 0);
  boot[36] = 0x80; boot[38] = 0x29; put32(boot + 39, 0x4a543532); /* "JT52" volume serial */
  memcpy(boot + 43, "APXSDK RAM ", 11);
  memcpy(boot + 54, "FAT12   ", 8);
  boot[510] = 0x55; boot[511] = 0xaa;

  for (unsigned f = 0; f < FATS; ++f) {
    uint8_t *fat = d + (FIRST_FAT + f * FAT_SECTORS) * RAMDISK_BLOCK_SIZE;
    fat12_set(fat, 0, 0xff8); fat12_set(fat, 1, 0xfff);
    fat12_set(fat, 2, 0xfff); /* README.TXT: one cluster, end of chain */
  }
  uint8_t *root = d + ROOT * RAMDISK_BLOCK_SIZE;
  memcpy(root, "APXSDK RAM ", 11); root[11] = 0x08; /* volume label */
  uint8_t *file = root + 32;
  memcpy(file, "README  TXT", 11); file[11] = 0x20;
  put16(file + 14, 0x0000);                           /* created: 00:00:00 */
  put16(file + 16, 0x5a21); put16(file + 18, 0x5a21); /* created / accessed date: 2025-01-01, fixed (the camera has no clock) */
  put16(file + 22, 0x0000); put16(file + 24, 0x5a21); /* written time / date */
  put16(file + 26, 2); put32(file + 28, sizeof(text) - 1);
  memcpy(d + DATA * RAMDISK_BLOCK_SIZE, text, sizeof(text) - 1);
  ramdisk_ready = true;
}

static int ramdisk_read(void *context, uint32_t lba, void *buffer, uint32_t blocks) {
  (void)context;
  memcpy(buffer, RAMDISK_BASE + (size_t)lba * RAMDISK_BLOCK_SIZE, (size_t)blocks * RAMDISK_BLOCK_SIZE);
  return 0;
}

static int ramdisk_write(void *context, uint32_t lba, const void *buffer, uint32_t blocks) {
  (void)context;
  memcpy(RAMDISK_BASE + (size_t)lba * RAMDISK_BLOCK_SIZE, buffer, (size_t)blocks * RAMDISK_BLOCK_SIZE);
  return 0;
}

static const struct apx_usb_msc_disk ramdisk = {
  .block_count = RAMDISK_BLOCKS, .block_size = RAMDISK_BLOCK_SIZE, .writable = true,
  .read = ramdisk_read, .write = ramdisk_write, .ready = NULL, .context = NULL };

static const struct apx_usb_msc_disk *current(void) {
  if (use_ramdisk) {
    if (!ramdisk_ready) apx_usb_msc_ramdisk_format();
    return &ramdisk;
  }
  return &disk;
}

void apx_usb_msc_set_disk(const struct apx_usb_msc_disk *new_disk) {
  if (new_disk) { disk = *new_disk; use_ramdisk = false; }
  else use_ramdisk = true;
  ejected = false;
}

/* For src/tusb/msc_sd.c: a different card may have a different size. */
void apx_usb_msc_update_capacity(uint32_t blocks) { disk.block_count = blocks; }

uint32_t apx_usb_msc_blocks_read(void) { return blocks_read; }
uint32_t apx_usb_msc_blocks_written(void) { return blocks_written; }

/* Called by usb_tusb.c when the MSC mode is entered. */
void apx_usb_msc_enter(void) {
  ejected = false;
  was_ready = ever_ready = false;
  blocks_read = blocks_written = 0;
  (void)current();
}

/*--------------------------------------------------------------------+
 * TinyUSB MSC callbacks
 *--------------------------------------------------------------------*/

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
  (void)lun;
  memcpy(vendor_id, "AppoTech", 8);
  memcpy(product_id, "JT5295 SDK Disk ", 16);
  memcpy(product_rev, "1.00", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  const struct apx_usb_msc_disk *d = current();
  const bool ready = !ejected && !(d->ready && d->ready(d->context) != 0);
  if (!ready) {
    was_ready = false;
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00); /* medium not present */
    return false;
  }
  if (!was_ready && ever_ready) { /* it was gone and is back: tell the host once that the medium may have changed */
    was_ready = true;
    tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00);
    return false;
  }
  was_ready = ever_ready = true;
  return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
  (void)lun;
  const struct apx_usb_msc_disk *d = current();
  *block_count = d->block_count;
  *block_size = d->block_size;
}

static void flush_disk(void) {
  const struct apx_usb_msc_disk *d = current();
  if (d->flush) (void)d->flush(d->context);
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
  (void)lun; (void)power_condition;
  if (load_eject) { flush_disk(); ejected = !start; } /* the host ejected the medium; presented again when the mode is re-entered */
  return true;
}

/* A WRITE(10) has been answered: close the SD driver's open write stream so the data is programmed, not just accepted (the next write starts a new one). */
void tud_msc_write10_complete_cb(uint8_t lun) { (void)lun; flush_disk(); }

bool tud_msc_is_writable_cb(uint8_t lun) {
  (void)lun;
  return current()->writable;
}

/* Is the medium there? Asked before every data command, not only on TEST UNIT READY: a host that keeps reading or writing never asks, and a disk whose card was pulled
 * (the SD driver has shut it down) is only brought back by the ready hook trying again, which it does at most once a second. A medium that is gone is reported as
 * "not ready, medium not present" (the host stops trying and reports no medium); when it returns, the next TEST UNIT READY says "medium may have changed". */
static bool medium_present(uint8_t lun, const struct apx_usb_msc_disk *d) {
  if (d->ready && d->ready(d->context) != 0) {
    was_ready = false;
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00);
    return false;
  }
  if (!was_ready && ever_ready) { /* it was gone (or failed) and is back: the command is refused once with "medium may have changed", the host retries */
    was_ready = true;
    tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00);
    return false;
  }
  return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  (void)lun;
  const struct apx_usb_msc_disk *d = current();
  if (!medium_present(lun, d)) return -1;
  const uint32_t bs = d->block_size;
  if ((uint64_t)lba * bs + offset + bufsize > (uint64_t)d->block_count * bs) return -1;
  if (offset == 0 && bufsize % bs == 0) { /* the common case: whole blocks, straight into TinyUSB's buffer */
    if (d->read(d->context, lba, buffer, bufsize / bs) != 0) {
      was_ready = false; /* the card failed under the command (pulled, or an error that shut it down): the host revalidates when it is back */
      tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00); /* unrecovered read error */
      return -1;
    }
    blocks_read += bufsize / bs;
    return (int32_t)bufsize;
  }
  /* A partial block: read the block(s) it touches into a bounce buffer. Only reached if CFG_TUD_MSC_EP_BUFSIZE is not
   * a multiple of the block size. */
  static uint8_t bounce[512] TU_ATTR_ALIGNED(4);
  TU_ASSERT(bs <= sizeof(bounce) && offset + bufsize <= bs, -1);
  if (d->read(d->context, lba, bounce, 1) != 0) {
    tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);
    return -1;
  }
  memcpy(buffer, bounce + offset, bufsize);
  blocks_read += 1;
  return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  (void)lun;
  const struct apx_usb_msc_disk *d = current();
  const uint32_t bs = d->block_size;
  if (!d->writable || !d->write) {
    tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00); /* write protected */
    return -1;
  }
  if (!medium_present(lun, d)) return -1;
  if ((uint64_t)lba * bs + offset + bufsize > (uint64_t)d->block_count * bs) return -1;
  if (offset == 0 && bufsize % bs == 0) {
    if (d->write(d->context, lba, buffer, bufsize / bs) != 0) {
      was_ready = false;
      tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0c, 0x00); /* write error */
      return -1;
    }
    blocks_written += bufsize / bs;
    return (int32_t)bufsize;
  }
  static uint8_t bounce[512] TU_ATTR_ALIGNED(4);
  TU_ASSERT(bs <= sizeof(bounce) && offset + bufsize <= bs, -1);
  if (d->read(d->context, lba, bounce, 1) != 0) return -1; /* read-modify-write of one block */
  memcpy(bounce + offset, buffer, bufsize);
  if (d->write(d->context, lba, bounce, 1) != 0) return -1;
  blocks_written += 1;
  return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
  (void)buffer; (void)bufsize;
  if (scsi_cmd[0] == 0x35) { flush_disk(); return 0; } /* SYNCHRONIZE CACHE (10): answered by flushing, no data stage */
  /* Anything TinyUSB does not answer itself: illegal request, invalid command operation code. */
  tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
  (void)scsi_cmd;
  return -1;
}

#else /* APX_USB_MSC=0: the public API still links */

void apx_usb_msc_set_disk(const struct apx_usb_msc_disk *disk) { (void)disk; }
void apx_usb_msc_ramdisk_format(void) {}
int apx_usb_msc_use_sd(void) { return -1; }
int apx_usb_msc_use_sd_writable(void) { return -1; }
uint32_t apx_usb_msc_blocks_read(void) { return 0; }
uint32_t apx_usb_msc_blocks_written(void) { return 0; }

#endif
