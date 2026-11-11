#include <appotech/printer.h>
#include <appotech/camera.h>
#include <appotech/battery.h>
#include <appotech/gpio.h>
#include <appotech/adc.h>
#include <appotech/soc.h>
#include <appotech/debug.h>
#include "board.h"
#include "battery_internal.h"
#include "printer_image.h"
#include "printer_render.h"
#include <string.h>
#include <stdio.h>
#if APX_HAS_PRINTER
#include "printer_reference.h"
#endif
/* Per-mode default pulse energy, percent of the stock-derived timing (100..400). */
#ifndef APX_PRINTER_ENERGY_GRAY_PERCENT
#define APX_PRINTER_ENERGY_GRAY_PERCENT 100u
#endif
#ifndef APX_PRINTER_ENERGY_DOT_PERCENT
#define APX_PRINTER_ENERGY_DOT_PERCENT 100u
#endif
_Static_assert(APX_PRINTER_ENERGY_GRAY_PERCENT>=100u&&APX_PRINTER_ENERGY_GRAY_PERCENT<=400u,"gray energy default must be 100..400");
_Static_assert(APX_PRINTER_ENERGY_DOT_PERCENT>=100u&&APX_PRINTER_ENERGY_DOT_PERCENT<=400u,"dot energy default must be 100..400");
/* Fixed half-row period for photo jobs, in microseconds (0 = as fast as the work allows). With it the motor steps
 * on a constant schedule instead of drifting with the renderer, temperature sampling and heating time. */
#ifndef APX_PRINTER_ROW_PERIOD_GRAY_US
#define APX_PRINTER_ROW_PERIOD_GRAY_US 0u
#endif
#ifndef APX_PRINTER_ROW_PERIOD_DOT_US
#define APX_PRINTER_ROW_PERIOD_DOT_US 0u
#endif
static unsigned mode_energy(unsigned mode){return mode==APX_PRINTER_DOT?APX_PRINTER_ENERGY_DOT_PERCENT:APX_PRINTER_ENERGY_GRAY_PERCENT;}
static struct apx_printer_info info={.temperature_limit=65,.energy_percent=APX_PRINTER_ENERGY_GRAY_PERCENT};
__attribute__((weak,noinline)) int apx_printer_photo_request(void) {return APX_PRINTER_UNSUPPORTED;}
__attribute__((weak,noinline)) int apx_printer_photo_cancel(void) {return 0;}
int apx_printer_busy(void) {return info.state==APX_PRINTER_SAMPLING||info.state==APX_PRINTER_PRINTING;}
void apx_printer_get_info(struct apx_printer_info *out) {if(out)*out=info;}
int apx_printer_set_temperature_limit(unsigned index) {
 if(index>65)return APX_PRINTER_INVALID;
 if(apx_printer_busy())return APX_PRINTER_BUSY;
 info.temperature_limit=index;return 0;
}
int apx_printer_set_energy(unsigned percent) {
 if(percent&&(percent<100||percent>400))return APX_PRINTER_INVALID;
 if(apx_printer_busy())return APX_PRINTER_BUSY;
 if(!percent){info.energy_override=0;info.energy_percent=mode_energy(info.mode);return 0;}
 info.energy_override=1;info.energy_percent=percent;return 0;
}
int apx_printer_set_mode(enum apx_printer_mode mode) {
 if(!APX_HAS_PRINTER)return APX_PRINTER_UNSUPPORTED;
 if((unsigned)mode>APX_PRINTER_DOT)return APX_PRINTER_INVALID;
 if(apx_printer_busy())return APX_PRINTER_BUSY;
 info.mode=mode;if(!info.energy_override)info.energy_percent=mode_energy(mode);return 0;
}

static int dither(const uint8_t *gray,unsigned w,unsigned h,unsigned pitch,
 uint8_t *raster,size_t capacity,unsigned ow,unsigned oh,int16_t *err,size_t cells,unsigned clockwise) {
 if(!gray||!raster||!err||!w||!h||pitch<w||!ow||ow>APX_PRINTER_WIDTH||!oh||oh>4096||
    (uint64_t)pitch*h>SIZE_MAX||(uint64_t)((ow+7)/8)*oh>capacity||cells<2*(ow+2))return APX_PRINTER_INVALID;
 unsigned stride=(ow+7)/8;
 memset(raster,0,stride*oh);memset(err,0,2*(ow+2)*sizeof(*err));
 for(unsigned y=0;y<oh;++y) {
  int16_t *current=err+(y&1)*(ow+2),*next=err+((y+1)&1)*(ow+2);
  unsigned source_row=(uint64_t)y*h/oh,source_column=(uint64_t)y*w/oh;
  memset(next,0,(ow+2)*sizeof(*next));
  for(unsigned x=0;x<ow;++x) {
   size_t offset=clockwise?(uint64_t)(h-1-(uint64_t)x*h/ow)*pitch+source_column:
                          (uint64_t)source_row*pitch+(uint64_t)x*w/ow;
   int value=(int)gray[offset]*16+current[x+1];
   if(value<0)value=0;
   if(value>4080)value=4080;
   int black=value<2048,error=value-(black?0:4080);
   if(black)raster[y*stride+x/8]|=0x80u>>(x&7);
   current[x+2]+=error*7/16;next[x]+=error*3/16;
   next[x+1]+=error*5/16;next[x+2]+=error/16;
  }
 }
 return 0;
}
int apx_printer_dither(const uint8_t *gray,unsigned w,unsigned h,unsigned pitch,
 uint8_t *raster,size_t capacity,unsigned ow,unsigned oh,int16_t *err,size_t cells) {
 return dither(gray,w,h,pitch,raster,capacity,ow,oh,err,cells,0);
}
int apx_printer_dither_clockwise(const uint8_t *gray,unsigned w,unsigned h,unsigned pitch,
 uint8_t *raster,size_t capacity,unsigned ow,unsigned oh,int16_t *err,size_t cells) {
 return dither(gray,w,h,pitch,raster,capacity,ow,oh,err,cells,1);
}

