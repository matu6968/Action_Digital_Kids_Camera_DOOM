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
/* The divider register (SPR 0x9888) is 8 bits wide and the SD clock is SOURCE/(divider+1): both measured on the Silvergear
 * (research/sd.md), the stock firmware's arithmetic. A written value above 255 wraps (359 behaves as 103). */
#define MAX_DIVIDER 255u
#ifndef APX_SD_HIGH_SPEED_CLOCK_HZ
#define APX_SD_HIGH_SPEED_CLOCK_HZ 48000000u
#endif
#ifndef APX_SD_HIGH_SPEED /* 1 = switch cards that support it to high speed (CMD6) and clock them at 48 MHz */
#define APX_SD_HIGH_SPEED 1
#endif
#define HIGH_SPEED_CONTROL_BITS (0x80000000u|8u) /* stock sets these two, in this order, with the CMD6 switch */
/* Widest data bus this board may try (1 or 4); boards/<board>/board.h sets it. The stock firmware has an unreachable four-bit branch (CONTROL bit 2, ACMD6
 * argument 2, PA3 + PE10/PE11 routed to the controller) but never runs it, so a board's wiring of DAT1..DAT3 is only known once the probe below has passed. */
#ifndef APX_SD_BUS_WIDTH_MAX
#define APX_SD_BUS_WIDTH_MAX 1
#endif
#define WIDE_RESTART (-100) /* private: the four-bit attempt failed and the card must be initialised again, one data line only */
#define CONTROL_FOUR_BIT 4u
static struct apx_sd_info card;
static uint8_t bounce[512] __attribute__((aligned(32)));
/* An open-ended read (CMD18) left running between calls, as the stock firmware does: a caller reading sector after sector
 * pays one command, then only a DMA re-arm per sector. Everything else that talks to the card stops it first. */
static struct { unsigned active;uint32_t next; } stream;
/* Read-ahead: while a stream is open the receive DMA is armed for the NEXT sector, into one of these two blocks, as soon as the current one is in. The card
 * sends it while the caller does something else (USB, a filesystem), and the next call only copies it out (about 5 us) instead of waiting for the card
 * (about 100 us). Two blocks, because the card starts the following block within about a microsecond of the end of the last one: the next receive must be
 * armed before the finished block is copied out, or the controller misses the start and a sector then costs two block times (measured: 5.5 -> 3 MB/s). */
static uint8_t prefetch_buf[2][512] __attribute__((aligned(32)));
static unsigned prefetch_idx;     /* the driver block the next read-ahead into prefetch_buf fills */
static unsigned prefetch_pending; /* the DMA is armed for sector stream.next */
/* Where the armed DMA lands: a driver block above, or, inside a multi-sector call, straight into the caller's buffer for the next sector. The latter needs no copy
 * at all; a copy out of freshly DMA'd (uncached) DDR costs about 70 us per sector, which capped every read at about 6 MB/s however fast the bus was (measured: the
 * same stream with no copy reaches 21 MB/s on a four-bit bus). */
static uint8_t *prefetch_target;
static int64_t last_read_end=-1; /* the sector after the last block read, -1 = none yet */
/* An open-ended write (CMD25), the mirror of the read stream: while it is open each further sector costs a DMA arm and the card's busy wait, no command. It is
 * closed (CMD12, then wait for the card to finish programming) by anything else that talks to the card, by apx_sd_sync(), by the first sector that does not
 * continue it, and by apx_sd_poll() once nothing has been written for APX_SD_WRITE_IDLE_TICKS (the data of a write that is still open is not safe against power loss). */
#ifndef APX_SD_WRITE_IDLE_TICKS
#define APX_SD_WRITE_IDLE_TICKS 1638u /* 50 ms */
#endif
static struct { unsigned active;uint32_t next,last_tick; } wstream;
static int64_t last_write_end=-1; /* the sector after the last block written, -1 = none yet */
static unsigned in_operation;     /* a public call is running: apx_sd_poll() must not touch the card */
static unsigned divider_for(unsigned hz) {
    unsigned steps=(APX_SD_CLOCK_SOURCE_HZ+hz-1)/hz; /* rounded up: the clock never exceeds hz */
    unsigned divider=steps?steps-1:0;
    return divider>MAX_DIVIDER?MAX_DIVIDER:divider;
}
static void set_divider(unsigned divider) {
    apx_spr_write(CLOCK,divider);card.clock_hz=APX_SD_CLOCK_SOURCE_HZ/(divider+1);
}
/* Acknowledge only the requested W1C latch. A short RX can complete while
 * command completion is serviced; replaying DATA_DONE would lose that event. */
