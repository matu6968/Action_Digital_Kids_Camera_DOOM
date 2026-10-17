#include <appotech/audio.h>
#include <appotech/soc.h>
#include "board.h"
#include <string.h>
#define DAC 0x9140u
static struct apx_audio_info info;
static unsigned initialized,servicing;
static uintptr_t owned_pcm;
__attribute__((weak,noinline)) int apx_mic_buffer_busy(const void *p,size_t bytes) {(void)p;(void)bytes;return 0;}
__attribute__((weak,noinline)) void apx_mic_shutdown(void) {}
int apx_audio_buffer_busy(const void *p,size_t bytes) {
 if((info.state!=APX_AUDIO_PLAYING&&info.state!=APX_AUDIO_DRAINING)||!bytes)return 0;
 uintptr_t address=(uintptr_t)p;
 return address<=owned_pcm?bytes>owned_pcm-address:address-owned_pcm<info.bytes;
}
static uint32_t start,deadline,drain_until;
static unsigned stream_active;           /* gapless streaming (see apx_audio_stream_*) */
static uint32_t stream_pending;          /* halfwords of the queued block that has not started yet, 0 = none */
static unsigned stream_rate;
static uint32_t stream_underruns;
static uint32_t saved_gate,saved_clock_select,saved_clock_divider,saved_analog,saved_analog_control,saved_amplifier;
static const uint16_t coefficients[27]={0x0034,0x7f98,0xc067,0x0068,0x7f2d,0xc0cf,0x00ce,0x7e54,0xc19c,
 0x0196,0x7c94,0xc32d,0x031a,0x78d7,0xc634,0x0603,0x704f,0xcc06,0x0a00,0x5eda,0xd3ff,
 0x11f1,0x3287,0xe3e2,0x19a8,0xe37a,0xf350};