#if APX_HAS_PRINTER
/* Stock 02051dd8/dfc: PA9 strobe; 02051e2c/e50: PB5 latch.
 * 02051f2c/f6c: PD13 clock, PD15 serial data; 02051800: PB6 power.
 * 02052060 and table 0208ea8c: PD0, PE10, PA3, PE11 motor phases.
 * 02052118: ADC1 -> 81-entry descending thermistor table.
 * Timing tables and pulses follow 0205c024; no vendor callbacks. */
#include "printer.inc"
static uint8_t upload[48u*1024u+64u] __attribute__((section(".jpeg_buffer"),aligned(32)));
static unsigned upload_width,upload_height,upload_bytes,upload_received,upload_format;
static struct apx_printer_render render __attribute__((section(".jpeg_buffer"),aligned(32)));
static uint8_t gray_planes[32][384] __attribute__((section(".jpeg_buffer"),aligned(32)));
static uint32_t row_period_ticks(void) {
 unsigned us=render.mode==APX_PRINTER_DOT?APX_PRINTER_ROW_PERIOD_DOT_US:APX_PRINTER_ROW_PERIOD_GRAY_US;
 return us?(uint32_t)(((uint64_t)us*32768u+999999u)/1000000u):0;
}
/* Thermistor sampling for photo jobs. The synchronous 20-sample average costs about 4.6 ms in front of every step. With
 * APX_PRINTER_SPREAD_TEMPERATURE only APX_PRINTER_SPREAD_SAMPLES conversions are taken before each half-row (the head has been off
 * for the whole step spacing, so the supply has settled) and a new 20-sample average completes every few half-rows; the
 * temperature interlock and the heat correction use the latest completed average and are evaluated at every half-row.
 * Sampling right after the strobe turns off was tried and read a spurious 962 (hardware 2026-11-07): do not do that. */
#ifndef APX_PRINTER_SPREAD_TEMPERATURE
#define APX_PRINTER_SPREAD_TEMPERATURE 0
#endif
#ifndef APX_PRINTER_SPREAD_SAMPLES
#define APX_PRINTER_SPREAD_SAMPLES 3u
#endif
static unsigned spread_mode,spread_count,sample_goal=20;
static uint32_t spread_sum;