static void acknowledge(uint32_t mask) {
    apx_spr_write(EVENT,(apx_spr_read(EVENT)&~(CMD_DONE|DATA_DONE))|mask);
}
/* Board input sampling can continue during SD command/data deadlines. */
__attribute__((weak,noinline)) void apx_buttons_poll(void) {}
static void service(void) { apx_usb_poll();apx_buttons_poll();apx_maintenance(); }
/* One service() takes about 12 us with TinyUSB (a USB poll, the buttons, the watchdog). If it ran on every spin of a wait, the data arriving mid-call would
 * only be noticed when the call returns, about 6 us late on average per sector, and the per-sector call after the wait adds a full 12. So the wait spins on the
 * status register (about 0.1 us a read) and services only every 256th iteration (about 40 us), and the per-sector service runs every 8th sector (about 0.8 ms):
 * USB, the buttons and the watchdog are still served well inside a millisecond. The deadline is checked at the same points; every wait here is milliseconds long. */
#define SPINS_PER_SERVICE 256u
#define SECTORS_PER_SERVICE 8u
static int wait_status(uint32_t mask,uint32_t ticks) {
    uint32_t deadline=apx_ticks()+ticks;
    for(unsigned spins=1;;++spins) {
        card.last_status=apx_spr_read(CONTROL);
        if(card.last_status&mask)return 0;
        if(spins%SPINS_PER_SERVICE==0) {
            service();
            if(apx_deadline_expired(deadline))break;
        }
    }
    ++card.errors;return APX_SD_TIMEOUT;
}
static unsigned sectors_since_service;
static void service_sector(void) { if(++sectors_since_service>=SECTORS_PER_SERVICE) { sectors_since_service=0;service(); } }
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
/* CMD6 (SWITCH_FUNC), group 1 function 1 = high speed, as stock 0x207514c: a check (mode 0) then the switch (mode 1), each
 * reading the 64-byte status, which must report function 1 selected in the low nibble of byte 16 and no controller CRC
 * error (CONTROL bit 11). Returns 1 only if the card is now in high-speed mode. Any failure leaves the card at default speed,
 * and the error counter untouched. */
static int switch_high_speed(void) {
    if(!((bits(card.csd,95,84)>>10)&1))return 0; /* command class 10 = the card implements CMD6 (physical layer 1.10 and later) */
    uint32_t errors=card.errors;
    int ok=0;
    for(unsigned pass=0;pass<2;++pass) {
        if(apx_dma_invalidate(bounce,64))break;
        apx_spr_write(DMA_ADDRESS,(uintptr_t)bounce);
        acknowledge(DATA_DONE);
        apx_spr_write(DMA_LENGTH,64);
        apx_spr_write(EVENT,apx_spr_read(EVENT)|0x10u);
        apx_spr_write(CONTROL,apx_spr_read(CONTROL)&~2u);
        int result=command(6,pass?0x80ffff01u:0x00ffff01u,0,0);
        if(!result)result=wait_status(DATA_DONE,16384);
        uint32_t status=apx_spr_read(CONTROL);
        acknowledge(DATA_DONE);apx_spr_write(DMA_LENGTH,0);
        if(result||(status&0x800u))break;
        apx_dma_invalidate(bounce,64);
        if((bounce[16]&15)!=1)break;
        ok=pass;
    }
    card.errors=errors;
    return ok;
}
/* Four-bit bus. The pins are stock 0x2050ac8..0x2050b30 (its 4-bit branch): DAT1..DAT3 are taken to be PA3 and PE10/PE11 (an inference, not measured), each routed with
 * the peripheral bit (0x8100/0x8110), a pull-up (0x8080/0x8090), no pull-down (0x80c0/0x80d0) and as an input (0x8040/0x8050). Their previous state is saved and put back
 * whenever the wide bus is left, because on some boards these pads belong to something else (Silvergear: camera power-down and printer motor phases). */