static void update(unsigned reg,uint32_t mask,uint32_t value) {
 apx_spr_write(reg,(apx_spr_read(reg)&~mask)|(value&mask));
}
static int transaction(unsigned index,uint32_t value,int writing,uint32_t *result) {
 apx_spr_write(0x915c,(index<<24)|(writing?0x40000000u:0)|value);
 uint32_t end=apx_ticks()+328; /* 10 ms; stock waits for busy bit 31. */
 while(apx_spr_read(0x915c)&0x80000000u){
  if(apx_deadline_expired(end)){++info.timeouts;return APX_AUDIO_TIMEOUT;}
  apx_maintenance();
 }
 if(result)*result=apx_spr_read(0x9160);
 return 0;
}
static int write(unsigned index,uint32_t value) {return transaction(index,value,1,NULL);}
static int read(unsigned index,uint32_t *value) {return transaction(index,0,0,value);}
static void halt(void) {
 apx_spr_write(DAC+12,7);apx_spr_write(DAC+8,0);apx_spr_write(DAC+4,0);apx_spr_write(0x9158,0);
}
static int gain(unsigned playing) {
 unsigned value=info.muted||!playing?0:info.volume*32767u/100u;
 int error=write(2,16);if(!error)error=write(3,value);if(!error)error=write(4,7);if(!error)error=write(2,16);
 update(0x8874,255,value?0xbb:0);return error;
}
static int failure(unsigned error) {
 halt();update(0x8874,255,0);info.error=error;info.state=APX_AUDIO_ERROR;return error;
}
int apx_audio_init(void) {
 if(initialized)return info.state==APX_AUDIO_ERROR?info.error:0;
 memset(&info,0,sizeof(info));info.volume=40;servicing=1;
 saved_gate=apx_spr_read(0x8840);saved_clock_select=apx_spr_read(0x8848);saved_clock_divider=apx_spr_read(0x884c);
 saved_analog=apx_spr_read(0x886c);saved_analog_control=apx_spr_read(0x8870);saved_amplifier=apx_spr_read(0x8874);
 apx_spr_write(0x4800,apx_spr_read(0x4800)&~(1u<<6)); /* Keep vendor DAC IRQ masked. */
 halt();update(0x8840,(1u<<24)|(1u<<30),0);
 update(0x8848,0x300,0);update(0x884c,0x0f000000,0x0b000000);
 apx_spr_write(0x886c,apx_spr_read(0x886c)|0x0158360f);apx_spr_write(0x8870,0x124);
 apx_spr_write(0x886c,apx_spr_read(0x886c)|0x0158260f);apx_spr_write(0x8870,apx_spr_read(0x8870)|0x24);
 apx_spr_write(0x886c,apx_spr_read(0x886c)&0x3ffffdff);
 apx_spr_write(0x8870,(apx_spr_read(0x8870)|2)&~0x200u);apx_spr_write(0x8870,apx_spr_read(0x8870)|1);
 apx_spr_write(0x8874,0x200);apx_spr_write(DAC,0x10020);
 int error=0;
 for(unsigned i=0;i<27&&!error;++i)error=write(15,0x00800000u|(i<<18)|coefficients[i]);
 static const uint32_t setup[]={0xa014,0x19814,0x19833,0x19833,0x33};
 for(unsigned i=0;i<5&&!error;++i)error=write(10+i,setup[i]);
 if(!error)error=write(2,128);
 uint32_t value=0;if(!error)error=read(0,&value);if(!error)error=write(0,value|1);
 if(!error)error=write(3,0);
 initialized=1;info.state=APX_AUDIO_READY;servicing=0;
 return error?failure(error):0;
}
static int selector(unsigned rate) {
 if(rate==8000)return APX_AUDIO_SELECTOR_8000;
 if(rate==16000)return APX_AUDIO_SELECTOR_16000;
 if(rate==32000)return APX_AUDIO_SELECTOR_32000;
 return -1;
}
int apx_audio_play(const int16_t *pcm,size_t bytes,unsigned rate) {
 if(info.state==APX_AUDIO_PLAYING||info.state==APX_AUDIO_DRAINING)return APX_AUDIO_BUSY;
 uintptr_t address=(uintptr_t)pcm;int setting=selector(rate);
 if(setting<0)return APX_AUDIO_UNSUPPORTED;
 if((address&31)||address<APX_RAM_BASE||address>=APX_RAM_END||bytes<32||bytes>65536||
    (bytes&31)||bytes>APX_RAM_END-address)return APX_AUDIO_RANGE;
 if(apx_mic_buffer_busy(pcm,bytes))return APX_AUDIO_BUSY;
 int error=apx_audio_init();if(error)return error;
 servicing=1;uint32_t value=0;error=read(0,&value);
 if(!error)error=write(0,(value&~0xf0u)|(unsigned)setting<<4|1u);
 if(!error)error=gain(1);
 if(error){servicing=0;return failure(error);}
 apx_dma_clean(pcm,bytes);update(0x8870,0x100,0);
 apx_spr_write(DAC,0x202c);apx_spr_write(DAC+12,7);
 apx_spr_write(DAC+8,address);apx_spr_write(DAC+4,bytes/4-1);apx_spr_write(0x9158,bytes/2);
 info.error=info.status=info.ticks=0;info.rate=rate;info.bytes=bytes;info.remaining=bytes/2;
 owned_pcm=address;start=apx_ticks();deadline=start+(bytes/2*32768u+rate-1)/rate+16384u;info.state=APX_AUDIO_PLAYING;
 apx_spr_write(DAC+12,8);servicing=0;return 0;
}
/* ---- gapless streaming (added for the DOOM port) ------------------------------------------------------------------
 * Measured on the camera (doom-probe, examples/doom-probe/main.c test E3): the DAC DMA has a one-deep "next block" queue.
 * Writing source (0x9148), length in words minus one (0x9144) and then the halfword count to 0x9158 while a block is
 * playing does not restart anything: 0x9158 is the total number of halfwords still to be played, a write ADDS to it,
 * and the engine switches to the new source when the current block runs out, without a gap. The queue is one deep, so
 * a second block may only be queued once the first queued one has started, i.e. once 0x9158 has fallen to the queued
 * block's length (plus a few halfwords of jitter). If 0x9158 ever reaches 7 with event bit 2 set the engine ran dry.
 */
