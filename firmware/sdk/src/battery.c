#include <appotech/battery.h>
#include <appotech/adc.h>
#include <appotech/soc.h>
#include <appotech/debug.h>
#include <appotech/trace.h>
#include <stdio.h>
#include <string.h>
static struct apx_battery_info info;
static unsigned phase,pairs,sum_battery,sum_reference,discard,logged;
#define REFERENCE_ENABLE 0x8000u /* SPR 0x8868 bit 15 */
static uint32_t saved_reference;
static uint16_t sample;
static uint32_t next_sample;
static const uint16_t thresholds[5]={3660,3762,3860,3964,4060};
int apx_battery_init(void){
 if(info.initialized)return 0;
 memset(&info,0,sizeof(info));info.level=0xff;
 uint32_t calibration=apx_mmio_read32(0x1fec);
 if(calibration<400||calibration>1600)return -1;
 info.calibration=calibration;apx_adc_acquire(APX_ADC_BATTERY);info.initialized=1;
 /* Channel 11 samples an internal reference that only exists while 0x8868 bit 15 is set.
  * The stock application sets it at start-up and a cold SPI boot does not; without it the
  * reference reads ~620 instead of ~246 and every voltage comes out invalid. Found by diffing
  * a stock-launched camera against an SPI boot (see research/battery-spi.md). */
 saved_reference=apx_spr_read(0x8868)&REFERENCE_ENABLE;
 apx_spr_write(0x8868,apx_spr_read(0x8868)|REFERENCE_ENABLE);
 phase=pairs=sum_battery=sum_reference=discard=logged=0;next_sample=apx_ticks()+1100; /* ~33 ms to settle */
 apx_trace(APX_TRACE_BATTERY_ADC,(apx_spr_read(0x9170)&0xfff)<<12|(apx_spr_read(0x8840)>>20&0xfff));
 return 0;
}
static void publish(void){
 info.adc_battery=sum_battery>>4;info.adc_reference=sum_reference>>4;
 if(info.adc_reference&&info.adc_battery<1023&&info.adc_reference<1023){
  uint32_t mv=2u*((uint32_t)info.adc_battery*info.calibration/info.adc_reference)+20;
  if(mv>=2500&&mv<=5000){unsigned level=0;while(level<5&&mv>thresholds[level])++level;
   if(info.level>5||(level<info.level&&mv+30<thresholds[level])||
      (level>info.level&&mv>thresholds[info.level]+30u))info.level=level;
   info.millivolts=mv;info.valid=1;info.last_ticks=apx_ticks();++info.updates;
  }else{info.valid=0;++info.invalid_samples;}
 }else{info.valid=0;++info.invalid_samples;}
 pairs=sum_battery=sum_reference=0;
}
void apx_battery_poll(void){
 if(!info.initialized)return;
 uint32_t now=apx_ticks();info.pm_control=apx_spr_read(0x88e0);info.external_power=(info.pm_control>>21)&1;
 if(info.valid&&now-info.last_ticks>3u*32768)info.valid=0;
 if(phase==1||phase==3){uint16_t value;int result=apx_adc_result(APX_ADC_BATTERY,&value);
  if(!result)return;
  if(result<0){++info.timeouts;info.valid=0;phase=pairs=sum_battery=sum_reference=discard=0;next_sample=now+1638;return;}
  /* The sample-and-hold needs a conversion to settle on the newly selected
   * channel: the first result after a channel change can still carry the
   * previous channel (a cold boot read the 11 reference as ~ the battery level).
   * Stock waits between select and start; here the first conversion is dropped. */
  if(discard){discard=0;if(apx_adc_begin(APX_ADC_BATTERY,phase==1?9:11))phase=phase==1?0:2;return;}
  if(phase==1){sample=value;phase=2;}else{sum_battery+=sample;sum_reference+=value;
   if(logged<16)apx_trace(APX_TRACE_BATTERY_PAIR+logged++,(unsigned)(sample&0xfff)<<12|(value&0xfff));
   if(++pairs==16)publish();
   phase=0;next_sample=now+1638;}
 }
 if(phase==2){if(!apx_adc_begin(APX_ADC_BATTERY,11)){phase=3;discard=1;}}
 else if(!phase&&(int32_t)(now-next_sample)>=0){if(!apx_adc_begin(APX_ADC_BATTERY,9)){phase=1;discard=1;}}
}
void apx_battery_get_info(struct apx_battery_info *out){if(out)*out=info;}
void apx_battery_shutdown(void){
 if(info.initialized){apx_adc_release(APX_ADC_BATTERY);
  apx_spr_write(0x8868,(apx_spr_read(0x8868)&~REFERENCE_ENABLE)|saved_reference);}
 info.initialized=info.valid=0;phase=0;}
uint32_t apx_battery_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *size){
 (void)input;
 *size=0;if(command||length)return APXD_INVALID;
 if(!info.initialized)return APXD_UNSUPPORTED;
 apx_battery_poll();int n=snprintf((char *)output,APXD_MAX_PAYLOAD,
 "{\"valid\":%s,\"millivolts\":%lu,\"level\":%u,\"level_max\":5,\"external_power\":%s,"
 "\"adc_battery\":%u,\"adc_reference\":%u,\"calibration\":%u,\"updates\":%lu,"
 "\"timeouts\":%lu,\"invalid_samples\":%lu,\"last_ticks\":%lu,\"pm_control\":%lu}",
 info.valid?"true":"false",(unsigned long)info.millivolts,info.level,info.external_power?"true":"false",
 info.adc_battery,info.adc_reference,info.calibration,(unsigned long)info.updates,(unsigned long)info.timeouts,
 (unsigned long)info.invalid_samples,(unsigned long)info.last_ticks,(unsigned long)info.pm_control);
 if(n<0||n>=APXD_MAX_PAYLOAD)return APXD_INVALID;
 *size=n;return 0;
}

#if defined(APX_SPI_TRACE) && APX_SPI_TRACE
static unsigned probe_convert(unsigned channel) {
 apx_spr_write(0x9170,0x10u|channel);
 for(volatile unsigned i=0;i<10;++i); /* stock waits a few cycles between select and start */
 apx_spr_write(0x9170,0x90u|channel);
 for(volatile unsigned i=0;i<4;++i)(void)apx_spr_read(0x9170);
 for(unsigned n=0;n<200000&&(apx_spr_read(0x9170)&0x40);++n);
 return apx_spr_read(0x9178)&0x3ff;
}
void apx_battery_probe(unsigned n) {
 if(!apx_trace_active())return;
 uint32_t control=apx_spr_read(0x9170),gate=apx_spr_read(0x8840);
 apx_spr_write(0x8840,gate&~(1u<<29));apx_spr_write(0x9174,71);
 unsigned first9=probe_convert(9),first11=probe_convert(11);
 unsigned second9=probe_convert(9),second11=probe_convert(11);
 apx_spr_write(0x9170,control&~0xc0u);apx_spr_write(0x8840,gate);
 apx_trace(0x50+n,second9<<12|second11);
 apx_trace(0x58+n,first9<<12|first11);
}
#endif