static unsigned wide_probe_result; /* what the last four-bit attempt came to (APX_SD_WIDE_*), kept across the restart it may cause */
static uint32_t wide_probe_detail;
#if APX_SD_BUS_WIDTH_MAX>=4
static unsigned wide_routed,wide_blocked; /* wide_blocked: four bits failed on the card whose CID is wide_blocked_cid; stay on one line until another card shows up */
static uint32_t wide_saved[8];
static uint8_t wide_blocked_cid[16];
static const struct { uint16_t reg,mask;uint8_t set; } wide_route[8]={
    {0x8100,0x0008,1},{0x8110,0x0c00,1},{0x8080,0x0008,1},{0x8090,0x0c00,1},
    {0x80c0,0x0008,0},{0x80d0,0x0c00,0},{0x8040,0x0008,1},{0x8050,0x0c00,1}};
static void wide_pins(unsigned on) {
    if(on==wide_routed)return;
    if(on) {
        for(unsigned i=0;i<8;++i) {
            uint32_t value=apx_spr_read(wide_route[i].reg);
            wide_saved[i]=value&wide_route[i].mask;
            apx_spr_write(wide_route[i].reg,(value&~(uint32_t)wide_route[i].mask)|(wide_route[i].set?wide_route[i].mask:0));
        }
    } else {
        for(unsigned i=8;i-- >0;)
            apx_spr_write(wide_route[i].reg,(apx_spr_read(wide_route[i].reg)&~(uint32_t)wide_route[i].mask)|wide_saved[i]);
    }
    wide_routed=on;
}
/* ACMD13, the 64-byte SD status, in the bus width the card is currently in. Its first byte carries DAT_BUS_WIDTH (bits 7:6: 00 one bit, 10 four bits), and the rest is
 * static, so the same 64 bytes read on one line and on four prove that all four lines deliver data (a floating or unwired DAT1..DAT3 gives a CRC error, a timeout or other bytes). */
static int read_sd_status(uint8_t *out) {
    if(apx_dma_invalidate(bounce,64))return APX_SD_RANGE;
    int result=command(55,card.rca,0,0);
    if(result)return result;
    apx_spr_write(DMA_ADDRESS,(uintptr_t)bounce);
    acknowledge(DATA_DONE);
    apx_spr_write(DMA_LENGTH,64);
    apx_spr_write(EVENT,apx_spr_read(EVENT)|0x10u);
    apx_spr_write(CONTROL,apx_spr_read(CONTROL)&~2u);
    result=command(13,0,0,0);
    if(!result)result=wait_status(DATA_DONE,16384);
    uint32_t status=apx_spr_read(CONTROL);
    acknowledge(DATA_DONE);apx_spr_write(DMA_LENGTH,0);
    if(result)return result;
    if(status&((1u<<16)|0x800u))return APX_SD_CARD_ERROR; /* receive error, controller CRC error */
    apx_dma_invalidate(bounce,64);memcpy(out,bounce,64);
    return 0;
}
static int wide_give_up(unsigned code,int result,uint32_t detail) {
    wide_probe_result=code;wide_probe_detail=detail?detail:(uint32_t)result&0xffu;
    wide_blocked=1;memcpy(wide_blocked_cid,card.cid,16);
    return WIDE_RESTART; /* the card may be in either width: the restart's shutdown clears CONTROL bit 2 and gives the pads back, its CMD0 puts the card back to one bit */
}
/* Called with the card selected, in the transfer state, one bit wide. Returns 0 with the card either four bits wide (card.bus_width 4) or, if the board/card
 * does not take it, WIDE_RESTART: the caller initialises the card again, and that run stays on one line. */