static unsigned gray_heat[32];
static const unsigned groups[]={0,1,3,4},masks[]={0x2c8,0x60,0xa001,0xc00};
static const unsigned actuators[]={0x208,0x40,1,0xc00};
static const unsigned config_regs[]={0x8040,0x8080,0x80c0,0x8100,0x8140,0x81c0,0x8200,0x8380};
static uint32_t saved[4][8],saved_timer[3],saved_interrupt,saved_uart_route;
static uint32_t saved_analog;
static const uint8_t *pixels;
static unsigned image_w,image_h,image_stride,feed_steps,finish_steps,claimed,phase,half,pass,shift,dots,samples,sum,check_only;
static uint32_t next_step,sample_deadline,power_ready,sample_started,step_time,prev_step_time;
static unsigned row_prepared;
static unsigned temperature_discard;
static unsigned timer_calibrating;
static uint32_t timer_started;
static void update(unsigned reg,uint32_t mask,uint32_t bits) {apx_spr_write(reg,(apx_spr_read(reg)&~mask)|(bits&mask));}
static void pin(unsigned bank,unsigned mask,int on) {
 (void)apx_gpio_write(bank,mask,on?mask:0);
 if(bank==0&&mask==0x200)info.head_on=!!on;
 if(bank==1&&mask==0x40)info.power_on=!!on;
}
static void motor(unsigned index) {
 const uint8_t *p=motor_phases[index&7];
 pin(3,1,p[0]);pin(4,0x400,p[1]);pin(0,8,p[2]);pin(4,0x800,p[3]);
 info.motor_on=1;
}
static void off(void) {
 pin(0,0x200,0);pin(1,0x40,0);pin(3,1,0);pin(4,0xc00,0);pin(0,8,0);
 info.motor_on=0;
}
static void release(void) {
 if(!claimed)return;
 off();apx_adc_release(APX_ADC_PRINTER);
 update(0x8868,0x1fff,saved_analog);
 apx_spr_write(0x9830,0);
 apx_spr_write(0x9838,saved_timer[2]);apx_spr_write(0x9834,saved_timer[1]);apx_spr_write(0x9830,saved_timer[0]);
 update(0x4800,8,saved_interrupt);
 update(0x8340,0x3e0,saved_uart_route);
 /* Restoring a saved force-high or peripheral route on an actuator can create
  * a pulse before the final GPIO configure. Keep those pads owned and low. */
 for(unsigned g=0;g<4;++g)for(unsigned r=0;r<8;++r)update(config_regs[r]+4*groups[g],masks[g]&~actuators[g],saved[g][r]);
 /* Never reinstate inherited active heater/motor/power levels. */
 apx_gpio_configure(0,0x208,APX_GPIO_OUTPUT,APX_GPIO_FLOATING,0);
 apx_gpio_configure(1,0x40,APX_GPIO_OUTPUT,APX_GPIO_FLOATING,0);
 apx_gpio_configure(3,1,APX_GPIO_OUTPUT,APX_GPIO_FLOATING,0);
 apx_gpio_configure(4,0xc00,APX_GPIO_OUTPUT,APX_GPIO_FLOATING,0);
 claimed=0;pixels=NULL;finish_steps=0;
}
static int fail(unsigned error) {info.error=error;info.state=APX_PRINTER_ERROR;release();return error;}
int apx_printer_init(void) {
 if(apx_printer_busy())return APX_PRINTER_BUSY;
 struct apx_camera_info camera;apx_camera_get_info(&camera);
 if(camera.state!=APX_CAMERA_OFF)return APX_PRINTER_BUSY;
 if(!claimed) {
  saved_analog=apx_spr_read(0x8868)&0x1fff;
  for(unsigned g=0;g<4;++g)for(unsigned r=0;r<8;++r)saved[g][r]=apx_spr_read(config_regs[r]+4*groups[g]);
  for(unsigned n=0;n<3;++n)saved_timer[n]=apx_spr_read(0x9830+4*n);
  saved_interrupt=apx_spr_read(0x4800)&8;update(0x4800,8,0);
  /* Stock 020526f8 disables UART TX routing before claiming PA7 for paper.
   * Preserve its selector so an inherited UART can be restored at release. */
  saved_uart_route=apx_spr_read(0x8340)&0x3e0;update(0x8340,0x3e0,0);
  for(unsigned g=0;g<4;++g)apx_gpio_configure(groups[g],masks[g]&~(groups[g]==0?0xc0:0),APX_GPIO_OUTPUT,APX_GPIO_FLOATING,groups[g]==1?0x20:0);
  apx_gpio_configure(0,0x80,APX_GPIO_INPUT,APX_GPIO_FLOATING,0);
  /* PA6 is the head thermistor input (ADC channel 1): the SoC's internal pull-up
   * is the divider's bias, set up as stock 020033c0 does (input, pull-up). With
   * it floating ADC1 reads 0 and the thermistor interlock correctly refuses
   * (hardware test 18: 782 with the pull-up, 0 without). */
  apx_gpio_configure(0,0x40,APX_GPIO_INPUT,APX_GPIO_PULL_UP,0);
  apx_adc_acquire(APX_ADC_PRINTER);claimed=1;
 }
 off();
 /* Stock camera boot leaves this analog domain configured for printing.
  * Forced-ROM launches inherit low13=0190 instead. Native GPIO/PHY/serial
  * replay printed blank until low13=182e was applied (2026-11-07 A/B).
  * Own it only with the camera OFF, preserve battery reference bit15 and
  * all unrelated fields, and restore after de-energizing the head/motor.
  * Reference examples keep the old baseline; their analog variant applies
  * this field explicitly so the controlled experiment stays reproducible. */
 update(0x8868,0x1fff,0x182e);
 pin(1,0x40,1);power_ready=apx_ticks()+6554;
 info.state=APX_PRINTER_READY;info.error=0;phase=half=pass=shift=dots=finish_steps=0;
 info.paper_valid=info.temperature_valid=0;
 info.timer_valid=timer_calibrating=0;
 spread_mode=0;sample_goal=20;spread_sum=spread_count=0;
 info.adc_channel=1;
 info.input_format=info.plane=0;
 info.gray_shift_max_counts=info.gray_off_max_counts=0;
 info.period_min=0xffffffffu;info.period_max=info.period_sum=info.period_count=info.sample_max=info.sample_sum=info.work_max=info.work_sum=0;
 memset(info.period_hist,0,sizeof(info.period_hist));info.period_overruns=0;prev_step_time=0;row_prepared=0;
 return 0;
}
static void sample(void) {
 sample_started=apx_ticks();samples=sum=0;temperature_discard=1;sample_deadline=apx_ticks()+32768;info.state=APX_PRINTER_SAMPLING;
}
int apx_printer_check(void) {
 int e=apx_printer_init();if(e)return e;
 pixels=NULL;feed_steps=0;check_only=1;++info.sequence;
 info.row=info.rows=info.steps=0;sample();return 0;
}
int apx_printer_probe_adc(unsigned channel) {
 if(channel>11)return APX_PRINTER_INVALID;
 int e=apx_printer_check();if(e)return e;
 info.adc_channel=channel;check_only=2;return 0;
}
int apx_printer_submit(const uint8_t *raster,size_t capacity,unsigned w,unsigned h,unsigned stride) {
 if(!raster||!w||w>384||!h||h>4096||stride<(w+7)/8||
    (uint64_t)(h-1)*stride+(w+7)/8>capacity)return APX_PRINTER_INVALID;
 int e=apx_printer_init();if(e)return e;
 pixels=raster;image_w=w;image_h=h;image_stride=stride;feed_steps=check_only=0;
 ++info.sequence;info.row=info.steps=0;info.rows=h;sample();return 0;
}
int apx_printer_submit_photo(const uint8_t *raster,size_t capacity,unsigned w,unsigned h,unsigned stride) {
 if(apx_printer_busy())return APX_PRINTER_BUSY;
 int e=apx_printer_submit(raster,capacity,w,h,stride);
 /* Stock gray wrapper 020527f8 passes80; 0205c494..4a0 doubles it to160
  * forward phases at0205c4c4..500. The board adds its calibrated tear-edge
  * margin and buffer; only completed photos take this path. */
 if(!e)finish_steps=APX_PRINTER_PHOTO_TEAR_STEPS;
 return e;
}
static int submit_luma(const uint8_t *gray,size_t capacity,unsigned w,unsigned h,unsigned pitch,
 unsigned ow,unsigned oh,unsigned clockwise,unsigned photo){
 if(apx_printer_busy())return APX_PRINTER_BUSY;
 int e=apx_printer_render_init(&render,gray,capacity,w,h,pitch,ow,oh,clockwise,info.mode,photo);
 if(e)return e;
 e=apx_printer_init();if(e)return e;
 pixels=gray;image_w=ow;image_h=oh;image_stride=pitch;feed_steps=check_only=0;
 finish_steps=photo?APX_PRINTER_PHOTO_TEAR_STEPS:0;
 info.input_format=1;++info.sequence;info.row=info.steps=0;info.rows=oh;sample();return 0;
}
int apx_printer_submit_image(const uint8_t *gray,size_t capacity,unsigned w,unsigned h,unsigned pitch){
 return submit_luma(gray,capacity,w,h,pitch,w,h,0,0);
}
int apx_printer_submit_photo_luma(const uint8_t *gray,size_t capacity,unsigned w,unsigned h,unsigned pitch,unsigned ow,unsigned oh){
 return submit_luma(gray,capacity,w,h,pitch,ow,oh,1,1);
}
int apx_printer_submit_photo_luma_curve(const uint8_t *gray,size_t capacity,unsigned w,unsigned h,unsigned pitch,unsigned ow,unsigned oh,unsigned curve){
 if(curve<1||curve>2)return APX_PRINTER_INVALID;
 return submit_luma(gray,capacity,w,h,pitch,ow,oh,1,curve);
}
int apx_printer_feed(unsigned steps) {
 if(!steps||steps>4096)return APX_PRINTER_INVALID;
 int e=apx_printer_init();if(e)return e;
 ++info.sequence;pixels=NULL;feed_steps=steps;check_only=0;image_h=0;info.row=info.rows=info.steps=0;sample();return 0;
}
static int black(unsigned x,unsigned plane) {
 unsigned left=(384-image_w)/2;
 if(x<left||x-left>=image_w)return 0;
 x-=left;
 if(info.input_format)return render.row[x]<apx_printer_gray_threshold(plane);
 return (pixels[info.row*image_stride+x/8]>>(7-(x&7)))&1;
}
static unsigned plane_bit_at(unsigned x,unsigned plane) {return ((x&1)==(half?0u:1u))&&black(x,plane);}
static unsigned plane_bit(unsigned x) {return plane_bit_at(x,pass);}
static void shift_bit(unsigned on) {
 pin(3,0x8000,on);pin(3,0x2000,0);pin(3,0x2000,1);
}
static int paper_present(void) {
 uint32_t value=0;
 if(apx_gpio_read(0,0x80,&value))return 0;
 info.paper=value!=0;return !value;
}
static int pulse(unsigned count) {
 if(!count)return 0;
 /* Exhaustive bound over the stock tables, valid indices 0..65 and all
  * battery gains/dot counts/passes: maximum 1112 timer counts. */
 if(count>1112)return -1;
 /* Use stock timer 3 configuration and counts. Head cannot stay on across polls. */
 apx_spr_write(0x9834,0);
 uint32_t until=apx_ticks()+64;
 pin(0,0x200,1);
 ++info.heat_pulses;if(count>info.max_heat_count)info.max_heat_count=count;
 while((apx_spr_read(0x9834)&0xffff)<count) {
  if(apx_deadline_expired(until)){pin(0,0x200,0);return -1;}
 }
 pin(0,0x200,0);return 0;
}
/* Native GPIO leaves preserve stock serial speed while the NEXT threshold plane
 * shifts during heating. No heater pulse survives a return to the application. */
