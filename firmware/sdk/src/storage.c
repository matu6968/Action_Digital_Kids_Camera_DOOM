#include <appotech/storage.h>
#include <appotech/sd.h>
#include <appotech/debug.h>
#include <appotech/usb.h>
#include <appotech/soc.h>
#include <appotech/fatfs_port.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static FATFS volume;
static unsigned mounted;
__attribute__((unused)) static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
__attribute__((unused)) static void put32(uint8_t *p,uint32_t n) { for(unsigned i=0;i<4;++i)p[i]=n>>(8*i); }
FRESULT apx_storage_mount(void) {
    if(mounted&&volume.fs_type&&apx_sd_get_info()->initialized)return FR_OK;
    FRESULT result=apx_storage_unmount();if(result)return result;
    if(!apx_sd_get_info()->initialized&&apx_sd_init())return FR_NOT_READY;
    result=f_mount(&volume,"0:",1);mounted=result==FR_OK;
    return result;
}
FRESULT apx_storage_sync(void) {
    if(!mounted||!apx_sd_get_info()->initialized)return FR_NOT_READY;
    if(ff_appotech_volume_busy(&volume))return FR_LOCKED;
    return ff_appotech_sync_volume(&volume);
}
FRESULT apx_storage_unmount(void) {
    if(mounted&&volume.fs_type&&apx_sd_get_info()->initialized) {
        FRESULT result=apx_storage_sync();if(result)return result;
    }
    FRESULT result=f_mount(NULL,"0:",0);if(!result)mounted=0;return result;
}
FRESULT apx_storage_format(unsigned format,uint32_t cluster_bytes) {
    if(!(format&FM_ANY)||(format&~(FM_ANY|FM_SFD))||
       (cluster_bytes&&(cluster_bytes<512||cluster_bytes>0x1000000u||
        (cluster_bytes&(cluster_bytes-1))||(!(format&FM_EXFAT)&&cluster_bytes>65536))))return FR_INVALID_PARAMETER;
    if(mounted&&ff_appotech_volume_busy(&volume))return FR_LOCKED;
    if(apx_sd_get_info()->write_protected)return FR_WRITE_PROTECTED;
    void *work=malloc(4096);if(!work)return FR_NOT_ENOUGH_CORE;
    FRESULT result=apx_storage_unmount();
    if(!result&&!apx_sd_get_info()->initialized&&apx_sd_init())result=FR_NOT_READY;
    MKFS_PARM options={.fmt=format,.n_fat=2,.align=0,.n_root=512,.au_size=cluster_bytes};
    if(!result)result=f_mkfs("0:",&options,work,4096);
    free(work);
    if(!result) { result=f_mount(&volume,"0:",1);mounted=result==FR_OK; }
    return result;
}
FATFS *apx_storage_volume(void) { return mounted&&volume.fs_type&&apx_sd_get_info()->initialized?&volume:NULL; }
static uint32_t failure(FRESULT result) { return result==FR_OK?APXD_OK:0x100u+result; }
__attribute__((weak,noinline)) uint32_t apx_image_prepare_stop(void){return 0;}
uint32_t apx_storage_prepare_bootrom(void) { uint32_t e=apx_image_prepare_stop();return e?e:failure(apx_storage_unmount()); }
__attribute__((unused)) static int path_valid(const uint8_t *p,size_t n) {
    return n&&n<=1024&&p[n-1]==0&&!memchr(p,0,n-1);
}
#ifndef APX_NO_STORAGE_COMMAND   /* the DOOM image drops the host SD/FatFs command set to save 15 KiB of the runtime window */
uint32_t apx_storage_command(unsigned cmd,const uint8_t *in,size_t len,uint8_t *out,size_t *size) {
    *size=0;
    const struct apx_sd_info *card=apx_sd_get_info();
    if(cmd==APX_STORAGE_INFO&&len==0) {
        int n=snprintf((char *)out,APXD_MAX_PAYLOAD,
            "{\"initialized\":%u,\"sectors\":%llu,\"sector_bytes\":512,\"high_capacity\":%u,"
            "\"clock_hz\":%lu,\"reads\":%lu,\"writes\":%lu,\"errors\":%lu,\"last_command\":%lu,"
            "\"last_status\":%lu,\"mounted\":%u,\"write_protected\":%s,\"retries\":%lu,\"last_failure_command\":%lu,\"last_failure_status\":%lu,\"fatfs\":\"R0.16\"}",
            card->initialized,(unsigned long long)card->sectors,card->high_capacity,
            (unsigned long)card->clock_hz,(unsigned long)card->reads,(unsigned long)card->writes,(unsigned long)card->errors,
            (unsigned long)card->last_command,(unsigned long)card->last_status,mounted,card->write_protected?"true":"false",
            (unsigned long)card->retries,(unsigned long)card->last_failure_command,(unsigned long)card->last_failure_status);
        if(n<0||n>=APXD_MAX_PAYLOAD)return APXD_INVALID;
        *size=n;return APXD_OK;
    }
    if(cmd==APX_STORAGE_CODEPAGE) {
        if(len!=4||get32(in)>65535)return APXD_INVALID;
#if FF_CODE_PAGE == 0
        return failure(f_setcp((WORD)get32(in)));
#else
        return APXD_UNSUPPORTED; /* fixed code page build (DOOM port) */
#endif
    }
    if(cmd==APX_STORAGE_UNMOUNT||cmd==APX_STORAGE_SYNC) {
        if(len)return APXD_INVALID;
        return failure(cmd==APX_STORAGE_UNMOUNT?apx_storage_unmount():apx_storage_sync());
    }
    if(cmd==APX_STORAGE_FORMAT) {
        if(len!=8)return APXD_INVALID;
        return failure(apx_storage_format(get32(in),get32(in+4)));
    }
    if(cmd==APX_STORAGE_BLOCK_WRITE) {
        if(len<516||(len-4)%512||(len-4)/512>7)return APXD_INVALID;
        if(mounted)return APXD_PROTECTED;
        if(!card->initialized&&apx_sd_init())return APXD_IO;
        int result=apx_sd_write(get32(in),in+4,(len-4)/512);
        return result==APX_SD_RANGE?APXD_RANGE:result==APX_SD_WRITE_PROTECTED?failure(FR_WRITE_PROTECTED):
               result?APXD_IO:APXD_OK;
    }
    if(cmd==APX_STORAGE_BLOCK) {
        if(len!=8)return APXD_INVALID;
        uint32_t count=get32(in+4);if(!count||count>8)return APXD_RANGE;
        int result=apx_sd_read(get32(in),out,count);
        if(result)return result==APX_SD_RANGE?APXD_RANGE:APXD_IO;
        *size=count*512;return APXD_OK;
    }
    if(cmd==APX_STORAGE_VOLUME) {
        if(len)return APXD_INVALID;
        if(!mounted||!card->initialized) { FRESULT r=apx_storage_mount();if(r)return failure(r); }
        DWORD serial=0;char label[768];FRESULT result=f_getlabel("0:",label,&serial);
        if(result)return failure(result);
        put32(out,volume.fs_type);put32(out+4,volume.csize);put32(out+8,serial);
        size_t n=strlen(label)+1;memcpy(out+12,label,n);*size=12+n;return APXD_OK;
    }
    if(cmd==APX_STORAGE_FILE_WRITE) {
        if(len<17)return APXD_INVALID;
        uint64_t offset=(uint64_t)get32(in)|(uint64_t)get32(in+4)<<32;
        uint32_t mode=get32(in+8),name_bytes=get32(in+12);
        if(mode>2||name_bytes>len-16||!path_valid(in+16,name_bytes)||(mode&&offset))return APXD_INVALID;
        if(!mounted||!card->initialized)return failure(FR_NOT_READY);
        FIL file;FRESULT result=f_open(&file,(const char *)in+16,
            FA_WRITE|(mode==1?FA_CREATE_NEW:mode==2?FA_CREATE_ALWAYS:FA_OPEN_EXISTING));
        if(result)return failure(result);
        UINT written=0;
        if(offset>f_size(&file)) { f_close(&file);return APXD_RANGE; }
        result=f_lseek(&file,offset);
        if(!result)result=f_write(&file,in+16+name_bytes,len-16-name_bytes,&written);
        FRESULT closed=f_close(&file);if(result)return failure(result);if(closed)return failure(closed);
        put32(out,written);*size=4;return APXD_OK;
    }
    if(cmd==APX_STORAGE_MKDIR||cmd==APX_STORAGE_UNLINK||cmd==APX_STORAGE_LABEL) {
        if(!path_valid(in,len))return APXD_INVALID;
        if(!mounted||!card->initialized)return failure(FR_NOT_READY);
        return failure(cmd==APX_STORAGE_MKDIR?f_mkdir((const char *)in):
                       cmd==APX_STORAGE_UNLINK?f_unlink((const char *)in):f_setlabel((const char *)in));
    }
    if(cmd==APX_STORAGE_RENAME) {
        if(len<6)return APXD_INVALID;
        uint32_t first=get32(in);
        if(first>len-4||!path_valid(in+4,first)||!path_valid(in+4+first,len-4-first))return APXD_INVALID;
        if(!mounted||!card->initialized)return failure(FR_NOT_READY);
        return failure(f_rename((const char *)in+4,(const char *)in+4+first));
    }
    if(cmd!=APX_STORAGE_LIST&&cmd!=APX_STORAGE_FILE)return APXD_UNSUPPORTED;
    size_t prefix=cmd==APX_STORAGE_LIST?4:12;
    if(len<=prefix||!path_valid(in+prefix,len-prefix))return APXD_INVALID;
    if(!mounted||!card->initialized)return failure(FR_NOT_READY);
    const char *path=(const char *)in+prefix;
    if(cmd==APX_STORAGE_LIST) {
        DIR directory;FILINFO entry;uint32_t index=get32(in);
        FRESULT result=f_opendir(&directory,path);if(result)return failure(result);
        uint32_t deadline=apx_ticks()+65536;
        for(uint32_t i=0;;++i) {
            result=f_readdir(&directory,&entry);
            if(result||!entry.fname[0]||i==index)break;
            apx_usb_poll();apx_maintenance();
            if(apx_deadline_expired(deadline)) { result=FR_TIMEOUT;break; }
        }
        FRESULT closed=f_closedir(&directory);if(result)return failure(result);if(closed)return failure(closed);
        if(!entry.fname[0])return APXD_OK;
        put32(out,(uint32_t)entry.fsize);put32(out+4,(uint32_t)(entry.fsize>>32));
        put32(out+8,entry.fattrib);put32(out+12,entry.fdate);put32(out+16,entry.ftime);
        size_t n=strlen(entry.fname)+1;memcpy(out+20,entry.fname,n);*size=20+n;return APXD_OK;
    }
    uint64_t offset=(uint64_t)get32(in)|(uint64_t)get32(in+4)<<32;
    uint32_t count=get32(in+8);if(count>APXD_MAX_PAYLOAD)return APXD_RANGE;
    FIL file;FRESULT result=f_open(&file,path,FA_READ);if(result)return failure(result);
    if(offset>f_size(&file)) { f_close(&file);return APXD_RANGE; }
    result=f_lseek(&file,offset);UINT got=0;
    if(!result)result=f_read(&file,out,count,&got);
    FRESULT closed=f_close(&file);if(result)return failure(result);if(closed)return failure(closed);
    *size=got;return APXD_OK;
}
#endif /* APX_NO_STORAGE_COMMAND */