static int negotiate_wide(void) {
    uint8_t narrow[64],wide[64];
    int result=read_sd_status(narrow);
    if(result)return wide_give_up(APX_SD_WIDE_NARROW_READ,result,0);
    wide_pins(1);
    result=command(55,card.rca,0,0);
    if(!result)result=command(6,2,0,0); /* ACMD6, argument 2 = four-bit bus */
    if(result)return wide_give_up(APX_SD_WIDE_REFUSED,result,0);
    apx_spr_write(CONTROL,apx_spr_read(CONTROL)|CONTROL_FOUR_BIT);
    result=read_sd_status(wide);
    if(result)return wide_give_up(APX_SD_WIDE_READ,result,0);
    uint32_t detail=0;
    if((narrow[0]>>6)!=0||(wide[0]>>6)!=2||(wide[0]&0x3fu)!=(narrow[0]&0x3fu))detail=1u|(uint32_t)wide[0]<<8|(uint32_t)narrow[0]<<16;
    else for(unsigned i=1;i<64;++i)if(wide[i]!=narrow[i]) { detail=(i+1u)|(uint32_t)wide[i]<<8|(uint32_t)narrow[i]<<16;break; }
    if(detail)return wide_give_up(APX_SD_WIDE_MISMATCH,0,detail); /* detail: byte index + 1, the four-bit byte, the one-bit byte */
    card.bus_width=4;wide_probe_result=APX_SD_WIDE_OK;wide_probe_detail=0;
    return 0;
}
#endif
static int sd_init_once(void) {
    apx_sd_shutdown();memset(&card,0,sizeof(card));card.bus_width=1;
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
    /* Identification must run at 400 kHz or less, which this divider cannot reach (SOURCE/256 = 562.5 kHz is the slowest).
     * Earlier code wrote 359, which the register truncated to 103: 1.4 MHz. */
    apx_spr_write(CLOCK,MAX_DIVIDER);
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
    card.initialized=1; /* the helpers below need it */
#if APX_SD_BUS_WIDTH_MAX>=4
    if(wide_blocked&&memcmp(card.cid,wide_blocked_cid,16)) { wide_blocked=0;wide_probe_result=APX_SD_WIDE_NOT_TRIED;wide_probe_detail=0; } /* another card */
    if(!wide_blocked) { result=negotiate_wide();if(result)goto fail; }
#endif
    unsigned fast=0;
#if APX_SD_HIGH_SPEED
    fast=switch_high_speed();
    if(fast) {
        result=command(13,card.rca,&response,0);
        if(result||(response&0x1f00u)!=0x900u) { card.initialized=0;if(!result)result=APX_SD_CARD_ERROR;goto fail; }
    }
#endif
    if(fast) {
        /* Stock 0x2075270: CONTROL bit 31, then bit 3, then the divider (48 MHz = 2). */
        apx_spr_write(CONTROL,apx_spr_read(CONTROL)|0x80000000u);
        apx_spr_write(CONTROL,apx_spr_read(CONTROL)|8u);
        set_divider(divider_for(APX_SD_HIGH_SPEED_CLOCK_HZ));card.high_speed=1;
    } else {
        apx_spr_write(CONTROL,apx_spr_read(CONTROL)&~8u);
        set_divider(divider_for(APX_SD_CLOCK_HZ));
    }
    stream.active=0;prefetch_pending=0;last_read_end=-1;wstream.active=0;last_write_end=-1;
    card.bus_probe=wide_probe_result;card.bus_probe_detail=wide_probe_detail;
    return 0;
fail:
    card.bus_probe=wide_probe_result;card.bus_probe_detail=wide_probe_detail; /* kept for diagnosis: a failed init still says how far four bits got */
    apx_sd_shutdown();return result;
}
/* After a failed four-bit attempt the card is not ready to be initialised again at once: measured on a Silvergear (DAT1..DAT3 not wired), the init that followed
 * immediately never got past ACMD41 ("not ready" for 2 s) while the same init one second later succeeded. So the restart waits first, and retries with longer waits. */
