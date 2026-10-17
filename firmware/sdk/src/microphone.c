#include <appotech/microphone.h>
#include <appotech/audio.h>
#include <appotech/soc.h>
#include "board.h"
static struct apx_mic_info info;
static unsigned initialized,servicing;
static uint32_t start,deadline,saved_analog;
static const unsigned passive[]={0x9100,0x9104,0x9110,0x9118,0x911c,0x9120};
static uint32_t saved[6];
static void update(unsigned reg,uint32_t mask,uint32_t value) {
 apx_spr_write(reg,(apx_spr_read(reg)&~mask)|(value&mask));
}
static void halt(void) {
 apx_spr_write(0x9114,0xf00);update(0x9100,15,0);update(0x9118,9,0);
 apx_spr_write(0x9108,0);apx_spr_write(0x910c,0);
}
static int failure(unsigned error) {
 halt();info.error=error;info.state=APX_MIC_ERROR;return error;
}
int apx_mic_init(void) {
 if(initialized)return info.state==APX_MIC_ERROR?info.error:0;
 /* The speaker core owns/restores the shared audio clock and analog domain. */
 int error=apx_audio_init();if(error)return error;
 saved_analog=apx_spr_read(0x886c);
 for(unsigned i=0;i<6;++i)saved[i]=apx_spr_read(passive[i]);
 update(0x4800,1u<<18,0);halt();update(0x886c,0x3c1f,0x3c1f);
 initialized=1;info.state=APX_MIC_READY;info.error=0;return 0;
}
int apx_mic_buffer_busy(const void *buffer,size_t bytes) {
 if(info.state!=APX_MIC_CAPTURING||!bytes)return 0;
 uintptr_t address=(uintptr_t)buffer;
 return address<=info.buffer?bytes>info.buffer-address:address-info.buffer<info.bytes;
}
int apx_mic_capture_start(int16_t *pcm,size_t bytes,unsigned rate) {
 if(info.state==APX_MIC_CAPTURING)return APX_MIC_BUSY;
 unsigned control;
 if(rate==8000)control=APX_MIC_CONTROL_8000;
 else if(rate==16000)control=APX_MIC_CONTROL_16000;
 else if(rate==32000)control=APX_MIC_CONTROL_32000;
 else return APX_MIC_UNSUPPORTED;
 uintptr_t address=(uintptr_t)pcm;
 if((address&31)||address<APX_RAM_BASE||address>=APX_RAM_END||bytes<32||bytes>65536||
    (bytes&31)||bytes>APX_RAM_END-address)return APX_MIC_RANGE;
 if(apx_audio_buffer_busy(pcm,bytes))return APX_MIC_BUSY;
 int error=apx_mic_init();if(error)return error;
 servicing=1;halt();apx_spr_write(0x9100,control);apx_spr_write(0x9104,0x7c0002);
 apx_spr_write(0x9110,0x780a0005);apx_spr_write(0x9118,0x55500000);
 apx_spr_write(0x9114,0x40000000);apx_spr_write(0x911c,0x40003000);
 apx_spr_write(0x9120,0x01007800);apx_spr_write(0x9118,0x5553330f);update(0x9100,2,2);
 apx_dma_clean(pcm,bytes);apx_dma_invalidate(pcm,bytes);
 apx_spr_write(0x9108,address);apx_spr_write(0x910c,bytes/4-1);
 update(0x9100,4,4);update(0x9100,8,8);
 apx_spr_write(0x9114,apx_spr_read(0x9114)|0x80000000u);
 uint32_t end=apx_ticks()+328;
 while(!(apx_spr_read(0x9114)&4)) {
  if(apx_deadline_expired(end)){++info.timeouts;servicing=0;return failure(APX_MIC_TIMEOUT);}
  apx_maintenance();
 }
 apx_spr_write(0x9114,0x400);
 info.error=info.status=info.ticks=0;info.rate=rate;info.bytes=bytes;info.buffer=address;
 start=apx_ticks();deadline=start+(bytes/2*32768u+rate-1)/rate+16384u;
 info.state=APX_MIC_CAPTURING;update(0x9100,1,1);apx_spr_write(0x9114,0x30000);
 servicing=0;return 0;
}
void apx_mic_poll(void) {
 if(servicing||info.state!=APX_MIC_CAPTURING)return;
 info.status=apx_spr_read(0x9114);
 /* Bit 1 is half-buffer; bit 0 is the complete buffer. */
 if(info.status&1){info.ticks=apx_ticks()-start;halt();
  apx_dma_invalidate((void *)(uintptr_t)info.buffer,info.bytes);
  info.state=APX_MIC_DONE;++info.completed;return;}
 if(apx_deadline_expired(deadline)){++info.timeouts;failure(APX_MIC_TIMEOUT);}
}
void apx_mic_stop(void) {
 if(!initialized)return;
 halt();info.state=APX_MIC_READY;info.error=info.bytes=info.buffer=0;
}
void apx_mic_shutdown(void) {
 if(!initialized)return;
 apx_mic_stop();
 for(unsigned i=0;i<6;++i)apx_spr_write(passive[i],saved[i]&
     (passive[i]==0x9100?~15u:passive[i]==0x9118?~9u:~0u));
 update(0x886c,0x3c1f,saved_analog);initialized=servicing=0;info.state=APX_MIC_OFF;
}
void apx_mic_get_info(struct apx_mic_info *out) {if(out)*out=info;}
