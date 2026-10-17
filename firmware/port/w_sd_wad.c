/*
 * w_sd_wad.c - the WAD lives in RAM, read once from the SD card at start-up.
 *
 * GBADoom reaches lump data by pointer straight into the WAD (&doom_iwad[filepos]), which on the GBA is cartridge ROM.
 * Here doom_iwad points at a RAM region (linker/doom.ld, .wad_buffer) that this file fills from BOARD_WAD_PATH. The
 * WAD must be a GBADoom-processed one (4-byte aligned lumps, converted graphics): see wad/README.md.
 */
#include <stdio.h>
#include <string.h>

#include <appotech/soc.h>
#include <appotech/storage.h>
#include <appotech/sd.h>
#include <appotech/spi.h>

#include "inflate.h"

#include "plat.h"

extern unsigned char __wad_buffer_start[], __wad_buffer_end[];

const unsigned char *const doom_iwad = __wad_buffer_start;
unsigned int doom_iwad_len;

/*
 * Development shortcut: tools/load_wad.py writes the WAD into the buffer over USB (apxtool debug write) and then a
 * 16-byte footer at the very end of the buffer. If the footer is there the SD card is not needed. RAM survives a
 * relaunch of the firmware over USB, so the WAD only has to be sent once per power-on. Garbage from an SPI cold boot does
 * not look like a footer (magic + length + inverted length).
 */
#define FOOTER_SIZE 16u
static unsigned ram_wad_length(void)
{
    const uint32_t *f = (const uint32_t *)(__wad_buffer_end - FOOTER_SIZE);
    const size_t capacity = (size_t)(__wad_buffer_end - __wad_buffer_start) - FOOTER_SIZE;
    if (f[0] != 0x44415744u /* "DWAD" */ || f[1] != ~f[2] || f[1] < 12 || f[1] > capacity) return 0;
    if (memcmp(__wad_buffer_start, "IWAD", 4) != 0) return 0;
    return f[1];
}

static const char *fr_name(FRESULT r)
{
    switch (r) {
    case FR_OK: return "OK";
    case FR_DISK_ERR: return "DISK ERROR";
    case FR_NOT_READY: return "NOT READY";
    case FR_NO_FILE: return "NO FILE";
    case FR_NO_PATH: return "NO PATH";
    case FR_NO_FILESYSTEM: return "NO FILESYSTEM";
    default: return "FS ERROR";
    }
}

/*
 * Fallback: a zlib-compressed copy of the WAD stored in the SPI flash's free area (tools/spi_pack.py appends it right after the
 * image, at apx_spi_image_end()). Header (little endian): "ZWAD", compressed length, raw length, CRC-32 of the raw WAD.
 */
#define ZWAD_MAGIC 0x44415a57u    /* bytes "WZAD" (57 5a 41 44) */

struct zwad_header { uint32_t magic, comp_len, raw_len, crc; };

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    static const uint32_t nib[16] = { 0x00000000,0x1db71064,0x3b6e20c8,0x26d930ac,0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,
                                      0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c };
    crc = ~crc;
    while (n--) { crc ^= *p++; crc = nib[crc & 15] ^ (crc >> 4); crc = nib[crc & 15] ^ (crc >> 4); }
    return ~crc;
}

static struct {
    uint32_t offset, remaining;
    uint8_t chunk[4096];
} flash_in;

static int flash_refill(void *ctx, const uint8_t **b, const uint8_t **e)
{
    (void)ctx;
    if (!flash_in.remaining) return 1;
    const uint32_t n = flash_in.remaining > sizeof flash_in.chunk ? sizeof flash_in.chunk : flash_in.remaining;
    if (apx_spi_read(flash_in.offset, flash_in.chunk, n) != APX_SPI_OK) return 1;
    flash_in.offset += n;
    flash_in.remaining -= n;
    *b = flash_in.chunk;
    *e = flash_in.chunk + n;
    plat_poll();                       /* USB and the watchdog stay serviced during the ~1 s inflate */
    return 0;
}

/* Returns 0 on success, 1 if there is no WAD in flash, -1 if there is one and it is damaged. */
static int load_wad_from_flash(void)
{
    if (apx_spi_init() != APX_SPI_OK) return 1;
    const uint32_t base = apx_spi_image_end();
    struct zwad_header h;
    int rc = 1;
    if (base && apx_spi_read(base, &h, sizeof h) == APX_SPI_OK && h.magic == ZWAD_MAGIC) {
        const size_t capacity = (size_t)(__wad_buffer_end - __wad_buffer_start) - FOOTER_SIZE;
        if (h.raw_len < 12 || h.raw_len > capacity || h.comp_len < 2 || base + sizeof h + h.comp_len > APX_SPI_OVERLAY_TABLE) {
            rc = -1;
        } else {
            plat_status("DOOM", "UNPACKING WAD", "FROM FLASH", 0);
            flash_in.offset = base + (uint32_t)sizeof h;
            flash_in.remaining = h.comp_len;
            const uint32_t t0 = plat_ms();
            const long n = inflate_raw(__wad_buffer_start, h.raw_len, flash_refill, 0);
            if (n != (long)h.raw_len || crc32_update(0, __wad_buffer_start, h.raw_len) != h.crc) {
                printf("flash WAD damaged: inflate returned %ld\n", n);
                rc = -1;
            } else {
                doom_iwad_len = h.raw_len;
                printf("WAD unpacked from flash: %u bytes in %u ms\n", (unsigned)h.raw_len, (unsigned)(plat_ms() - t0));
                rc = 0;
            }
        }
    }
#if !DOOM_STATS
    apx_spi_shutdown();
#endif
    return rc;
}

