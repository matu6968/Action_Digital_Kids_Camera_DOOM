/* JT5295 byte-wide indexed USB registers, addressed as SPR 0xa000+4*offset.
 * Register/FIFO accesses reproduce the direct BootROM path; no ROM calls.
 */
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/trace.h>
#include <string.h>
#define FADDR 0x00
#define POWER 0x01
#define INTRTX 0x02
#define INTRRX 0x04
#define INTRUSB 0x0a
#define INDEX 0x0e
#define TXMAXP 0x10
#define TXCSR 0x12
#define RXMAXP 0x14
#define RXCSR 0x16
#define RXCOUNT 0x18
#ifndef APX_USB_SETUP_FIFO /* the host model test replaces the memory-mapped FIFO */
#define APX_USB_SETUP_FIFO(i) (*(volatile uint8_t *)(uintptr_t)(0x8000+(i)))
#endif
static uint8_t reg_read(unsigned offset) { return apx_spr_read(0xa000+4*offset); }
static void reg_write(unsigned offset,unsigned value) { apx_spr_write(0xa000+4*offset,value&255); }
static unsigned read16(unsigned offset) { return reg_read(offset)|(unsigned)reg_read(offset+1)<<8; }
static void write16(unsigned offset,unsigned value) { reg_write(offset,value);reg_write(offset+1,value>>8); }
static unsigned configured,high_speed,suspended;
#ifndef APX_USB_ENUM_TIMEOUT_TICKS
#define APX_USB_ENUM_TIMEOUT_TICKS 0 /* diagnostic builds: reset if the host has not configured the device by then */
#endif
extern _Noreturn void apx_watchdog_reset(void);
static unsigned suspend_count,resume_count;
static unsigned tx_halt,rx_halt;
static const uint8_t *tx_data;
static size_t tx_length,tx_offset;
static unsigned tx_active,tx_zlp;
static uint8_t control[256];
static unsigned control_length,control_offset,control_zlp,control_active;
static int pending_address=-1;
static unsigned address_wait;
extern void apx_debug_reset(void);
static uint16_t le16(const uint8_t *p) { return p[0]|(uint16_t)p[1]<<8; }
static void set_le16(uint8_t *p,unsigned value) { p[0]=value;p[1]=value>>8; }
static const uint8_t device_descriptor[]={18,1,0,2,0,0,0,64,
    APX_USB_VID&255,APX_USB_VID>>8,APX_USB_PID&255,APX_USB_PID>>8,0,1,1,2,0,1};
static const uint8_t qualifier[]={10,6,0,2,0,0,0,64,1,0};
static const uint8_t config_template[]={9,2,32,0,1,1,0,0x80,50,
    9,4,0,0,2,0xff,0,1,0,7,5,0x01,2,0,2,0,7,5,0x81,2,0,2,0};