#ifndef APX_SD_WIDE_SETTLE_TICKS
#define APX_SD_WIDE_SETTLE_TICKS 16384u /* 500 ms, doubling on every retry (0.5, 1, 2, 4 s) */
#endif
static int sd_init(void) {
    int result=sd_init_once();
    if(result==WIDE_RESTART) { /* the four-bit attempt failed and blocked itself: this run uses one data line */
        for(unsigned attempt=0;attempt<4;++attempt) {
            uint32_t deadline=apx_ticks()+(APX_SD_WIDE_SETTLE_TICKS<<attempt);
            while(!apx_deadline_expired(deadline))service();
            result=sd_init_once();
            if(!result)break;
        }
    }
    return result;
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
    card.reads+=previous.reads;card.writes+=previous.writes;card.errors+=previous.errors;card.streams+=previous.streams;
    card.retries=previous.retries+1;card.last_failure_command=failed_command;card.last_failure_status=failed_status;
    if(result)return result;
    /* Do not replay a filesystem operation against a replaced card. */
    if(card.sectors!=previous.sectors||memcmp(card.cid,previous.cid,sizeof(card.cid))) {
        apx_sd_shutdown();return APX_SD_NOT_READY;
    }
    return apx_sd_set_clock(previous.clock_hz);
}
static int sync_wait(void);
static int wstream_stop(void);
/* Arm the receive DMA for one block at `target` (stock 0x2074f24: address, acknowledge, length, event bit 4, DAT0 clock off). */
static void arm_receive(void *target) {
    apx_spr_write(DMA_ADDRESS,(uintptr_t)target);
    acknowledge(DATA_DONE);
    apx_spr_write(DMA_LENGTH,512);
    apx_spr_write(EVENT,apx_spr_read(EVENT)|0x10u);
    apx_spr_write(CONTROL,apx_spr_read(CONTROL)&~2u);
}
/* CMD12, then wait for the card to be back in the transfer state (the same two-step wait a write uses). */
static int finish_block(void) { /* wait for the armed block; returns an error, counting it */
    int result=wait_status(DATA_DONE,16384);
    uint32_t status=apx_spr_read(CONTROL);
    acknowledge(DATA_DONE);
    /* Only bit 16 is a receive error flag. Bits 22:21 are the CRC-status token of a WRITE; on reads they vary with identical,
     * correct data (stock 0x20750d0 checks bit 16 alone for reads), so they are not looked at. */
    if(!result&&(status&(1u<<16))) { result=APX_SD_CARD_ERROR;++card.errors; }
    return result;
}
static void start_prefetch(uint8_t *destination) { /* destination: the caller's next sector, or NULL for a driver block; never for the card's last block (see read_block) */
    if(!stream.active||(uint64_t)stream.next+1>=card.sectors)return;
    prefetch_target=destination?destination:prefetch_buf[prefetch_idx];
    arm_receive(prefetch_target);prefetch_pending=1;
}
static int stream_stop(void) {
    if(wstream.active)return wstream_stop();
    if(!stream.active)return 0;
    stream.active=0;
    if(prefetch_pending) { /* let the block that is already on its way finish: the stream is stopped between blocks, as before */
        prefetch_pending=0;
        int pending=finish_block();
        if(pending) { apx_spr_write(DMA_LENGTH,0);apx_sd_shutdown();return pending; }
    }
    int result=command(12,0,0,0);
    apx_spr_write(DMA_LENGTH,0);
    if(result) { apx_sd_shutdown();return result; }
    return sync_wait();
}
/* One block into `target` (16-byte aligned, 512 bytes). `want_stream`: begin or continue a CMD18 stream; otherwise CMD17.
 * The card's last block is never read as part of a stream: a stream that runs to the end of the card is ended by the card
 * itself, and some cards then flag OUT_OF_RANGE in the response to the CMD12 that stops it (found on a Samsung 64 GB card;
 * the driver took it for a failure and shut the card down). A CMD17 for that block avoids depending on how a card reports it. */
