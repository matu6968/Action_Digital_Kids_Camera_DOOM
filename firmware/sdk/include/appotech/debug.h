#ifndef APPOTECH_DEBUG_H
#define APPOTECH_DEBUG_H
#include <stddef.h>
#include <stdint.h>
#define APXD_VERSION 1
#define APXD_HEADER_SIZE 24
#define APXD_MAX_PAYLOAD 4096
#define APXD_RAW 0x80
enum apxd_operation { APXD_INFO=0, APXD_READ=1, APXD_WRITE=2, APXD_BOOTROM=3, APXD_LOG=4, APXD_STORAGE=5, APXD_GPIO=6, APXD_SYSTEM=7, APXD_JPEG=8, APXD_AUDIO=9, APXD_MICROPHONE=10, APXD_CAMERA=11, APXD_SPI=12, APXD_BATTERY=13, APXD_UART=14 };
enum apxd_space { APXD_MEMORY=0, APXD_MMIO=1, APXD_SPR=2 };
enum apxd_status { APXD_OK=0, APXD_INVALID=1, APXD_RANGE=2, APXD_UNSUPPORTED=3, APXD_PROTECTED=4, APXD_IO=5 };
void apx_debug_poll(void);
size_t apx_log_read(void *buffer,size_t capacity);
uint32_t apx_log_dropped(void);
#endif