static __attribute__((always_inline)) inline void gray_shift(unsigned bit){
 /* Same bus operations as02051f2c/f6c, without a stack frame per bit.
  * The low-to-high clock interval still contains the native read/OR/write;
  * data is established before the rising edge in both cases. */
 uint32_t v=apx_spr_read(0x800c);
 if(bit){apx_spr_write(0x800c,v|0x8000);v=apx_spr_read(0x800c);apx_spr_write(0x800c,v&~0x2000u);}
 else apx_spr_write(0x800c,v&~0xa000u);
 v=apx_spr_read(0x800c);apx_spr_write(0x800c,v|0x2000);
}
static __attribute__((noinline,optimize("O2"))) int gray_pulse(unsigned count,unsigned next_plane){
 if(count>1112)return -1;
 const uint8_t *next=next_plane<32?gray_planes[next_plane]:NULL;
 apx_printer_reference_latch_low();apx_printer_reference_latch_high();
 apx_spr_write(0x9834,0);unsigned start=apx_spr_read(0x9804)&0x3fff,hot=0,off_at=0;
 if(count){apx_printer_reference_strobe_high();info.head_on=hot=1;++info.heat_pulses;
  if(count>info.max_heat_count)info.max_heat_count=count;}
 if(next)for(unsigned x=0;x<384;++x){
  if(hot){
   /* Same64-tick deadline, using the raw reference to avoid runtime calls
    * in this sub-2ms section. Masking handles its14-bit counter wrap. */
   if(((apx_spr_read(0x9804)-start)&0x3fff)>=64)goto timeout;
   if((apx_spr_read(0x9834)&0xffff)>=count){apx_printer_reference_strobe_low();info.head_on=hot=0;off_at=apx_spr_read(0x9834)&0xffff;}
  }
  gray_shift(next[x]);
 }
 unsigned shifted=apx_spr_read(0x9834)&0xffff;
 if(next&&shifted>info.gray_shift_max_counts)info.gray_shift_max_counts=shifted;
 if(count&&!hot&&shifted-off_at>info.gray_off_max_counts)info.gray_off_max_counts=shifted-off_at;
 while(hot){
  if(((apx_spr_read(0x9804)-start)&0x3fff)>=64)goto timeout;
  if((apx_spr_read(0x9834)&0xffff)>=count){apx_printer_reference_strobe_low();info.head_on=hot=0;}
 }
 return 0;
timeout:
 apx_printer_reference_strobe_low();info.head_on=0;return -1;
}
static unsigned heat_count(unsigned dot_count,unsigned plane){
 unsigned index=(dot_count-1)/2,t=info.temperature_index;
 unsigned factor=(dot_cold[index]*(83-t)+dot_hot[index]*t)/83;
 unsigned voltage_index=(info.battery_mv-3400)/25;if(voltage_index>47)voltage_index=47;
 unsigned temperature=plane<2?temperature_initial[t]:temperature_scale[t];
 unsigned weight=(heat_weights[plane]*battery_gain[voltage_index])/100;
 unsigned heat=(((factor*weight)/100)*temperature)/100;
 heat=heat*info.energy_percent/100;return heat>1112?1112:heat;
}
static void prepare_gray(void){
 unsigned left=(384-image_w)/2;
 for(unsigned p=0;p<32;++p){
  unsigned threshold=apx_printer_gray_threshold(p),count=0;
  for(unsigned x=0;x<384;++x){
   unsigned bit=x>=left&&x-left<image_w&&(x&1)==(half?0u:1u)&&render.row[x-left]<threshold;
   gray_planes[p][x]=bit;count+=bit;
  }
  gray_heat[p]=count?heat_count(count,p):0;
 }
}
/* Half-row timing: called when the motor step of a half-row is issued. */
static void record_step(void) {
 uint32_t now=apx_ticks();
 if(pixels){
  uint32_t sampling=now-sample_started;
  if(sampling>info.sample_max)info.sample_max=sampling;
  info.sample_sum+=sampling;
  if(prev_step_time){
   uint32_t period=now-prev_step_time;
   if(period<info.period_min)info.period_min=period;
   if(period>info.period_max)info.period_max=period;
   info.period_sum+=period;++info.period_count;
   if(info.input_format&&row_period_ticks()&&period>row_period_ticks()+16)++info.period_overruns;
   /* Bin edges in ticks: 9, 11, 13, 15, 18, 24 ms. */
   static const uint32_t edge[6]={295,361,426,492,590,786};
   unsigned bin=0;while(bin<6&&period>=edge[bin])++bin;
   ++info.period_hist[bin];
  }
 }
 prev_step_time=now;step_time=now;
}
static void record_work(void) {
 uint32_t work=apx_ticks()-step_time;
 if(work>info.work_max)info.work_max=work;
 info.work_sum+=work;
}
void apx_printer_poll(void) {
 if(!apx_printer_busy())return;
 if(info.state==APX_PRINTER_SAMPLING&&!apx_deadline_expired(power_ready))return;
 uint32_t pins=0;apx_gpio_read(0,0x80,&pins);info.paper=pins!=0;info.paper_valid=1;
 /* A high input inhibits every subsequent heat pulse or motor step, including
  * feed-only jobs. Require explicit submission after replenishing paper. */
 if(info.paper&&!check_only){fail(APX_PRINTER_PAPER);return;}
 if(info.state==APX_PRINTER_SAMPLING) {
  if(!apx_deadline_expired(power_ready))return;
  if(apx_deadline_expired(sample_deadline)){fail(APX_PRINTER_TIMEOUT);return;}
  if(timer_calibrating) {
   unsigned elapsed=apx_ticks()-timer_started;
   if(elapsed<164)return;
   unsigned count=apx_spr_read(0x9834)&0xffff;
   info.timer_hz=(uint64_t)count*32768u/elapsed;
   info.timer_valid=info.timer_hz>=900000u&&info.timer_hz<=1300000u;
   timer_calibrating=0;
   if(!info.timer_valid){fail(APX_PRINTER_IO);return;}
   goto start_work;
  }
  uint16_t value;int r=apx_adc_result(APX_ADC_PRINTER,&value);
  if(r>0){if(temperature_discard)temperature_discard=0;else{sum+=value;++samples;}}
  if(samples<sample_goal){(void)apx_adc_begin(APX_ADC_PRINTER,info.adc_channel);return;}
  if(spread_mode){
   spread_sum+=sum;spread_count+=samples;
   if(spread_count>=20){info.sample_adc=spread_sum/spread_count;spread_sum=0;spread_count=0;}
  } else {info.sample_adc=sum/20;spread_sum=0;spread_count=0;}
  if(check_only==2&&info.adc_channel!=1)goto timer_check;
  info.temperature_adc=info.sample_adc;unsigned t=0;
  while(t<81&&temperature_adc[t]>info.temperature_adc)++t;
  info.temperature_index=t;
  info.temperature_valid=info.temperature_adc<=temperature_adc[0]&&t<=80;
  /* Stock preflight 02052544/02052268 rejects over 65 on its temperature
   * scale. Limit this table's index to 65 as a conservative cutoff. */
  if(check_only!=2&&(!info.temperature_valid||t>info.temperature_limit)){fail(APX_PRINTER_TEMPERATURE);return;}
timer_check:
  if(!info.timer_valid) {
   /* Verify the stock 3809 timer progresses against the independent 32kHz
    * reference, with all heating and motor phases still off. */
   apx_spr_write(0x9830,0x3809);apx_spr_write(0x9838,0xffff);apx_spr_write(0x9834,0);
   timer_started=apx_ticks();timer_calibrating=1;return;
  }
start_work:
  if(info.paper&&check_only!=2){fail(APX_PRINTER_PAPER);return;}
  if(check_only){info.state=APX_PRINTER_DONE;++info.completed;release();return;}
  if(pixels) {
   struct apx_battery_info battery;apx_battery_get_info(&battery);info.battery_mv=battery.millivolts;
   if(!battery.valid||battery.millivolts<3400||battery.millivolts>4600){fail(APX_PRINTER_POWER);return;}
   if(!row_prepared) {
    if(info.input_format&&!half&&apx_printer_render_row(&render,info.row)){fail(APX_PRINTER_IO);return;}
    if(info.input_format&&render.mode==APX_PRINTER_GRAY)prepare_gray();
    row_prepared=1;
   }
   /* Fixed schedule: hold the motor step until the row period since the previous step has passed. */
   if(info.input_format&&prev_step_time&&row_period_ticks()&&!apx_deadline_expired(prev_step_time+row_period_ticks()))return;
  }
  apx_spr_write(0x9830,0x3809);apx_spr_write(0x9838,0xffff);apx_spr_write(0x9834,0);
  pin(1,0x40,1);info.state=APX_PRINTER_PRINTING;
  record_step();row_prepared=0;
  next_step=apx_ticks()+263;motor(phase);phase=(phase+2)&7;++info.steps;return;
 }
 if(!pixels) {
  if(!apx_deadline_expired(next_step))return;
  if(--feed_steps){motor(phase);phase=(phase+2)&7;++info.steps;next_step=apx_ticks()+263;}
  else {info.state=APX_PRINTER_DONE;++info.completed;release();}
  return;
 }
 if(pass==32)goto advance;
 if(shift<384) {
  for(unsigned n=0;n<32&&shift<384;++n,++shift) {
   unsigned on=plane_bit(shift);
   dots+=on;shift_bit(on);
  }
  return;
 }
 /* Stock0205c024 uses32 weighted strobes. Gray shifts the next threshold
  * plane during each bounded pulse; binary Dot/raster input repeats the
  * same plane, so it needs only one shift per half-row. Earlier blank-paper
  * timing experiments preceded discovery of the missing analog setting. */
 if(info.input_format&&render.mode==APX_PRINTER_GRAY){
  apx_battery_defer_conversion();
  for(pass=0;pass<32;++pass){
   if(!paper_present()){fail(APX_PRINTER_PAPER);return;}
   info.plane=pass;
   if(gray_pulse(gray_heat[pass],pass+1)){fail(APX_PRINTER_TIMEOUT);return;}
  }
 } else if(dots) {
  apx_battery_defer_conversion();
  for(pass=0;pass<32;++pass) {
   /* Recheck between synchronous passes, not just between application polls. */
   if(!paper_present()){fail(APX_PRINTER_PAPER);return;}
   info.plane=pass;unsigned heat=heat_count(dots,pass);
   pin(1,0x20,0);pin(1,0x20,1);
   if(pulse(heat)){fail(APX_PRINTER_TIMEOUT);return;}
  }
 }
 record_work();
 pass=32;return; /* one phase per poll: USB is serviced before the next half-row */
advance:
 if(!apx_deadline_expired(next_step))return;
 pass=0;shift=dots=0;
 if(++half==2){half=0;++info.row;}
 if(info.row==image_h){
  if(finish_steps) {
   /* Continue the same forward phase sequence, with the heater already low.
    * Keep one job active so cancellation/paper loss also cover the tear feed. */
   pixels=NULL;feed_steps=finish_steps;finish_steps=0;
   motor(phase);phase=(phase+2)&7;++info.steps;next_step=apx_ticks()+263;return;
  }
  info.state=APX_PRINTER_DONE;++info.completed;release();return;
 }
 /* Temperature is resampled with the head off before every half-row. */
 spread_mode=APX_PRINTER_SPREAD_TEMPERATURE&&info.input_format;
 sample_goal=spread_mode?APX_PRINTER_SPREAD_SAMPLES:20;
 sample();
}
void apx_printer_cancel(void) {if(apx_printer_busy()){info.state=APX_PRINTER_CANCELLED;++info.cancelled;}release();}
void apx_printer_shutdown(void) {apx_printer_cancel();info.state=APX_PRINTER_OFF;}
#else
int apx_printer_init(void){return APX_PRINTER_UNSUPPORTED;}
int apx_printer_check(void){return APX_PRINTER_UNSUPPORTED;}
int apx_printer_probe_adc(unsigned channel){(void)channel;return APX_PRINTER_UNSUPPORTED;}
int apx_printer_submit(const uint8_t *p,size_t n,unsigned w,unsigned h,unsigned s){(void)p;(void)n;(void)w;(void)h;(void)s;return APX_PRINTER_UNSUPPORTED;}
int apx_printer_submit_photo(const uint8_t *p,size_t n,unsigned w,unsigned h,unsigned s){(void)p;(void)n;(void)w;(void)h;(void)s;return APX_PRINTER_UNSUPPORTED;}
int apx_printer_submit_image(const uint8_t *p,size_t n,unsigned w,unsigned h,unsigned s){(void)p;(void)n;(void)w;(void)h;(void)s;return APX_PRINTER_UNSUPPORTED;}
int apx_printer_submit_photo_luma(const uint8_t *p,size_t n,unsigned w,unsigned h,unsigned s,unsigned ow,unsigned oh){(void)p;(void)n;(void)w;(void)h;(void)s;(void)ow;(void)oh;return APX_PRINTER_UNSUPPORTED;}
int apx_printer_submit_photo_luma_curve(const uint8_t *p,size_t n,unsigned w,unsigned h,unsigned s,unsigned ow,unsigned oh,unsigned c){(void)p;(void)n;(void)w;(void)h;(void)s;(void)ow;(void)oh;(void)c;return APX_PRINTER_UNSUPPORTED;}
int apx_printer_feed(unsigned n){(void)n;return APX_PRINTER_UNSUPPORTED;}
void apx_printer_poll(void){}
void apx_printer_cancel(void){}
void apx_printer_shutdown(void){}
#endif