int apx_audio_stream_start(const int16_t *pcm,size_t bytes,unsigned rate) {
 int r=apx_audio_play(pcm,bytes,rate);
 if(r)return r;
 stream_active=1;stream_pending=0;stream_rate=rate;
 return 0;
}
int apx_audio_stream_queue(const int16_t *pcm,size_t bytes) {
 if(!stream_active||info.state!=APX_AUDIO_PLAYING)return APX_AUDIO_BUSY;
 uintptr_t address=(uintptr_t)pcm;
 if((address&31)||address<APX_RAM_BASE||address>=APX_RAM_END||bytes<32||bytes>65536||(bytes&31)||bytes>APX_RAM_END-address)
  return APX_AUDIO_RANGE;
 uint32_t rem=apx_spr_read(0x9158);
 if(stream_pending&&rem<=stream_pending+8)stream_pending=0; /* the queued block has started */
 if(stream_pending)return APX_AUDIO_BUSY;
 if(rem<=8||(apx_spr_read(0x9150)&4)){++stream_underruns;return APX_AUDIO_TIMEOUT;} /* ran dry: caller restarts */
 apx_dma_clean(pcm,bytes);
 apx_spr_write(DAC+8,address);apx_spr_write(DAC+4,bytes/4-1);apx_spr_write(0x9158,bytes/2);
 stream_pending=bytes/2;info.bytes+=bytes;
 deadline=apx_ticks()+(((rem+bytes/2)*32768u+stream_rate-1)/stream_rate)+16384u;
 return 0;
}
int apx_audio_stream_ready(void) {
 if(!stream_active||info.state!=APX_AUDIO_PLAYING)return 0;
 if(!stream_pending)return 1;
 return apx_spr_read(0x9158)<=stream_pending+8;
}
uint32_t apx_audio_stream_underruns(void) { return stream_underruns; }
int apx_audio_stream_active(void) { return stream_active&&info.state==APX_AUDIO_PLAYING; }

int apx_audio_volume(unsigned percent) {
 if(percent>100)return APX_AUDIO_INVALID;
 if(!initialized)return APX_AUDIO_INVALID;
 info.volume=percent;servicing=1;int error=gain(info.state==APX_AUDIO_PLAYING||info.state==APX_AUDIO_DRAINING);servicing=0;
 return error?failure(error):0;
}
int apx_audio_mute(int muted) {
 if(!initialized||(muted!=0&&muted!=1))return APX_AUDIO_INVALID;
 info.muted=muted;servicing=1;int error=gain(info.state==APX_AUDIO_PLAYING||info.state==APX_AUDIO_DRAINING);servicing=0;
 return error?failure(error):0;
}
void apx_audio_poll(void) {
 if(servicing||!initialized)return;
 if(stream_active&&info.state!=APX_AUDIO_PLAYING&&info.state!=APX_AUDIO_DRAINING)stream_active=0;
 if(info.state==APX_AUDIO_DRAINING){
  if(!apx_deadline_expired(drain_until))return;
  servicing=1;halt();int error=write(3,0);update(0x8874,255,0);servicing=0;
  if(error){failure(error);return;}info.state=APX_AUDIO_DONE;++info.completed;return;
 }
 if(info.state!=APX_AUDIO_PLAYING)return;
 info.status=apx_spr_read(0x9150);info.remaining=apx_spr_read(0x9158);
 if(info.status&4){info.ticks=apx_ticks()-start;info.state=APX_AUDIO_DRAINING;
  drain_until=apx_ticks()+(16*32768u+info.rate-1)/info.rate;return;}
 if(apx_deadline_expired(deadline)){++info.timeouts;failure(APX_AUDIO_TIMEOUT);}
}
void apx_audio_stop(void) {
 stream_active=0;stream_pending=0;
 if(!initialized)return;
 servicing=1;halt();int error=write(3,0);update(0x8874,255,0);servicing=0;
 info.state=APX_AUDIO_READY;info.error=0;info.remaining=0;if(error)failure(error);
}
void apx_audio_shutdown(void) {
 apx_mic_shutdown();
 if(!initialized)return;
 apx_audio_stop();servicing=1;
 uint32_t value=0;if(!read(0,&value))write(0,value&~1u);
 /* Quiesced DAC only. Preserve shared clock/power fields outside our masks. */
 update(0x8848,0x300,saved_clock_select);update(0x884c,0x0f000000,saved_clock_divider);
 update(0x886c,0xc158360f,saved_analog);apx_spr_write(0x8870,saved_analog_control);
 update(0x8874,0x3ff,saved_amplifier&~255u);update(0x8840,(1u<<24)|(1u<<30),saved_gate);
 initialized=servicing=0;info.state=APX_AUDIO_OFF;
}
void apx_audio_get_info(struct apx_audio_info *out) {if(out)*out=info;}
