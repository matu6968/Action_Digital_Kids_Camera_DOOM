#include <appotech/sd.h>
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <string.h>
#include "board.h"
#define CONTROL 0x9880u
#define EVENT 0x9884u
#define CLOCK 0x9888u
#define LONG_RESPONSE 0x988cu
#define DMA_ADDRESS 0x9890u
#define DMA_LENGTH 0x9894u
#define COMMAND 0x9898u
#define ARGUMENT 0x989cu
#define CMD_DONE (1u<<17)
#define DATA_DONE (1u<<15)
#define CARD_READY (1u<<20)
#define R1_ERRORS 0xfdffe008u
static struct apx_sd_info card;
static uint8_t bounce[512] __attribute__((aligned(32)));
/* Acknowledge only the requested W1C latch. A short RX can complete while
 * command completion is serviced; replaying DATA_DONE would lose that event. */
static void acknowledge(uint32_t mask) {
    apx_spr_write(EVENT,(apx_spr_read(EVENT)&~(CMD_DONE|DATA_DONE))|mask);
}
/* Board input sampling can continue during SD command/data deadlines. */
__attribute__((weak,noinline)) void apx_buttons_poll(void) {}
static void service(void) { apx_usb_poll();apx_buttons_poll();apx_maintenance(); }
static int wait_status(uint32_t mask,uint32_t ticks) {
    uint32_t deadline=apx_ticks()+ticks;
    do {
        card.last_status=apx_spr_read(CONTROL);
        if(card.last_status&mask)return 0;
        service();
    } while(!apx_deadline_expired(deadline));
    ++card.errors;return APX_SD_TIMEOUT;
}
static int command(unsigned cmd,uint32_t argument,uint32_t *response,uint8_t *long_response) {
    unsigned mode=cmd==0?2:(cmd==2||cmd==9)?3:(cmd==7||cmd==12)?5:1;
    card.last_command=cmd;
    apx_spr_write(COMMAND,cmd|0x40u);
    apx_spr_write(ARGUMENT,argument);
    acknowledge(CMD_DONE);
    apx_spr_write(LONG_RESPONSE,0x2000);
    apx_spr_write(EVENT,mode);
    int result=wait_status(CMD_DONE,3277); /* 100 ms */
    uint32_t reply=apx_spr_read(ARGUMENT);
    if(response)*response=reply;
    if(!result&&long_response) {
        /* The controller reverses bytes within each response word in SRAM. */
        for(unsigned i=0;i<16;++i)long_response[i]=apx_mmio_read8(0x2000+(i^3u));
    }
    acknowledge(CMD_DONE);
    if(result)return result;
    if(cmd!=0&&(card.last_status&(1u<<19))) { ++card.errors;return APX_SD_TIMEOUT; }
    /* R3 has no CRC; R2 is checked when parsing its structure. */
    if(cmd!=0&&cmd!=2&&cmd!=3&&cmd!=8&&cmd!=9&&cmd!=41&&(reply&R1_ERRORS)) {
        ++card.errors;
        if(reply&(1u<<26)) { card.write_protected=1;return APX_SD_WRITE_PROTECTED; }
        return APX_SD_CARD_ERROR;
    }
    return 0;
}
static uint32_t bits(const uint8_t *data,unsigned high,unsigned low) {
    uint32_t value=0;
    for(unsigned b=high+1;b-- >low;)value=(value<<1)|((data[(127-b)/8]>>(b%8))&1u);
    return value;
}
int apx_sd_init(void) {
    apx_sd_shutdown();memset(&card,0,sizeof(card));
    apx_spr_write(0x8840,apx_spr_read(0x8840)&~(1u<<22));
    apx_spr_write(0x9280,apx_spr_read(0x9280)|(1u<<12));
    /* SD0 pinmux (02050a7c -> 02023874 selector 7). */
    apx_spr_write(0x8344,(apx_spr_read(0x8344)&~(1u<<16))|(1u<<16));
    /* Board uses SD0 in one-bit mode: stock argument 0 (PA0..2).
     * The four-bit route uses extra pins and is not validated on this PCB. */
    apx_spr_write(0x8100,apx_spr_read(0x8100)|7);
    apx_spr_write(0x8080,apx_spr_read(0x8080)|7);
    apx_spr_write(0x80c0,apx_spr_read(0x80c0)&~7u);
    /* PA2 must remain an output driven low on this PCB. Making it an input
     * (generic vendor init) makes CMD8 time out; restoring low recovers it.
     * This appears to be a board card-enable line; electrical role unconfirmed. */
    apx_spr_write(0x8000,apx_spr_read(0x8000)&~4u);
    apx_spr_write(0x8040,apx_spr_read(0x8040)&~6u);
    apx_spr_write(CONTROL,0);apx_spr_write(CONTROL,0x42);
    apx_spr_write(CONTROL,apx_spr_read(CONTROL)&0x7fffcfffu);
    apx_spr_write(CLOCK,APX_SD_CLOCK_SOURCE_HZ/(2*200000u)-1);
    apx_spr_write(EVENT,0);apx_spr_write(DMA_LENGTH,0);
    apx_spr_write(CONTROL,apx_spr_read(CONTROL)|1);
    uint32_t deadline=apx_ticks()+328;
    while(!apx_deadline_expired(deadline))service();
    int result=command(0,0,0,0);if(result)goto fail;
    uint32_t response=0;
    result=command(8,0x1aa,&response,0);
    unsigned v2=!result&&response==0x1aa;
    /* Legacy SDSC may time out on CMD8. Restart its idle sequence. */
    if(!v2) { result=command(0,0,0,0);if(result)goto fail; }
    deadline=apx_ticks()+65536;
    do {
        result=command(55,0,0,0);if(result)goto fail;
        result=command(41,0x00ff8000u|(v2?0x40000000u:0),&card.ocr,0);if(result)goto fail;
        if(card.ocr&0x80000000u)break;
        service();
    } while(!apx_deadline_expired(deadline));
    if(!(card.ocr&0x80000000u)) { result=APX_SD_NOT_READY;goto fail; }
    card.high_capacity=!!(card.ocr&0x40000000u);
    result=command(2,0,0,card.cid);if(result)goto fail;
    result=command(3,0,&response,0);if(result)goto fail;
    card.rca=response&0xffff0000u;
    if(!card.rca||(response&0xe000u)) { result=APX_SD_CARD_ERROR;goto fail; }
    result=command(9,card.rca,0,card.csd);if(result)goto fail;
    card.write_protected=bits(card.csd,13,12)!=0;
    unsigned version=bits(card.csd,127,126);
    if(version==1&&card.high_capacity)card.sectors=((uint64_t)bits(card.csd,69,48)+1)*1024;
    else if(version==0&&!card.high_capacity) {
        unsigned shift=bits(card.csd,83,80)+bits(card.csd,49,47)+2;
        if(shift<9||shift>31) { result=APX_SD_CARD_ERROR;goto fail; }
        card.sectors=((uint64_t)bits(card.csd,73,62)+1)<<(shift-9);
    } else { result=APX_SD_UNSUPPORTED;goto fail; }
    if(!card.sectors||card.sectors>UINT64_C(0x100000000)||
       (!card.high_capacity&&card.sectors>UINT64_C(0x800000))) {
        /* SDSC command byte addresses must fit in a 32-bit argument. */
        result=APX_SD_UNSUPPORTED;goto fail;
    }
    result=command(7,card.rca,0,0);if(result)goto fail;
    if(!card.high_capacity) { result=command(16,512,0,0);if(result)goto fail; }
    result=command(13,card.rca,&response,0);if(result)goto fail;
    if((response&0x1f00u)!=0x900u) { result=APX_SD_CARD_ERROR;goto fail; }
    unsigned divider=APX_SD_CLOCK_SOURCE_HZ/(2*APX_SD_CLOCK_HZ);
    apx_spr_write(CLOCK,divider-1);
    card.clock_hz=APX_SD_CLOCK_SOURCE_HZ/(2*divider);
    card.initialized=1;return 0;
fail:
    apx_sd_shutdown();return result;
}
const struct apx_sd_info *apx_sd_get_info(void) { return &card; }
static int range(uint32_t sector,const void *buffer,unsigned count) {
    uintptr_t address=(uintptr_t)buffer;
    return count&&buffer&&(uint64_t)sector+count<=card.sectors&&
       count<=UINT32_MAX/512&&address>=APX_RAM_BASE&&address<APX_RAM_END&&
       count*512u<=APX_RAM_END-address;
}
static int retry_sector(void) {
    struct apx_sd_info previous=card;
    uint32_t failed_command=card.last_command,failed_status=card.last_status;
    int result=apx_sd_init();
    card.reads+=previous.reads;card.writes+=previous.writes;card.errors+=previous.errors;
    card.retries=previous.retries+1;card.last_failure_command=failed_command;card.last_failure_status=failed_status;
    if(result)return result;
    /* Do not replay a filesystem operation against a replaced card. */
    if(card.sectors!=previous.sectors||memcmp(card.cid,previous.cid,sizeof(card.cid))) {
        apx_sd_shutdown();return APX_SD_NOT_READY;
    }
    return apx_sd_set_clock(previous.clock_hz);
}
int apx_sd_read(uint32_t sector,void *buffer,unsigned count) {
    if(!card.initialized)return APX_SD_NOT_READY;
    if(!range(sector,buffer,count))return APX_SD_RANGE;
    uint8_t *out=buffer;
    for(unsigned i=0;i<count;++i) {
      for(unsigned attempt=0;;++attempt) {
        int result=apx_dma_invalidate(bounce,sizeof(bounce));if(result)return APX_SD_RANGE;
        apx_spr_write(DMA_ADDRESS,(uintptr_t)bounce);
        acknowledge(DATA_DONE);
        apx_spr_write(DMA_LENGTH,512);
        apx_spr_write(EVENT,apx_spr_read(EVENT)|0x10u);
        apx_spr_write(CONTROL,apx_spr_read(CONTROL)&~2u);
        uint32_t argument=card.high_capacity?sector+i:(sector+i)*512u;
        result=command(17,argument,0,0);
        if(!result)result=wait_status(DATA_DONE,16384);
        uint32_t status=apx_spr_read(CONTROL);
        acknowledge(DATA_DONE);
        apx_spr_write(DMA_LENGTH,0);
        /* Receive status. Bit 16 is the data CRC error flag: it is set when the data comes out as garbage (checked with a card
         * left in 4-bit mode). Bits 22:21 were required to read 10 on the SDSC card the SDK was first validated with, but they
         * read 11, 01 or 00 on an SDHC card from one run to the next while the data was intact (FAT boot sectors, 4096
         * sectors read twice with identical hashes), so they are not a validity flag and are ignored here. */
        if(!result&&(status&0x00010000u)) {
            result=APX_SD_CARD_ERROR;++card.errors;
        }
        if(result) {
            apx_sd_shutdown();
            if(attempt<2&&!retry_sector())continue;
            return result;
        }
        apx_dma_invalidate(bounce,sizeof(bounce));
        memcpy(out+i*512u,bounce,512);++card.reads;service();break;
      }
    }
    return 0;
}
int apx_sd_sync(void) {
    if(!card.initialized)return APX_SD_NOT_READY;
    /* Stock 02050e28 clocks DAT0 while waiting for SPR 9880 bit20, then
     * 020321bc polls CMD13 until the card returns to transfer state. */
    apx_spr_write(CONTROL,apx_spr_read(CONTROL)|2u);
    int result=wait_status(CARD_READY,65536); /* 2 s, including slow SDXC programming */
    apx_spr_write(CONTROL,apx_spr_read(CONTROL)&~2u);
    if(result)goto fail;
    uint32_t deadline=apx_ticks()+65536,response;
    do {
        result=command(13,card.rca,&response,0);if(result)goto fail;
        if((response&0x1f00u)==0x900u)return 0;
        service();
    } while(!apx_deadline_expired(deadline));
    ++card.errors;result=APX_SD_TIMEOUT;
fail:
    apx_sd_shutdown();return result;
}
int apx_sd_write(uint32_t sector,const void *buffer,unsigned count) {
    if(!card.initialized)return APX_SD_NOT_READY;
    if(!range(sector,buffer,count))return APX_SD_RANGE;
    if(card.write_protected)return APX_SD_WRITE_PROTECTED;
    const uint8_t *source=buffer;
    for(unsigned i=0;i<count;++i) {
      for(unsigned attempt=0;;++attempt) {
        memcpy(bounce,source+i*512u,512);
        if(apx_dma_clean(bounce,sizeof(bounce)))return APX_SD_RANGE;
        uint32_t argument=card.high_capacity?sector+i:(sector+i)*512u;
        /* Stock issues the write command before arming the transmit DMA.
         * TX 02050df0 uses event bits 4/5, RX uses bit4 alone. */
        int result=command(24,argument,0,0);if(result)goto fail;
        apx_spr_write(DMA_ADDRESS,(uintptr_t)bounce);
        acknowledge(DATA_DONE);
        apx_spr_write(DMA_LENGTH,512);
        apx_spr_write(EVENT,apx_spr_read(EVENT)|0x30u);
        result=wait_status(DATA_DONE,16384);
        uint32_t status=apx_spr_read(CONTROL);
        acknowledge(DATA_DONE);
        apx_spr_write(DMA_LENGTH,0);
        /* Stock 02050f48(write=1) rejects controller bit16. */
        if(!result&&(status&(1u<<16))) { result=APX_SD_CARD_ERROR;++card.errors; }
        if(!result)result=apx_sd_sync();
        if(result)goto fail;
        ++card.writes;service();break;
fail:
        apx_sd_shutdown();
        if(attempt<2&&result!=APX_SD_WRITE_PROTECTED&&!retry_sector())continue;
        return result;
      }
    }
    return 0;
}
int apx_sd_set_clock(unsigned maximum_hz) {
    if(!card.initialized||maximum_hz<200000||maximum_hz>APX_SD_CLOCK_HZ)return APX_SD_RANGE;
    unsigned divisor=(APX_SD_CLOCK_SOURCE_HZ+2*maximum_hz-1)/(2*maximum_hz);
    apx_spr_write(CLOCK,divisor-1);card.clock_hz=APX_SD_CLOCK_SOURCE_HZ/(2*divisor);return 0;
}
void apx_sd_shutdown(void) {
    /* EVENT writes omit the stock interrupt-enable bit6. CPU interrupt-source
     * assignments for SD are unverified; preserve unrelated CPU masks. */
    apx_spr_write(CONTROL,apx_spr_read(CONTROL)&~3u);
    apx_spr_write(EVENT,0x28000);apx_spr_write(DMA_LENGTH,0);
    card.initialized=0;
}
