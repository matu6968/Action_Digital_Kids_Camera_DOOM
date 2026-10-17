#ifndef APPOTECH_FATFS_PORT_H
#define APPOTECH_FATFS_PORT_H
#include <ff.h>
/* SDK integration with the pinned FatFs lock table and volume cache.
 * Cooperative access only. Applications normally use storage.h. */
int ff_appotech_volume_busy(const FATFS *fs);
FRESULT ff_appotech_sync_volume(FATFS *fs);
void ff_appotech_progress(void);
#endif