static void configure_endpoints(void) {
    reg_write(INDEX,1);
    write16(TXMAXP,high_speed?512:64);write16(RXMAXP,high_speed?512:64);
    reg_write(TXCSR+1,0);reg_write(RXCSR+1,0);
    reg_write(TXCSR,0x48);reg_write(TXCSR,0x48);
    reg_write(RXCSR,0x90);reg_write(RXCSR,0x90);
    tx_active=tx_halt=rx_halt=0;
    reg_write(INDEX,0);
}
static void trace_phy(void) { /* ring builds: the registers the PHY and USB clocks depend on */
    static const uint16_t list[]={0xd800,0xd804,0xd808,0xd80c,0xd840,0x9840,0x9870,0x984c,0x9850,0x9854,0x884c,0x8850,0x8844,0x8848,0x9800,0x0018,0x4800};
    for(unsigned i=0;i<sizeof(list)/sizeof(list[0]);++i)apx_trace_spr(list[i]);
}
static void reset_state(void) {
    configured=0;suspended=0;high_speed=!!(reg_read(POWER)&0x10);
    control_active=control_offset=control_length=control_zlp=0;
    pending_address=-1;address_wait=0;reg_write(FADDR,0);
    configure_endpoints();
    apx_debug_reset();
}
static void control_stall(void) { control_active=0;reg_write(TXCSR,0x60); }
static void control_status(void) { control_active=0;reg_write(TXCSR,0x48); }
static void control_reply(unsigned count,unsigned requested) {
    if(count>requested)count=requested;
    control_length=count;control_offset=0;
    control_zlp=(count<requested && count%64==0);
    control_active=1;reg_write(TXCSR,0x40);
}
static void setup_request(const uint8_t *setup) {
    unsigned type=setup[0],request=setup[1],value=le16(setup+2),index=le16(setup+4),length=le16(setup+6);
    {static unsigned seen;if(!seen){seen=1;apx_trace(APX_TRACE_USB_FIRST_SETUP,apx_ticks()&0xffffff);}} /* enumeration timeline, trace builds only */
    apx_trace_usb(2,(unsigned)type<<24|request<<16|(value&0xffff),(unsigned)index<<16|(length&0xffff)); /* U2: bmRequestType bRequest wValue, wIndex wLength */
    control_active=0;pending_address=-1;address_wait=0;
    if(type==0x80 && request==6) {
        unsigned descriptor=value>>8,number=value&255,count=0;
        if(descriptor==1 && number==0) { count=sizeof(device_descriptor);memcpy(control,device_descriptor,count); }
        else if((descriptor==2 || descriptor==7) && number==0) {
            count=sizeof(config_template);memcpy(control,config_template,count);control[1]=descriptor;
            unsigned packet=(descriptor==7 ? !high_speed : high_speed)?512:64;
            set_le16(control+22,packet);set_le16(control+29,packet);
        } else if(descriptor==6 && number==0) { count=sizeof(qualifier);memcpy(control,qualifier,count); }
        else if(descriptor==3) {
            if(number==0) { control[0]=4;control[1]=3;control[2]=9;control[3]=4;count=4; }
            else if((number==1 || number==2) && index==0x409) {
                const char *text=number==1?"AppoTech SDK":"JT5295 SDK Debug";
                unsigned n=strlen(text);count=2+2*n;control[0]=count;control[1]=3;
                for(unsigned i=0;i<n;++i) { control[2+2*i]=text[i];control[3+2*i]=0; }
            }
        }
        if(count)control_reply(count,length);else control_stall();
    } else if(type==0 && request==5 && value<128 && index==0 && length==0) {
        pending_address=value;address_wait=1;control_status();
        apx_trace_usb(3,value,0); /* U3: SET_ADDRESS accepted, waiting for the status stage */
    } else if(type==0 && request==9 && value<=1 && index==0 && length==0) {
        configured=value;configure_endpoints();apx_debug_reset();control_status();
        apx_trace(APX_TRACE_USB_CONFIGURED,apx_ticks()&0xffffff);
        apx_trace_usb(5,value,0); /* U5: configured */
    } else if(type==0x80 && request==8 && value==0 && index==0 && length==1) {
        control[0]=configured;control_reply(1,length);
    } else if((type==0x80 || type==0x81 || type==0x82) && request==0 && value==0 && length==2 &&
              ((type==0x80 && index==0) || (type==0x81 && index==0) ||
               (type==0x82 && (index==0 || index==0x80 || index==1 || index==0x81)))) {
        control[0]=type==0x82?(index==1?rx_halt:index==0x81?tx_halt:0):0;
        control[1]=0;control_reply(2,length);
    } else if(type==0x02 && (request==1 || request==3) && value==0 && length==0 && (index==1 || index==0x81)) {
        reg_write(INDEX,1);
        if(index==1) { rx_halt=(request==3);reg_write(RXCSR,rx_halt?0x20:0x90); }
        else { tx_halt=(request==3);reg_write(TXCSR,tx_halt?0x10:0x48); }
        reg_write(INDEX,0);control_status();
    } else if(type==0x81 && request==10 && index==0 && value==0 && length==1 && configured) {
        control[0]=0;control_reply(1,1);
    } else if(type==0x01 && request==11 && index==0 && value==0 && length==0 && configured) {
        configure_endpoints();apx_debug_reset();control_status();
    } else control_stall();
}
void apx_usb_init(void) {
    apx_spr_write(0x18,apx_spr_read(0x18)&~2u);
    apx_spr_write(0x4800,apx_spr_read(0x4800)&~0x8000u);
    /* BootROM 105d20..105f28: cold boot has not enabled the PHY/core clocks.
     * All accesses are independent SPR writes; PLL2 is inherited from the
     * preserved RAM-calibration bootstrap. Keep the pull-up off throughout. */
    apx_spr_write(0x884c,apx_spr_read(0x884c)|0x80000000u);
    apx_spr_write(0x9840,apx_spr_read(0x9840)|0x10000u);apx_delay_ms(1);
    apx_spr_write(0x9840,(apx_spr_read(0x9840)&0xfc0df3ffu)|0x1080u);apx_delay_ms(1);
    apx_spr_write(0x9840,apx_spr_read(0x9840)|0x40u);apx_delay_ms(10);
    /* BootROM 105e40..105e88 then sets bit 0 and bit 2, each after a settling
     * delay. A RAM launch inherits them from the ROM loader; a cold SPI boot has
     * nobody to set them, and without them the device never enumerates. */
    apx_spr_write(0x9840,apx_spr_read(0x9840)|0x1u);apx_delay_ms(1);
    apx_spr_write(0x9840,apx_spr_read(0x9840)|0x4u);apx_delay_ms(1);
    apx_spr_write(0x9870,(apx_spr_read(0x9870)&0x3fffffffu)|0x80000000u);
    apx_spr_write(0x8850,(apx_spr_read(0x8850)&~0x08000000u)|0x03000000u);
    apx_spr_write(0x8844,apx_spr_read(0x8844)&~1u);
    reg_write(POWER,reg_read(POWER)&~0x40u);apx_delay_ms(100);
    apx_spr_write(0xd840,apx_spr_read(0xd840)&~1u);
    apx_spr_write(0xd804,31);
    uint32_t start=apx_ticks();
    while(!(apx_spr_read(0xd804)&8)) { apx_maintenance();if(apx_ticks()-start>32768)break; }
    apx_spr_write(0xd800,0xf0);apx_spr_write(0xd808,0);
    apx_spr_write(0xd80c,apx_spr_read(0xd80c)|0x1800);
    apx_spr_write(0x984c,0x8000);apx_spr_write(0x9850,0x8040);apx_spr_write(0x9854,0x8240);
    reg_write(0x06,3);reg_write(0x07,0);reg_write(0x08,2);reg_write(0x09,0);reg_write(0x0b,7);
    reset_state();reg_write(POWER,APX_USB_FORCE_FULL_SPEED?0x40:0x60);
    apx_trace_usb(0xb,apx_ticks()-start,apx_spr_read(0xd804)); /* phy wait: ticks, 0xd804 */
    trace_phy();
}
void apx_usb_poll(void) {
    apx_maintenance();(void)apx_ticks();
    {static unsigned polled;if(!polled){polled=1;apx_trace(APX_TRACE_USB_FIRST_POLL,apx_ticks()&0xffffff);}}
#if APX_USB_ENUM_TIMEOUT_TICKS
    {static uint32_t first;static unsigned started;if(!started){started=1;first=apx_ticks();}
     if(!configured&&apx_ticks()-first>APX_USB_ENUM_TIMEOUT_TICKS){apx_trace_usb(9,0xdead,0);apx_watchdog_reset();}}
#endif
    unsigned interrupt=reg_read(INTRUSB),tx_interrupt=read16(INTRTX);(void)read16(INTRRX);
    if(interrupt)apx_trace_usb(1,interrupt<<8|reg_read(POWER),tx_interrupt); /* U1: INTRUSB (4 reset, 1 suspend, 2 resume) | POWER, INTRTX */
    if(interrupt&4) {
        static unsigned first_reset;
        reset_state();
        if(!first_reset){first_reset=1;trace_phy();}
    }
    if(interrupt&1) { suspended=1;++suspend_count; }
    if(interrupt&2) { suspended=0;++resume_count; }
    reg_write(INDEX,0);unsigned csr=reg_read(TXCSR);
    if(csr&0x14)apx_trace_usb(8,csr,control_active); /* U8: SENTSTALL (4) / SETUPEND (0x10) */
    if(csr&4) { reg_write(TXCSR,0);control_active=0; }
    /* SETUPEND: the host abandoned the control transfer (it resets and retries quickly while it
     * enumerates). That cancels a SET_ADDRESS waiting for its status stage; leaving address_wait set
     * with pending_address cleared made the next status-stage interrupt write 0xff into FADDR, so the
     * device vanished from the address the host had just given it ("device not accepting address"). */
    if(csr&0x10) { reg_write(TXCSR,0x80);control_active=0;pending_address=-1;address_wait=0; }
    if(address_wait && (tx_interrupt&1) && !(csr&2)) {
        if(pending_address>=0) {
            apx_trace_usb(4,pending_address,0); /* U4: address applied */
            reg_write(FADDR,pending_address);
        }
        pending_address=-1;address_wait=0;
    }
    if(csr&1) {
        unsigned count=read16(RXCOUNT);
        apx_trace_usb(6,csr<<8|(count&255),control_active); /* U6: RxPktRdy on EP0 */
        if(count==8) {
            uint8_t setup[8];
            for(unsigned i=0;i<8;++i)setup[i]=APX_USB_SETUP_FIFO(i);
            setup_request(setup);
        } else { reg_write(TXCSR,0x40);control_active=0; }
    }
    if(control_active && !(reg_read(TXCSR)&2)) {
        unsigned remaining=control_length-control_offset,count=remaining>64?64:remaining;
        for(unsigned i=0;i<count;++i)apx_spr_write(0xa080,control[control_offset++]);
        unsigned last=(control_offset==control_length && !control_zlp);
        if(!count) { control_zlp=0;last=1; }
        reg_write(TXCSR,2|(last?8:0));if(last)control_active=0;
        apx_trace_usb(7,count<<8|last,control_offset); /* U7: data-stage packet loaded */
    }
    if(configured && !suspended && tx_active && !tx_halt) {
        reg_write(INDEX,1);
        if(!(reg_read(TXCSR)&1)) {
            unsigned packet=high_speed?512:64;
            if(tx_offset<tx_length || tx_zlp) {
                size_t count=tx_length-tx_offset;if(count>packet)count=packet;
                reg_write(TXCSR+1,reg_read(TXCSR+1)|0x20);
                for(size_t i=0;i<count;++i)apx_spr_write(0xa090,tx_data[tx_offset++]);
                if(!count)tx_zlp=0;
                reg_write(TXCSR,1);
            } else { tx_active=0;reg_write(TXCSR+1,reg_read(TXCSR+1)&~0x20u); }
        }
    }
}
int apx_usb_receive(void *buffer,size_t capacity) {
    if(!configured || suspended || tx_active || rx_halt)return 0;
    reg_write(INDEX,1);if(!(reg_read(RXCSR)&1))return 0;
    unsigned count=read16(RXCOUNT);uint8_t *out=buffer;
    for(unsigned i=0;i<count;++i) { unsigned value=apx_spr_read(0xa090);if(i<capacity)out[i]=value; }
    reg_write(RXCSR,0);
    return count>capacity?-1:(int)count;
}
int apx_usb_send(const void *buffer,size_t length) {
    if(!configured || tx_active || tx_halt)return -1;
    tx_data=buffer;tx_length=length;tx_offset=0;tx_zlp=(length%(high_speed?512:64)==0);tx_active=1;return 0;
}
int apx_usb_tx_idle(void) { return !tx_active; }
int apx_usb_configured(void) { return configured; }
int apx_usb_high_speed(void) { return high_speed; }
int apx_usb_suspended(void) { return suspended; }
unsigned apx_usb_suspend_count(void) { return suspend_count; }
unsigned apx_usb_resume_count(void) { return resume_count; }