static int read_block(uint32_t sector,uint8_t *target,int want_stream,uint8_t *next_target) {
    int result;
    const int last_block=(uint64_t)sector+1>=card.sectors;
    const int streaming=want_stream&&!last_block;
    if(stream.active&&stream.next==sector&&streaming) {
        if(prefetch_pending) { /* the block was requested when the previous one finished: take it */
            prefetch_pending=0;
            result=finish_block();
            if(result) { apx_spr_write(DMA_LENGTH,0);apx_sd_shutdown();return result; }
            stream.next=sector+1;
            if(prefetch_target==target) { /* it was received straight into the caller's buffer: nothing to copy */
                start_prefetch(next_target);
                return 0;
            }
            const unsigned done=prefetch_idx;
            prefetch_idx^=1;
            start_prefetch(next_target); /* first: the card is already sending the next block */
            apx_dma_invalidate(prefetch_buf[done],512);
            memcpy(target,prefetch_buf[done],512);
            return 0;
        }
        arm_receive(target); /* the stream is already running: no command */
    } else {
        if((result=stream_stop()))return result;
        arm_receive(target); /* armed before the command, as stock does */
        uint32_t argument=card.high_capacity?sector:sector*512u;
        result=command(streaming?18:17,argument,0,0);
        if(result) { apx_spr_write(DMA_LENGTH,0);apx_sd_shutdown();return result; }
        if(streaming) {
            stream.active=1;++card.streams;
            /* the blocks must hold no dirty cache lines that could be written back over received data later */
            apx_dma_invalidate(prefetch_buf,sizeof(prefetch_buf));prefetch_idx=0;
        }
    }
    result=finish_block();
    if(result) { apx_spr_write(DMA_LENGTH,0);apx_sd_shutdown();return result; }
    if(stream.active) { stream.next=sector+1;start_prefetch(next_target); }
    else apx_spr_write(DMA_LENGTH,0);
    return 0;
}
static int sd_read(uint32_t sector,void *buffer,unsigned count) {
    if(!card.initialized)return APX_SD_NOT_READY;
    if(!range(sector,buffer,count))return APX_SD_RANGE;
    uint8_t *out=buffer;
    /* Straight into the caller's buffer when it is 16-byte aligned (the cache maintenance works in 16-byte lines);
     * otherwise through the aligned bounce block. */
    const int direct=!((uintptr_t)buffer&15);
    if(direct&&apx_dma_invalidate(buffer,count*512u))return APX_SD_RANGE;
    for(unsigned i=0;i<count;++i) {
      for(unsigned attempt=0;;++attempt) {
        const uint32_t block=sector+i;
        uint8_t *target=direct?out+i*512u:bounce;
        if(!direct&&apx_dma_invalidate(bounce,sizeof(bounce)))return APX_SD_RANGE;
        /* A stream pays off for a multi-sector request or a read that continues the last one; an isolated sector is one CMD17. */
        /* the next sector of this call is received straight into the caller's buffer (no read-ahead block, no copy) */
        int result=read_block(block,target,count-i>1||(int64_t)block==last_read_end,direct&&count-i>1?out+(i+1)*512u:NULL);
        if(result) {
            if(attempt<2&&!retry_sector())continue;
            return result;
        }
        if(!direct) { apx_dma_invalidate(bounce,sizeof(bounce));memcpy(out+i*512u,bounce,512); }
        ++card.reads;last_read_end=(int64_t)block+1;service_sector();break;
      }
    }
    if(direct)apx_dma_invalidate(buffer,count*512u);
    return 0;
}
static int sync_wait(void) {
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
static int sd_sync(void) {
    if(!card.initialized)return APX_SD_NOT_READY;
    int result=stream_stop(); /* a card that is still sending a stream never reports the transfer state */
    return result?result:sync_wait();
}
/* The card has finished programming: DAT0 high (stock 02050e28 clocks it with CONTROL bit 1 while waiting for bit 20). */
static int wait_ready(void) {
    apx_spr_write(CONTROL,apx_spr_read(CONTROL)|2u);
    int result=wait_status(CARD_READY,65536); /* 2 s, including slow SDXC programming */
    apx_spr_write(CONTROL,apx_spr_read(CONTROL)&~2u);
    return result;
}
/* Close the open write stream: CMD12, then wait until the card is back in the transfer state, which is when everything written is programmed. */
static int wstream_stop(void) {
    if(!wstream.active)return 0;
    wstream.active=0;
    int result=command(12,0,0,0);
    apx_spr_write(DMA_LENGTH,0);
    if(result) { apx_sd_shutdown();return result; }
    return sync_wait();
}
/* Stock 020750d0 (write): the data-response token in CONTROL bits 22:21 must be 10b (the card's "010 = accepted"), and bit 16 (transfer error) clear. */
static int write_status_ok(uint32_t status) { return !(status&(1u<<16))&&(status&0x600000u)==0x400000u; }
/* One block from `source` (16-byte aligned, already cleaned from the cache). `want_stream`: begin or continue a CMD25 stream; otherwise CMD24. */
static int write_block(uint32_t sector,const uint8_t *source,int want_stream) {
    int result;
    if(!(want_stream&&wstream.active&&wstream.next==sector)) {
        if((result=stream_stop()))return result; /* the other open stream, or this one when the block does not continue it */
        uint32_t argument=card.high_capacity?sector:sector*512u;
        /* Stock issues the write command before arming the transmit DMA. */
        if((result=command(want_stream?25:24,argument,0,0)))return result;
        if(want_stream) { wstream.active=1;++card.wstreams; }
    }
    /* TX 02050df0 uses event bits 4/5, RX uses bit4 alone. */
    apx_spr_write(DMA_ADDRESS,(uintptr_t)source);
    acknowledge(DATA_DONE);
    apx_spr_write(DMA_LENGTH,512);
    apx_spr_write(EVENT,apx_spr_read(EVENT)|0x30u);
    result=wait_status(DATA_DONE,16384);
    uint32_t status=apx_spr_read(CONTROL);
    acknowledge(DATA_DONE);
    apx_spr_write(DMA_LENGTH,0);
    card.last_write_status=status;
    if(!result&&!write_status_ok(status)) { result=APX_SD_CARD_ERROR;++card.errors; }
    if(result)return result;
    if(wstream.active) { /* the card holds DAT0 low while it programs; the next block may only follow when it lets go (no CMD13 per block, as stock) */
        wstream.next=sector+1;wstream.last_tick=apx_ticks();
        return wait_ready();
    }
    return sync_wait(); /* CMD24: programming complete and the card back in the transfer state, as before */
}
static int sd_write(uint32_t sector,const void *buffer,unsigned count) {
    if(!card.initialized)return APX_SD_NOT_READY;
    if(!range(sector,buffer,count))return APX_SD_RANGE;
    if(card.write_protected)return APX_SD_WRITE_PROTECTED;
    const uint8_t *source=buffer;
    /* DMA straight from the caller's buffer when it is 16-byte aligned (the cache maintenance works in 16-byte lines); otherwise through the bounce block. */
    const int direct=!((uintptr_t)buffer&15);
    if(direct&&apx_dma_clean(buffer,count*512u))return APX_SD_RANGE;
    for(unsigned i=0;i<count;++i) {
      for(unsigned attempt=0;;++attempt) {
        const uint32_t block=sector+i;
        const uint8_t *data=source+i*512u;
        if(!direct) {
            memcpy(bounce,data,512);
            if(apx_dma_clean(bounce,sizeof(bounce)))return APX_SD_RANGE;
            data=bounce;
        } else if(attempt&&apx_dma_clean(data,512))return APX_SD_RANGE; /* a replay after the card was reinitialized starts from clean lines again */
        /* A stream pays off for a multi-sector request or a write that continues the last one; an isolated sector is one CMD24. */
        int result=write_block(block,data,count-i>1||(wstream.active&&wstream.next==block)||(int64_t)block==last_write_end);
        if(result) {
            apx_sd_shutdown();
            if(attempt<2&&result!=APX_SD_WRITE_PROTECTED&&!retry_sector())continue;
            return result;
        }
        ++card.writes;last_write_end=(int64_t)block+1;service_sector();break;
      }
    }
    return 0;
}
int apx_sd_write(uint32_t sector,const void *buffer,unsigned count) {
    ++in_operation;
    int result=sd_write(sector,buffer,count);
    --in_operation;
    return result;
}
/* Closes a write stream that has been idle: call it from the main loop (apx_maintenance() does, when the SD driver is linked). */
void apx_sd_poll(void) {
    if(in_operation||!wstream.active||(int32_t)(apx_ticks()-wstream.last_tick)<(int32_t)APX_SD_WRITE_IDLE_TICKS)return;
    ++in_operation;
    (void)wstream_stop();
    --in_operation;
}
static int sd_set_clock(unsigned maximum_hz) {
    unsigned limit=card.high_speed?APX_SD_HIGH_SPEED_CLOCK_HZ:APX_SD_CLOCK_HZ;
    if(!card.initialized||maximum_hz<APX_SD_CLOCK_SOURCE_HZ/(MAX_DIVIDER+1)||maximum_hz>limit)return APX_SD_RANGE;
    int result=stream_stop();
    if(result)return result;
    set_divider(divider_for(maximum_hz));return 0;
}
/* The public calls: apx_sd_poll() (run from apx_maintenance(), including from inside these calls' waits) leaves the card alone while one is running. */
int apx_sd_init(void) { ++in_operation;int result=sd_init();--in_operation;return result; }
int apx_sd_read(uint32_t sector,void *buffer,unsigned count) { ++in_operation;int result=sd_read(sector,buffer,count);--in_operation;return result; }
int apx_sd_sync(void) { ++in_operation;int result=sd_sync();--in_operation;return result; }
int apx_sd_set_clock(unsigned maximum_hz) { ++in_operation;int result=sd_set_clock(maximum_hz);--in_operation;return result; }
void apx_sd_shutdown(void) {
    /* EVENT writes omit the stock interrupt-enable bit6. CPU interrupt-source
     * assignments for SD are unverified; preserve unrelated CPU masks. */
    apx_spr_write(CONTROL,apx_spr_read(CONTROL)&~(3u|CONTROL_FOUR_BIT));
#if APX_SD_BUS_WIDTH_MAX>=4
    wide_pins(0); /* give the extra pads back */
#endif
    apx_spr_write(EVENT,0x28000);apx_spr_write(DMA_LENGTH,0);
    card.initialized=0;stream.active=0;prefetch_pending=0;wstream.active=0;
}
