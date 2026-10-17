#define _GNU_SOURCE
#include <appotech/storage.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
struct file_cookie { FIL file;unsigned append,writable; };
static int error(FRESULT r) {
    switch(r) {
    case FR_NO_FILE:case FR_NO_PATH:errno=ENOENT;break;
    case FR_EXIST:errno=EEXIST;break;
    case FR_DENIED:errno=EACCES;break;
    case FR_NOT_ENOUGH_CORE:errno=ENOMEM;break;
    case FR_NOT_READY:errno=ENODEV;break;
    case FR_TOO_MANY_OPEN_FILES:errno=EMFILE;break;
    case FR_LOCKED:errno=EBUSY;break;
    case FR_INVALID_NAME:case FR_INVALID_PARAMETER:errno=EINVAL;break;
    case FR_WRITE_PROTECTED:errno=EROFS;break;
    default:errno=EIO;break;
    }
    return -1;
}
static ssize_t file_read(void *opaque,char *buffer,size_t size) {
    struct file_cookie *cookie=opaque;UINT got=0;
    UINT count=size>UINT_MAX?UINT_MAX:(UINT)size;
    FRESULT result=f_read(&cookie->file,buffer,count,&got);
    return result==FR_OK?(ssize_t)got:error(result);
}
static ssize_t file_write(void *opaque,const char *buffer,size_t size) {
    struct file_cookie *cookie=opaque;UINT written=0;
    if(cookie->append) {
        FRESULT result=f_lseek(&cookie->file,f_size(&cookie->file));if(result)return error(result);
    }
    UINT count=size>UINT_MAX?UINT_MAX:(UINT)size;
    FRESULT result=f_write(&cookie->file,buffer,count,&written);
    if(!result)result=f_sync(&cookie->file); /* fflush commits FatFs metadata too. */
    if(result)return error(result);
    if(written<count)errno=ENOSPC;
    return written;
}
static int file_seek(void *opaque,off_t *position,int whence) {
    struct file_cookie *cookie=opaque;
    uint64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?f_tell(&cookie->file):f_size(&cookie->file);
    if(whence!=SEEK_SET&&whence!=SEEK_CUR&&whence!=SEEK_END) { errno=EINVAL;return -1; }
    if(base>INT64_MAX) { errno=EOVERFLOW;return -1; }
    uint64_t target;
    if(*position<0) {
        uint64_t delta=(uint64_t)(-(*position+1))+1;
        if(delta>base) { errno=EINVAL;return -1; }
        target=base-delta;
    } else {
        if((uint64_t)*position>INT64_MAX-base) { errno=EOVERFLOW;return -1; }
        target=base+(uint64_t)*position;
    }
    if(!cookie->writable&&target>f_size(&cookie->file)) { errno=EINVAL;return -1; }
    FRESULT result=f_lseek(&cookie->file,target);if(result)return error(result);
    if(f_tell(&cookie->file)!=target) { errno=ENOSPC;return -1; }
    *position=(off_t)target;return 0;
}
static int file_close(void *opaque) {
    struct file_cookie *cookie=opaque;
    FRESULT result=f_close(&cookie->file);free(cookie);
    return result==FR_OK?0:error(result);
}
FILE *apx_fopen(const char *path,const char *mode) {
    _Static_assert(sizeof(off_t)==8,"FatFs stdio needs 64-bit file offsets");
    if(!path||!mode) { errno=EINVAL;return NULL; }
    if(!*mode||!strchr("rwa",*mode)) { errno=EINVAL;return NULL; }
    unsigned binary=0,plus=0,exclusive=0;
    for(const char *p=mode+1;*p;++p) {
        if(*p=='b'&&!binary)binary=1;
        else if(*p=='+'&&!plus)plus=1;
        else if(*p=='x'&&!exclusive&&*mode=='w')exclusive=1;
        else { errno=EINVAL;return NULL; }
    }
    BYTE flags=*mode=='r'?FA_READ:FA_WRITE;
    if(plus)flags|=FA_READ|FA_WRITE;
    if(*mode=='w')flags|=exclusive?FA_CREATE_NEW:FA_CREATE_ALWAYS;
    if(*mode=='a')flags|=FA_OPEN_APPEND;
    struct file_cookie *cookie=malloc(sizeof(*cookie));if(!cookie) { errno=ENOMEM;return NULL; }
    cookie->append=*mode=='a';cookie->writable=(flags&FA_WRITE)!=0;
    FRESULT result=f_open(&cookie->file,path,flags);
    if(result) { free(cookie);error(result);return NULL; }
    /* Picolibc's cookie parser only needs r/w/a and optional +. */
    char stream_mode[3]={*mode,plus?'+':0,0};
    cookie_io_functions_t operations={.read=(flags&FA_READ)?file_read:NULL,
        .write=(flags&FA_WRITE)?file_write:NULL,.seek=file_seek,.close=file_close};
    FILE *stream=fopencookie(cookie,stream_mode,operations);
    if(!stream) { int saved=errno;f_close(&cookie->file);free(cookie);errno=saved; }
    return stream;
}
