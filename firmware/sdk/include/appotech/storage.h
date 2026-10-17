#ifndef APPOTECH_STORAGE_H
#define APPOTECH_STORAGE_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <ff.h>
/* FatFs R0.16, drive 0, single cooperative task, UTF-8 paths.
 * Native read/write backend. Close all handles and unmount before card removal.
 * An I/O error invalidates SD state; discard old handles before remounting. */
FRESULT apx_storage_mount(void);
/* Sync/unmount/format reject open files or directories with FR_LOCKED.
 * After a controller failure, unmount abandons the invalid volume/cache. */
FRESULT apx_storage_sync(void);
FRESULT apx_storage_unmount(void);
/* FatFs FM_FAT/FM_FAT32/FM_EXFAT/FM_ANY, optionally FM_SFD; cluster_bytes=0
 * selects FatFs defaults. Creates a new volume, erasing the existing directory.
 * The 4 KiB workspace is allocated from the SDK heap before changing the card. */
FRESULT apx_storage_format(unsigned format,uint32_t cluster_bytes);
FATFS *apx_storage_volume(void);
/* Picolibc FILE over FatFs: r/w/a, optional b/+/x (x for w only).
 * Mount first. fflush commits data and metadata; append stays at EOF on writes. */
FILE *apx_fopen(const char *path,const char *mode);
enum apx_storage_command { APX_STORAGE_INFO=0, APX_STORAGE_BLOCK=1,
    APX_STORAGE_VOLUME=2, APX_STORAGE_LIST=3, APX_STORAGE_FILE=4, APX_STORAGE_CODEPAGE=5,
    APX_STORAGE_BLOCK_WRITE=6, APX_STORAGE_FILE_WRITE=7, APX_STORAGE_MKDIR=8,
    APX_STORAGE_UNLINK=9, APX_STORAGE_RENAME=10, APX_STORAGE_LABEL=11,
    APX_STORAGE_FORMAT=12, APX_STORAGE_UNMOUNT=13, APX_STORAGE_SYNC=14 };
uint32_t apx_storage_prepare_bootrom(void);
uint32_t apx_storage_command(unsigned command,const uint8_t *input,size_t length,
                             uint8_t *output,size_t *output_length);
#endif
