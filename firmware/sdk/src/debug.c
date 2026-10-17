#include <appotech/debug.h>
#include <appotech/exception.h>
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/power.h>
#include <appotech/clock.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#ifndef APX_BUILD_ID
#define APX_BUILD_ID "development"
#endif
static uint8_t request[APXD_HEADER_SIZE+APXD_MAX_PAYLOAD],response[APXD_HEADER_SIZE+APXD_MAX_PAYLOAD];
static uint8_t packet[512];
static size_t received,expected;
static uint32_t request_start;
static unsigned pending_action;
__attribute__((weak,noinline)) unsigned apx_boot_from_spi(void){return 0;}
__attribute__((weak,noinline)) uint32_t apx_storage_prepare_bootrom(void) { return APXD_OK; }
__attribute__((weak,noinline)) uint32_t apx_gpio_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *output_length) {
    (void)command;(void)input;(void)length;(void)output;*output_length=0;return APXD_UNSUPPORTED;
}
extern unsigned char __display_dma_start[],__display_dma_end[];
extern unsigned char __jpeg_buffer_start[],__jpeg_buffer_end[];
extern unsigned char __extra_ram_start[],__extra_ram_end[];
__attribute__((weak,noinline)) uint32_t apx_storage_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *output_length) {
    (void)command;(void)input;(void)length;(void)output;*output_length=0;return APXD_UNSUPPORTED;
}
__attribute__((weak,noinline)) uint32_t apx_jpeg_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *output_length) {
    (void)command;(void)input;(void)length;(void)output;*output_length=0;return APXD_UNSUPPORTED;
}
__attribute__((weak,noinline)) uint32_t apx_audio_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *output_length) {
    (void)command;(void)input;(void)length;(void)output;*output_length=0;return APXD_UNSUPPORTED;
}
__attribute__((weak,noinline)) uint32_t apx_microphone_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *output_length) {
    (void)command;(void)input;(void)length;(void)output;*output_length=0;return APXD_UNSUPPORTED;
}
__attribute__((weak,noinline)) uint32_t apx_camera_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *output_length) {
    (void)command;(void)input;(void)length;(void)output;*output_length=0;return APXD_UNSUPPORTED;
}
__attribute__((weak,noinline)) uint32_t apx_spi_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *output_length) {
    (void)command;(void)input;(void)length;(void)output;*output_length=0;return APXD_UNSUPPORTED;
}
__attribute__((weak,noinline)) uint32_t apx_battery_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *output_length) {
    (void)command;(void)input;(void)length;(void)output;*output_length=0;return APXD_UNSUPPORTED;
}
__attribute__((weak,noinline)) uint32_t apx_uart_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *output_length) {
    (void)command;(void)input;(void)length;(void)output;*output_length=0;return APXD_UNSUPPORTED;
}
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put32(uint8_t *p,uint32_t v) { p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24; }
void apx_debug_reset(void) { received=expected=pending_action=0; }
static int protected_range(uint32_t address,uint32_t length) {
    return (address<APX_RUNTIME_END && (address>=APX_RUNTIME_BASE || length>APX_RUNTIME_BASE-address)) ||
           (address<APX_STACK_END && (address>=APX_STACK_BASE || length>APX_STACK_BASE-address)) ||
           ((uintptr_t)__display_dma_end>(uintptr_t)__display_dma_start && address<0x02220000u &&
            (address>=0x02210000u || length>0x02210000u-address)) ||
           ((uintptr_t)__extra_ram_end>(uintptr_t)__extra_ram_start && address<0x022e0000u &&
            (address>=0x02220000u || length>0x02220000u-address)) ||
           ((uintptr_t)__jpeg_buffer_end>(uintptr_t)__jpeg_buffer_start && address<0x02400000u &&
            (address>=0x022e0000u || length>0x022e0000u-address));
}
static void dispatch(void) {
    uint32_t address=get32(request+12),length=get32(request+16),status=APXD_OK,output=0;
    unsigned operation=request[5],space=request[6],width=request[7]&0x7f,raw=request[7]&APXD_RAW;
    memcpy(response,request,APXD_HEADER_SIZE);
    if(memcmp(request,"APXD",4) || request[4]!=APXD_VERSION || get32(request+20)!=0 || length>APXD_MAX_PAYLOAD)
        status=APXD_INVALID;
    else if((raw && operation!=APXD_WRITE) ||
            ((operation==APXD_INFO || operation==APXD_LOG || operation==APXD_BOOTROM) &&
             (space!=APXD_MEMORY || width!=1 || address!=0)))status=APXD_INVALID;
    else if(operation==APXD_INFO && length==0) {
        struct apx_clock_info clock;apx_clock_get_info(&clock);
        char exception_json[300]="";
        struct apx_exception crash;
        if(apx_exception_previous(&crash))
            snprintf(exception_json,sizeof(exception_json),
                ",\"previous_exception\":{\"vector\":%lu,\"name\":\"%s\",\"spr20\":%lu,\"spr30\":%lu,\"spr40\":%lu,"
                "\"sp\":%lu,\"lr\":%lu,\"fp\":%lu,\"stack\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu]}",
                (unsigned long)crash.vector,apx_exception_name(crash.vector),(unsigned long)crash.spr20,(unsigned long)crash.spr30,
                (unsigned long)crash.spr40,(unsigned long)crash.sp,(unsigned long)crash.lr,(unsigned long)crash.fp,
                (unsigned long)crash.stack[0],(unsigned long)crash.stack[1],(unsigned long)crash.stack[2],(unsigned long)crash.stack[3],
                (unsigned long)crash.stack[4],(unsigned long)crash.stack[5],(unsigned long)crash.stack[6],(unsigned long)crash.stack[7]);
        int count=snprintf((char *)response+24,APXD_MAX_PAYLOAD,
            "{\"protocol\":1,\"board\":\"%s\",\"build_id\":\"%s\",\"max_payload\":4096,"
            "\"ram\":[33554432,37748736],\"owned_ram\":[[35127296,35258368],[35651584,35667968]%s%s%s],"
            "\"boot_source\":\"%s\",\"cpu_hz_estimate\":%lu,\"usb_speed\":\"%s\",\"log_dropped\":%lu,"
            "\"suspend_count\":%u,\"resume_count\":%u,"
            "\"clock\":{\"requested_mhz\":%lu,\"pll1_control\":%lu,\"pll1_coefficient\":%lu,"
            "\"baseline_loop_ticks\":%lu,\"active_loop_ticks\":%lu,\"error\":%lu}%s}",
            APX_BOARD_NAME,APX_BUILD_ID,
            (uintptr_t)__display_dma_end>(uintptr_t)__display_dma_start?",[35717120,35782656]":"",
            (uintptr_t)__extra_ram_end>(uintptr_t)__extra_ram_start?",[35782656,36569088]":"",
            (uintptr_t)__jpeg_buffer_end>(uintptr_t)__jpeg_buffer_start?",[36569088,37748736]":"",
            apx_boot_from_spi()?"spi":"usb",(unsigned long)clock.cpu_hz_estimate,apx_usb_high_speed()?"high":"full",(unsigned long)apx_log_dropped(),
            apx_usb_suspend_count(),apx_usb_resume_count(),(unsigned long)clock.requested_mhz,
            (unsigned long)clock.pll1_control,(unsigned long)clock.pll1_coefficient,
            (unsigned long)clock.baseline_loop_ticks,(unsigned long)clock.active_loop_ticks,(unsigned long)clock.error,exception_json);
        if(count<0 || count>=APXD_MAX_PAYLOAD)status=APXD_INVALID;else output=count;
    } else if(operation==APXD_LOG) output=apx_log_read(response+24,length);
    else if(operation==APXD_BOOTROM && length==0)pending_action=(status=apx_storage_prepare_bootrom())==APXD_OK?3:0;
    else if(operation==APXD_STORAGE||operation==APXD_GPIO||operation==APXD_SYSTEM||operation==APXD_JPEG||operation==APXD_AUDIO||operation==APXD_MICROPHONE||operation==APXD_CAMERA||operation==APXD_SPI||operation==APXD_BATTERY||operation==APXD_UART) {
        if(space!=APXD_MEMORY||width!=1)status=APXD_INVALID;
        else {
            size_t size=0;
            if(operation==APXD_SYSTEM&&(address==4||address==5)) {
                if(length)status=APXD_INVALID;
                else {
                    enum apx_system_action action=address==4?APX_SYSTEM_REBOOT:APX_SYSTEM_POWER_OFF;
                    status=apx_system_prepare(action);pending_action=status==APXD_OK?action:0;
                }
            } else status=operation==APXD_STORAGE?apx_storage_command(address,request+24,length,response+24,&size):
                operation==APXD_GPIO?apx_gpio_command(address,request+24,length,response+24,&size):
                operation==APXD_SYSTEM?apx_system_command(address,request+24,length,response+24,&size):
                operation==APXD_JPEG?apx_jpeg_command(address,request+24,length,response+24,&size):
                operation==APXD_AUDIO?apx_audio_command(address,request+24,length,response+24,&size):
                operation==APXD_MICROPHONE?apx_microphone_command(address,request+24,length,response+24,&size):
                operation==APXD_CAMERA?apx_camera_command(address,request+24,length,response+24,&size):
                operation==APXD_SPI?apx_spi_command(address,request+24,length,response+24,&size):
                operation==APXD_BATTERY?apx_battery_command(address,request+24,length,response+24,&size):
                apx_uart_command(address,request+24,length,response+24,&size);
            if(size>APXD_MAX_PAYLOAD)status=APXD_INVALID;
            else if(status==APXD_OK)output=size;
        }
    }
    else if(operation==APXD_READ || operation==APXD_WRITE) {
        if((width!=1 && width!=2 && width!=4) || (space!=APXD_SPR && address%width) || length%width ||
           (length && address>UINT32_MAX-(length-1)))status=APXD_RANGE;
        else if(space==APXD_SPR) {
            if(width!=4 || length!=4 || address>0xffff)status=APXD_RANGE;
            else if(operation==APXD_READ) { put32(response+24,apx_spr_read(address));output=4; }
            else apx_spr_write(address,get32(request+24));
        } else if(space==APXD_MEMORY || space==APXD_MMIO) {
            if(operation==APXD_WRITE && !raw && length && protected_range(address,length))status=APXD_PROTECTED;
            else for(uint32_t offset=0;offset<length;offset+=width) {
                uintptr_t current=address+offset;
                if(operation==APXD_READ) {
                    uint32_t value=width==4?*(volatile uint32_t *)current:
                                   width==2?*(volatile uint16_t *)current:*(volatile uint8_t *)current;
                    for(unsigned i=0;i<width;++i)response[24+offset+i]=value>>(8*i);
                } else {
                    uint32_t value=0;for(unsigned i=0;i<width;++i)value|=(uint32_t)request[24+offset+i]<<(8*i);
                    if(width==4)*(volatile uint32_t *)current=value;
                    else if(width==2)*(volatile uint16_t *)current=value;
                    else *(volatile uint8_t *)current=value;
                }
            }
            if(status==APXD_OK && operation==APXD_READ)output=length;
        } else status=APXD_UNSUPPORTED;
    } else status=APXD_UNSUPPORTED;
    put32(response+16,output);put32(response+20,status);
    if(apx_usb_send(response,24+output)!=0)pending_action=0;
}
void apx_debug_poll(void) {
    apx_usb_poll();
    if(pending_action && apx_usb_tx_idle()) {
        if(pending_action==3)apx_bootrom();
        apx_system_commit((enum apx_system_action)pending_action);
    }
    if(!apx_usb_tx_idle())return;
    if(received && apx_ticks()-request_start>16384)received=expected=0;
    int count=apx_usb_receive(packet,sizeof(packet));
    if(count<=0) { if(count<0)received=expected=0;return; }
    if(!received)request_start=apx_ticks();
    if(received+(unsigned)count>sizeof(request)) { received=expected=0;return; }
    memcpy(request+received,packet,count);received+=count;
    if(received>=24 && !expected) {
        uint32_t length=get32(request+16);
        if(length>APXD_MAX_PAYLOAD || memcmp(request,"APXD",4)) { received=expected=0;return; }
        expected=24+((request[5]==APXD_WRITE||request[5]==APXD_STORAGE||request[5]==APXD_GPIO||request[5]==APXD_SYSTEM||request[5]==APXD_JPEG||request[5]==APXD_AUDIO||request[5]==APXD_MICROPHONE||request[5]==APXD_CAMERA||request[5]==APXD_SPI||request[5]==APXD_BATTERY||request[5]==APXD_UART)?length:0);
    }
    if(expected && received>=expected) {
        if(received==expected)dispatch();
        received=expected=0;
    }
}