uint32_t apx_printer_command(unsigned command,const uint8_t *in,size_t n,uint8_t *out,size_t *length) {
 *length=0;
 if(command==0&&!n) {
  int size=snprintf((char *)out,APXD_MAX_PAYLOAD,
   "{\"present\":%s,\"state\":%lu,\"error\":%lu,\"row\":%lu,\"rows\":%lu,\"steps\":%lu,\"temperature_adc\":%lu,\"temperature_index\":%lu,\"paper_signal\":%lu,\"sequence\":%lu,\"completed\":%lu,\"battery_mv\":%lu,\"width\":384,\"paper_valid\":%lu,\"temperature_valid\":%lu,\"temperature_limit\":%lu,\"heat_pulses\":%lu,\"max_heat_count\":%lu,\"head_on\":%lu,\"motor_on\":%lu,\"power_on\":%lu,\"cancelled\":%lu,\"timer_hz\":%lu,\"timer_valid\":%lu,\"adc_channel\":%lu,\"sample_adc\":%lu,\"energy_percent\":%lu,\"mode\":%lu,\"mode_name\":\"%s\",\"input_format\":\"%s\",\"plane\":%lu,\"gray_shift_max_counts\":%lu,\"gray_off_max_counts\":%lu,\"energy_override\":%lu,\"period_min\":%lu,\"period_max\":%lu,\"period_sum\":%lu,\"period_count\":%lu,\"sample_max\":%lu,\"sample_sum\":%lu,\"work_max\":%lu,\"work_sum\":%lu,\"period_hist\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu],\"period_overruns\":%lu}",
   APX_HAS_PRINTER?"true":"false",(unsigned long)info.state,(unsigned long)info.error,
   (unsigned long)info.row,(unsigned long)info.rows,(unsigned long)info.steps,
   (unsigned long)info.temperature_adc,(unsigned long)info.temperature_index,(unsigned long)info.paper,(unsigned long)info.sequence,(unsigned long)info.completed,(unsigned long)info.battery_mv,
   (unsigned long)info.paper_valid,(unsigned long)info.temperature_valid,(unsigned long)info.temperature_limit,
   (unsigned long)info.heat_pulses,(unsigned long)info.max_heat_count,(unsigned long)info.head_on,(unsigned long)info.motor_on,(unsigned long)info.power_on,(unsigned long)info.cancelled,(unsigned long)info.timer_hz,(unsigned long)info.timer_valid,(unsigned long)info.adc_channel,(unsigned long)info.sample_adc,(unsigned long)info.energy_percent,
   (unsigned long)info.mode,info.mode==APX_PRINTER_DOT?"dot":"gray",info.input_format?"luma8":"mono1",(unsigned long)info.plane,(unsigned long)info.gray_shift_max_counts,(unsigned long)info.gray_off_max_counts,(unsigned long)info.energy_override,
   (unsigned long)(info.period_count?info.period_min:0),(unsigned long)info.period_max,(unsigned long)info.period_sum,(unsigned long)info.period_count,
   (unsigned long)info.sample_max,(unsigned long)info.sample_sum,(unsigned long)info.work_max,(unsigned long)info.work_sum,
   (unsigned long)info.period_hist[0],(unsigned long)info.period_hist[1],(unsigned long)info.period_hist[2],(unsigned long)info.period_hist[3],
   (unsigned long)info.period_hist[4],(unsigned long)info.period_hist[5],(unsigned long)info.period_hist[6],(unsigned long)info.period_overruns);
  if(size<0||size>=APXD_MAX_PAYLOAD)return APXD_INVALID;
  *length=size;return APXD_OK;
 }
 if(command==1&&!n){if(apx_printer_photo_cancel()&&!apx_printer_busy()){++info.sequence;info.state=APX_PRINTER_CANCELLED;++info.cancelled;}apx_printer_cancel();return 0;}
 if(command==2&&n==4){unsigned steps=(unsigned)in[0]|(unsigned)in[1]<<8|(unsigned)in[2]<<16|(unsigned)in[3]<<24;int e=apx_printer_feed(steps);return e?0x600u+e:0;}
 if(command==6&&!n){int e=apx_printer_photo_request();return e?0x600u+e:0;}
 if(command==7&&!n){int e=apx_printer_check();return e?0x600u+e:0;}
 if(command==8&&!n){apx_printer_shutdown();return 0;}
 if(command==9&&n==4){uint32_t value=0;for(unsigned j=0;j<4;++j)value|=(uint32_t)in[j]<<(8*j);int e=apx_printer_set_temperature_limit(value);return e?0x600u+e:0;}
 if(command==11&&n==4){uint32_t value=0;for(unsigned j=0;j<4;++j)value|=(uint32_t)in[j]<<(8*j);int e=apx_printer_set_energy(value);return e?0x600u+e:0;}
 if(command==10&&n==4){uint32_t value=0;for(unsigned j=0;j<4;++j)value|=(uint32_t)in[j]<<(8*j);int e=apx_printer_probe_adc(value);return e?0x600u+e:0;}
 if(command==13&&n==4){uint32_t value=0;for(unsigned j=0;j<4;++j)value|=(uint32_t)in[j]<<(8*j);int e=apx_printer_set_mode((enum apx_printer_mode)value);return e?0x600u+e:0;}
#if APX_HAS_PRINTER
 if(((command>=3&&command<=5)||(command>=14&&command<=16))&&apx_printer_busy())return 0x600u+APX_PRINTER_BUSY;
 if((command==3||command==14)&&n==8) {
  uint32_t w=0,h=0;for(unsigned j=0;j<4;++j){w|=(uint32_t)in[j]<<(8*j);h|=(uint32_t)in[j+4]<<(8*j);}
  unsigned format=command==14;
  if(!w||w>384||!h||h>(format?4096u:1024u))return APXD_RANGE;
  uint64_t bytes=(uint64_t)(format?w:(w+7)/8)*h;
  if(bytes>sizeof(upload)-64)return APXD_RANGE;
  upload_width=w;upload_height=h;upload_bytes=bytes;upload_received=0;upload_format=format;
  memset(upload,0xa5,sizeof(upload));return 0;
 }
 if((command==4||command==15)&&n>4) {
  uint32_t offset=0;for(unsigned j=0;j<4;++j)offset|=(uint32_t)in[j]<<(8*j);
  if(!upload_bytes||upload_format!=(unsigned)(command==15)||offset!=upload_received||n-4>upload_bytes-offset)return APXD_RANGE;
  memcpy(upload+32+offset,in+4,n-4);upload_received+=n-4;return 0;
 }
 if((command==5||command==16)&&!n) {
  if(!upload_bytes||upload_format!=(unsigned)(command==16)||upload_received!=upload_bytes)return APXD_INVALID;
  for(unsigned j=0;j<32;++j)if(upload[j]!=0xa5||upload[32+upload_bytes+j]!=0xa5)return APXD_IO;
  int e=upload_format?apx_printer_submit_image(upload+32,upload_bytes,upload_width,upload_height,upload_width):
                      apx_printer_submit(upload+32,upload_bytes,upload_width,upload_height,(upload_width+7)/8);
  return e?0x600u+e:0;
 }
#endif
 return APXD_INVALID;
}
