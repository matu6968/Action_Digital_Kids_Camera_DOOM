#include <appotech/sd.h>
#include <ff.h>
#include <diskio.h>
#include <stdlib.h>
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/buttons.h>
void ff_appotech_progress(void) { apx_usb_poll();apx_buttons_poll();apx_maintenance(); }
DSTATUS disk_status(BYTE drive) {
    return drive?STA_NOINIT:((apx_sd_get_info()->write_protected?STA_PROTECT:0)|
                            (apx_sd_get_info()->initialized?0:STA_NOINIT));
}
DSTATUS disk_initialize(BYTE drive) {
    if(drive)return STA_NOINIT;
    if(!apx_sd_get_info()->initialized)apx_sd_init();
    return disk_status(drive);
}
DRESULT disk_read(BYTE drive,BYTE *buffer,LBA_t sector,UINT count) {
    if(drive||!buffer||!count)return RES_PARERR;
    if(!apx_sd_get_info()->initialized)return RES_NOTRDY;
    int result=apx_sd_read(sector,buffer,count);
    return result==APX_SD_OK?RES_OK:result==APX_SD_RANGE?RES_PARERR:RES_ERROR;
}
DRESULT disk_write(BYTE drive,const BYTE *buffer,LBA_t sector,UINT count) {
    if(drive||!buffer||!count)return RES_PARERR;
    if(!apx_sd_get_info()->initialized)return RES_NOTRDY;
    int result=apx_sd_write(sector,buffer,count);
    return result==APX_SD_OK?RES_OK:result==APX_SD_RANGE?RES_PARERR:
           result==APX_SD_WRITE_PROTECTED?RES_WRPRT:RES_ERROR;
}
DRESULT disk_ioctl(BYTE drive,BYTE command,void *buffer) {
    if(drive)return RES_PARERR;
    if(!apx_sd_get_info()->initialized)return RES_NOTRDY;
    switch(command) {
    case CTRL_SYNC:return apx_sd_sync()==APX_SD_OK?RES_OK:RES_ERROR;
    case GET_SECTOR_COUNT:
        if(!buffer||apx_sd_get_info()->sectors>UINT32_MAX)return RES_PARERR;
        *(LBA_t *)buffer=(LBA_t)apx_sd_get_info()->sectors;return RES_OK;
    case GET_SECTOR_SIZE:if(!buffer)return RES_PARERR;*(WORD *)buffer=512;return RES_OK;
    case GET_BLOCK_SIZE:if(!buffer)return RES_PARERR;*(DWORD *)buffer=1;return RES_OK;
    default:return RES_PARERR;
    }
}
void *ff_memalloc(UINT size) { return malloc(size); }
void ff_memfree(void *pointer) { free(pointer); }