/* The card is mounted: read BOARD_WAD_PATH into the WAD buffer. On failure returns -1 and leaves a two-line reason in
 * sd_err_1/sd_err_2 (drawn by the caller only if there is nothing to fall back to). */
static const char *sd_err_1, *sd_err_2;

static int load_wad_from_card(void)
{
    const size_t capacity = (size_t)(__wad_buffer_end - __wad_buffer_start) - FOOTER_SIZE;
    FIL fil;
    FRESULT r = f_open(&fil, BOARD_WAD_PATH, FA_READ);
    if (r != FR_OK) {
        printf("open %s failed: %s (%d)\n", BOARD_WAD_PATH, fr_name(r), (int)r);
        sd_err_1 = (r == FR_NO_FILE || r == FR_NO_PATH) ? "NO DOOM.WAD ON CARD" : "CANNOT OPEN DOOM.WAD";
        sd_err_2 = fr_name(r);
        return -1;
    }
    const FSIZE_t size = f_size(&fil);
    if (size < 12 || size > capacity) {
        printf("WAD size %lu does not fit %lu\n", (unsigned long)size, (unsigned long)capacity);
        sd_err_1 = "DOOM.WAD HAS THE";
        sd_err_2 = "WRONG SIZE";
        f_close(&fil);
        return -1;
    }

    unsigned char *dst = __wad_buffer_start;
    FSIZE_t done = 0;
    const uint32_t t0 = plat_ms();
    unsigned last_pct = 101;
    while (done < size) {
        UINT want = (UINT)((size - done) > 32768u ? 32768u : (size - done)), got = 0;
        r = f_read(&fil, dst + done, want, &got);
        if (r != FR_OK || got == 0) {
            printf("read failed at %lu: %s (%d)\n", (unsigned long)done, fr_name(r), (int)r);
            sd_err_1 = "SD READ ERROR";
            sd_err_2 = fr_name(r);
            f_close(&fil);
            return -1;
        }
        done += got;
        const unsigned pct = (unsigned)(done * 100u / size);
        if (pct != last_pct) { last_pct = pct; plat_progress(pct); }
        plat_poll();
    }
    f_close(&fil);
    apx_storage_sync();
    printf("WAD loaded: %lu bytes in %lu ms\n", (unsigned long)size, (unsigned long)(plat_ms() - t0));

    if (memcmp(dst, "IWAD", 4) != 0) {
        sd_err_1 = "DOOM.WAD IS NOT";
        sd_err_2 = "AN IWAD";
        return -1;
    }
    doom_iwad_len = (unsigned)size;
    return 0;
}

int plat_load_wad(void)
{
    FRESULT r = FR_NOT_READY;
    char l2[24];

    if ((doom_iwad_len = ram_wad_length()) != 0) {
        printf("WAD already in RAM (%u bytes), SD card not used\n", doom_iwad_len);
        return 0;
    }

    plat_status("DOOM", "INSERT SD CARD WITH", "DOOM.WAD", 0);
    for (unsigned attempt = 0;; attempt++) {
        r = apx_storage_mount();
        if (r == FR_OK) break;
        snprintf(l2, sizeof l2, "SD: %s", fr_name(r));
        printf("SD mount failed: %s (%d), attempt %u\n", fr_name(r), (int)r, attempt);
        if (attempt == 2) {                    /* no usable card: use the copy in flash if the image has one */
            const int f = load_wad_from_flash();
            if (f == 0) return 0;
            if (f < 0) { plat_status("DOOM", "FLASH WAD DAMAGED", "REFLASH THE IMAGE", 1); return -1; }
        }
        plat_status("DOOM", l2, "RETRYING", 0);
        for (uint32_t t = plat_ms(); plat_ms() - t < 1000u;) {
            plat_poll();
            if ((doom_iwad_len = ram_wad_length()) != 0) {
                printf("WAD arrived in RAM (%u bytes)\n", doom_iwad_len);
                return 0;
            }
        }
    }

    /* A card is in and mounted. Use its DOOM.WAD; if that is missing or unusable fall back to the copy in the firmware image. */
    if (load_wad_from_card() == 0) return 0;
    const int f = load_wad_from_flash();
    if (f == 0) return 0;
    if (f < 0) { plat_status("DOOM", "FLASH WAD DAMAGED", "REFLASH THE IMAGE", 1); return -1; }
    plat_status("DOOM", sd_err_1, sd_err_2, 1);
    return -1;
}
