#include <appotech/camera.h>
#include <appotech/trace.h>
#include <appotech/gpio.h>
#include <appotech/soc.h>
#include <appotech/audio.h>
#include <appotech/microphone.h>
#include "board.h"
#include <appotech/printer.h>
static struct apx_camera_info info;
static unsigned initialized,claimed,scaled_running;
static enum apx_camera_id selected;
struct sensor_profile { unsigned id,write_address,read_address,id_register,clock,width,height,control; const uint8_t (*pairs)[2]; unsigned count; };
#if APX_BOARD_SILVERGEAR
#include "sensors.inc"
static uint32_t saved_enable[9];
static unsigned bus_write_address=0x42,bus_read_address=0x43;
#else
#define bus_write_address 0x42u
#define bus_read_address 0x43u
#endif
static uint32_t start,deadline;
static const unsigned pin_regs[]={0x800c,0x804c,0x808c,0x80cc,0x810c,0x814c,0x81cc,0x820c,0x838c};
static uint32_t saved_pins[9],saved_mux,saved_clock,saved_divider,saved_gate,saved_aux,saved_select;
#if !APX_BOARD_SILVERGEAR
static const uint8_t sensor_setup[]={
 0xfd,0x00,0x1d,0x25,0x31,0x07,0x32,0x01,0x30,0x01,0xf0,0xff,0xf1,0xff,0xf2,0xff,
 0xf3,0xff,0xfc,0x50,0xfd,0x01,0x5d,0x01,0x34,0xeb,0x35,0x10,0xfd,0x00,0x03,0x03,
 0x04,0x6c,0x24,0x10,0xef,0x40,0x06,0x00,0x09,0x00,0x0a,0x80,0x10,0x07,0x11,0x04,
 0x16,0x01,0x19,0x22,0x1e,0x58,0x29,0x48,0x13,0x37,0x14,0x01,0x25,0x01,0x2a,0x06,
 0x27,0x01,0x54,0x00,0x55,0x10,0x58,0x38,0x5d,0x12,0x63,0x00,0x64,0x00,0x66,0x28,
 0x68,0x2a,0x72,0x3a,0x73,0x0a,0x75,0x48,0x76,0x0a,0x1f,0x77,0x20,0x07,0xfb,0x16,
 0xfd,0x01,0xf2,0x69,0xf7,0x97,0x02,0x08,0x03,0x01,0x06,0x8a,0x08,0x01,0xfd,0x02,
 0xb8,0x40,0xb9,0xff,0xba,0x30,0xbb,0x35,0xbc,0xf0,0xbd,0x60,0xbe,0xb8,0xbf,0x04,
 0xd0,0xb8,0xd1,0x04,0xfd,0x01,0xc0,0x1f,0xc1,0x18,0xc2,0x15,0xc3,0x13,0xc4,0x13,
 0xc5,0x12,0xc6,0x12,0xc7,0x11,0xc8,0x11,0xc9,0x11,0xca,0x10,0xf3,0x10,0xf4,0x10,
 0xfd,0x01,0x04,0xf0,0x05,0x10,0x0a,0x30,0x0b,0x10,0xfd,0x01,0xcb,0x3c,0xcc,0x3c,
 0xcd,0x03,0xce,0x05,0xfd,0x00,0xfb,0x16,0x35,0xaa,0xfd,0x01,0x1e,0x00,0x20,0x00,
 0x84,0x0f,0x85,0x16,0x86,0x10,0x87,0x1f,0x88,0x08,0x89,0x0a,0x8a,0x07,0x8b,0x18,
 0x8c,0x02,0x8d,0x05,0x8e,0x05,0x8f,0x12,0x90,0x1d,0x91,0x15,0x92,0x20,0x93,0x1d,
 0x94,0x18,0x95,0x15,0x96,0x1d,0x97,0x18,0x98,0x18,0x99,0x15,0x9a,0x20,0x9b,0x1a,
 0xe8,0x20,0xe9,0x08,0xea,0x80,0xbd,0x1e,0xbe,0x00,0xfd,0x01,0xa4,0x00,0x0e,0x80,
 0x18,0x40,0x0f,0x30,0x10,0x98,0x11,0x78,0x12,0x88,0x13,0x88,0x14,0x7f,0x15,0x88,
 0x16,0xf8,0x17,0x98,0x6e,0x00,0x6f,0x07,0x70,0x10,0x71,0x17,0x72,0x21,0x73,0x35,
 0x74,0x46,0x75,0x5a,0x76,0x69,0x77,0x85,0x78,0x96,0x79,0xa7,0x7a,0xb3,0x7b,0xc0,
 0x7c,0xca,0x7d,0xd3,0x7e,0xdb,0x7f,0xe2,0x80,0xe8,0x81,0xed,0x82,0xf2,0x83,0xf8,
 0xfd,0x02,0x09,0x06,0x0d,0x1a,0x1c,0x09,0x1d,0x03,0x1e,0x10,0x1f,0x06,0xfd,0x01,
 0x32,0x00,0xfd,0x02,0x26,0xe2,0x27,0xbb,0x10,0x00,0x11,0x00,0x18,0x17,0x19,0x36,
 0x2a,0x01,0x2b,0x10,0x28,0xf8,0x29,0x08,0x66,0x5f,0x67,0x7f,0x68,0xe0,0x69,0x10,
 0x6a,0xa6,0x7c,0x3a,0x7d,0x75,0x7e,0x10,0x7f,0x2a,0x80,0xaa,0x70,0x10,0x71,0x60,
 0x72,0x3a,0x73,0x56,0x74,0xaa,0x6b,0xff,0x6c,0x50,0x6d,0x50,0x6e,0x70,0x6f,0x6a,
 0x61,0xff,0x62,0x30,0x63,0x70,0x64,0x85,0x65,0x6a,0x75,0x80,0x76,0x09,0x77,0x02,
 0x0e,0x12,0x3b,0x09,0x48,0xea,0x49,0xfc,0x4a,0x05,0x02,0x00,0x03,0x88,0xf5,0xfe,
 0x22,0xfe,0x20,0xfe,0xf7,0xfe,0xfd,0x02,0xde,0x0f,0xcf,0x00,0xd7,0x1a,0xd8,0x2a,
 0xd9,0x30,0xda,0x40,0xdc,0x07,0xe8,0x80,0xe9,0x80,0xea,0x80,0xeb,0xf0,0xec,0x60,
 0xed,0x50,0xee,0x40,0xef,0x30,0xd3,0x30,0xd4,0xc0,0xd5,0x50,0xd6,0x0b,0xf0,0x7f,
 0xfd,0x01,0xb1,0x20,0xfd,0x02,0xdc,0x05,0x05,0x08,0xfd,0x01,0x26,0x33,0x27,0x99,
 0x62,0xf0,0x63,0x80,0x64,0x80,0x65,0x20,0xfd,0x02,0xdd,0xff,0xfd,0x01,0xa8,0x00,
 0xa9,0x09,0xaa,0x09,0xab,0x0c,0xd3,0x00,0xd4,0x09,0xd5,0x09,0xd6,0x0c,0xcf,0xff,
 0xd0,0xf0,0xd1,0x80,0xd2,0x80,0xdf,0xff,0xe0,0xf0,0xe1,0xd0,0xe2,0x80,0xe3,0xff,
 0xe4,0xf0,0xe5,0xd0,0xe6,0x80,0xfd,0x02,0x15,0xe0,0x16,0xa0,0xa0,0x92,0xa1,0xe6,
 0xa2,0x08,0xa3,0xf9,0xa4,0x84,0xa5,0x03,0xa6,0xfd,0xa7,0xfc,0xa8,0x87,0xac,0x82,
 0xad,0xda,0xae,0x24,0xaf,0xe3,0xb0,0x91,0xb1,0x0c,0xb2,0xcd,0xb3,0x1f,0xb4,0x94,
 0xfd,0x01,0xb3,0xa0,0xb4,0xa0,0xb5,0x90,0xb6,0x80,0xb7,0xa0,0xb8,0xa0,0xb9,0x90,
 0xba,0x80,0xfd,0x01,0xbf,0xff,0x00,0x00,0xfd,0x01,0xa4,0x00,0xa5,0x1f,0xa6,0x50,
 0xa7,0x65,0xfd,0x02,0x30,0x38,0x31,0x40,0x32,0x40,0x33,0xff,0x35,0x60,0x36,0x28,
 0x37,0x07,0x38,0x08,0xe6,0x8f,0xfd,0x01,0x1b,0x15,0x1c,0x1a,0x1d,0x0c,0xfd,0x01,
 0x32,0x15,0x33,0xef,0x36,0x10,0xf6,0x90,0xf5,0x10,0xd7,0x3a,0xd8,0x35,0xd9,0x30,
 0xda,0x40,0xdb,0x40,0xdc,0x3a,0xdd,0x30,0xde,0x00,0xe7,0x3a,0x9c,0x77,0x9d,0x55,
 0x9e,0x11,0x9f,0x11,0xfd,0x00,0x1c,0x00,0xfd,0x00,
};
#endif
/* Stock panel resource, SPI 0x96104; camera scaler 0202b4b8. */
static const uint32_t scaler_filter[24]={
#if APX_BOARD_SILVERGEAR
#include "scaler.inc"
#else
 0x2113fefe,0x01fefe13,0x2010fdff,0x01fdff17,0x220cfcff,0x01fc001a,
 0x1f09fc00,0x00fc031d,0x2006fc00,0x00fc061c,0x1d03fc00,0x00fc091f,
 0x1a00fc01,0xfffc0c22,0x17fffd01,0xfffd1020,0xfe112011,0xff141f0e,
 0x00171f0a,0x02191e07,0x051b1b05,0x071e1902,0x0a1f1700,0x0e1f14ff
#endif
};
static void update(unsigned reg,uint32_t mask,uint32_t value) {
 apx_spr_write(reg,(apx_spr_read(reg)&~mask)|(value&mask));
}
static void halt(void) {
 scaled_running=0;
 update(0xb000,1,0);apx_spr_write(0xb018,0);apx_spr_write(0xb014,0);
 for(unsigned reg=0xb040;reg<=0xb064;reg+=4)apx_spr_write(reg,0);
 apx_spr_write(0xb00c,0xffffffffu);
}
static int failure(unsigned error) {
 halt();info.error=error;info.state=APX_CAMERA_ERROR;return error;
}
static int bus_wait(void) {
 uint32_t end=apx_ticks()+328;
 while(!(apx_spr_read(0x90a4)&1)) {
  if(apx_deadline_expired(end)){++info.i2c_errors;return APX_CAMERA_TIMEOUT;}
  apx_maintenance();apx_delay_service();
 }
 apx_spr_write(0x90a0,apx_spr_read(0x90a0)|1);return 0;
}
static int bus_stop(void) {apx_spr_write(0x90a0,0x40);return bus_wait();}
static int bus_send(unsigned byte) {
 apx_spr_write(0x909c,byte);apx_spr_write(0x90a0,apx_spr_read(0x90a0)|0x10);
 int error=bus_wait();if(error)return error;
 if(apx_spr_read(0x90a4)&0x80){++info.i2c_errors;return APX_CAMERA_NACK;}return 0;
}
static int sensor_write(unsigned reg,unsigned value) {
 apx_spr_write(0x90a0,0x80);int error=bus_send(bus_write_address);
 if(!error)error=bus_send(reg);
 if(!error)error=bus_send(value);
 int stop=bus_stop();return error?error:stop;
}
static int sensor_read(unsigned reg,unsigned *value) {
 apx_spr_write(0x90a0,0x80);int error=bus_send(bus_write_address);
 if(!error)error=bus_send(reg);
 int stop=bus_stop();if(error||stop)return error?error:stop;
 apx_spr_write(0x90a0,0x80);error=bus_send(bus_read_address);
 if(!error){apx_spr_write(0x90a0,apx_spr_read(0x90a0)|0x28);error=bus_wait();
  if(!error)*value=apx_spr_read(0x9098)&255;}
 stop=bus_stop();return error?error:stop;
}
/* Register access for the ID 0x39 sensor, which has three register pages selected through register 0xfd; page 0 is left selected afterwards (the setup and the mirror
 * write assume it). Works while a scaled capture runs: the sensor bus is separate from the frame DMA. */
static int sensor_page_access(unsigned page,unsigned reg,unsigned *value,int write) {
 if(!initialized||info.sensor_id!=0x39)return APX_CAMERA_UNSUPPORTED;
 if(page>2||reg>255)return APX_CAMERA_RANGE;
 int error=sensor_write(0xfd,page);
 if(!error)error=write?sensor_write(reg,*value):sensor_read(reg,value);
 if(page){int restore=sensor_write(0xfd,0);if(!error)error=restore;}
 return error;
}
int apx_camera_sensor_read(unsigned page,unsigned reg,unsigned *value) {return value?sensor_page_access(page,reg,value,0):APX_CAMERA_INVALID;}
int apx_camera_sensor_write(unsigned page,unsigned reg,unsigned value) {return sensor_page_access(page,reg,&value,1);}
/* Controls: stored per camera, written by apx_camera_service(). The register map (all page 1 unless noted): 0x02 longest exposure with AE in units of 151 lines (1..8, the frame
 * stretches with the exposure beyond 500 lines), 0x04 gain, 0x12 luma contrast, 0x18 luma offset (higher darker), 0x32 switches (bit 0 AE, bit 4 AWB); page 0 0x03/0x04 exposure
 * lines (written only with AE off; with AE on it is overwritten every frame). */
static struct apx_camera_controls wanted[APX_CAMERA_COUNT],applied[APX_CAMERA_COUNT];
static uint32_t exposure_retry[APX_CAMERA_COUNT]; /* tick at which the manual exposure is written once more after auto exposure was switched off (0 = none) */
static unsigned dirty[APX_CAMERA_COUNT],chosen[APX_CAMERA_COUNT],controls_applying; /* chosen: the application has set controls for this camera */
void apx_camera_controls_default(struct apx_camera_controls *c) {
 c->auto_exposure=1;c->auto_white_balance=1;c->dynamic_frame_rate=1;c->gain=240;c->contrast=136;c->brightness=64;c->blank_lines=0;c->exposure_lines=500;c->frame_interval_us=0;
}
void apx_camera_controls_get(enum apx_camera_id id,struct apx_camera_controls *c) {
 if(!c)return;
 if((unsigned)id>=APX_CAMERA_COUNT||!chosen[id]){apx_camera_controls_default(c);return;}
 *c=wanted[id];
}
int apx_camera_controls_set(enum apx_camera_id id,const struct apx_camera_controls *c) {
 if(!c||(unsigned)id>=APX_CAMERA_COUNT)return APX_CAMERA_INVALID;
 struct apx_camera_controls v=*c;
 if(v.gain<32)v.gain=32;
 if(v.contrast>160)v.contrast=160;
 if(v.exposure_lines<1)v.exposure_lines=1;
 if(v.exposure_lines>2047)v.exposure_lines=2047;
 v.auto_exposure=v.auto_exposure!=0;v.auto_white_balance=v.auto_white_balance!=0;v.dynamic_frame_rate=v.dynamic_frame_rate!=0;
 wanted[id]=v;chosen[id]=dirty[id]=1;return 0;
}
static unsigned exposure_cap_units(const struct apx_camera_controls *c) {
 if(c->dynamic_frame_rate||!c->frame_interval_us)return 8;
 unsigned lines=(unsigned)((uint64_t)c->frame_interval_us*1000u/APX_CAMERA_LINE_NS),n=lines/151u;
 return n<1?1:n>8?8:n;
}
static int write_if_changed(unsigned reg,unsigned have,unsigned want) {return have==want?0:sensor_write(reg,want);}
/* `have` = what the sensor holds (applied), `want` = what is wanted; the page registers are switched once per page. */
static int apply_controls(unsigned cam,int fresh) {
 if(!initialized||info.sensor_id!=0x39||cam!=selected||controls_applying)return 0;
 struct apx_camera_controls want,have;
 apx_camera_controls_get((enum apx_camera_id)cam,&want);
 if(fresh)apx_camera_controls_default(&have);else have=applied[cam];
 controls_applying=1;dirty[cam]=0;
 const unsigned cap_want=exposure_cap_units(&want),cap_have=exposure_cap_units(&have);
 int error=0;
 if(cap_want!=cap_have||want.gain!=have.gain||want.contrast!=have.contrast||want.brightness!=have.brightness||
    want.auto_exposure!=have.auto_exposure||want.auto_white_balance!=have.auto_white_balance) {
  error=sensor_write(0xfd,1);
  if(!error)error=write_if_changed(0x02,cap_have,cap_want);
  if(!error)error=write_if_changed(0x04,have.gain,want.gain);
  if(!error)error=write_if_changed(0x12,have.contrast,want.contrast);
  if(!error)error=write_if_changed(0x18,have.brightness,want.brightness);
  if(!error&&(want.auto_exposure!=have.auto_exposure||want.auto_white_balance!=have.auto_white_balance)) {
   unsigned sw=0;error=sensor_read(0x32,&sw);
   if(!error){sw=(sw&~0x11u)|(want.auto_exposure?1u:0u)|(want.auto_white_balance?0x10u:0u);error=sensor_write(0x32,sw);}
  }
  int restore=sensor_write(0xfd,0);if(!error)error=restore;
 }

 if(!error&&want.blank_lines!=have.blank_lines) {error=sensor_write(0x05,0);if(!error)error=sensor_write(0x06,want.blank_lines);} /* page 0 is selected again here */
 if(!error&&!want.auto_exposure&&(have.auto_exposure||want.exposure_lines!=have.exposure_lines)) {
  error=sensor_write(0x03,want.exposure_lines>>8);
  if(!error)error=sensor_write(0x04,want.exposure_lines&255);
  /* The sensor's last auto-exposure update lands after the switch and overwrites this write (the exposure stays at the long auto value): write it again later. */
  if(!error&&have.auto_exposure)exposure_retry[cam]=(apx_ticks()+3300)|1u; /* ~100 ms */
 }
 if(!error)applied[cam]=want;else dirty[cam]=1; /* try again at the next service */
 controls_applying=0;return error;
}
int apx_camera_service(void) {
 if(!initialized||selected>=APX_CAMERA_COUNT)return 0;
 if(exposure_retry[selected]&&(int32_t)(apx_ticks()-exposure_retry[selected])>=0&&!controls_applying) {
  exposure_retry[selected]=0;
  struct apx_camera_controls want;apx_camera_controls_get((enum apx_camera_id)selected,&want);
  if(!want.auto_exposure&&info.sensor_id==0x39) {
   controls_applying=1;
   int error=sensor_write(0x03,want.exposure_lines>>8);
   if(!error)error=sensor_write(0x04,want.exposure_lines&255);
   controls_applying=0;
   if(error)return error;
  }
 }
 if(!dirty[selected])return 0;
 return apply_controls(selected,0);
}
int apx_camera_init(void) {
 if(APX_HAS_PRINTER&&apx_printer_busy())return APX_CAMERA_BUSY;
 if(initialized)return info.state==APX_CAMERA_ERROR?info.error:0;
 if(!claimed){for(unsigned n=0;n<9;++n)saved_pins[n]=apx_spr_read(pin_regs[n]);
  #if APX_BOARD_SILVERGEAR
  for(unsigned n=0;n<9;++n)saved_enable[n]=apx_spr_read(pin_regs[n]+4);
#endif
  saved_mux=apx_spr_read(0x8348);saved_clock=apx_spr_read(0x8850);
  saved_divider=apx_spr_read(0x884c);saved_select=apx_spr_read(0x8848);
  saved_gate=apx_spr_read(0x8840);saved_aux=apx_spr_read(0x8868);claimed=1;}
 update(0x4800,1u<<14,0);
 /* Clock the engine before reading/stopping its inherited registers. */
 update(0x8840,1u<<27,0);halt();
#if APX_BOARD_SILVERGEAR
 /* 02050c60: active-high power-down on PE10/PE11; alternate is held off. */
 apx_gpio_configure(4,0xc00,APX_GPIO_OUTPUT,APX_GPIO_FLOATING,0xc00);
 apx_delay_ms(50);
 unsigned enable=selected==APX_CAMERA_REAR?APX_CAMERA_REAR_ENABLE_MASK:APX_CAMERA_FRONT_ENABLE_MASK;
 apx_gpio_write(4,enable,0);apx_delay_ms(50);
#else
 /* 02003144: sensor power-down low and a low/high/low reset pulse. */
 apx_gpio_configure(3,3,APX_GPIO_OUTPUT,APX_GPIO_FLOATING,0);
 apx_delay_ms(50);apx_gpio_write(3,1,1);apx_delay_ms(50);apx_gpio_write(3,1,0);
#endif
 /* 0202af0c: 288 MHz / 12 sensor MCLK; preserve every unrelated divider. */
 update(0x8850,1u<<26,0);update(0x8850,1u<<26,1u<<26);
 update(0x8348,0xc00,0x800);update(0x810c,0x400,0x400);update(0x804c,0x400,0);
 update(0x8848,0x20,0);
#if APX_BOARD_SILVERGEAR
 /* Stock 02050d38: 16 MHz during identification, then the matched profile. */
 update(0x884c,0x1f00,0x1100);
#else
 update(0x884c,0x1f00,0xb00);
#endif
 /* 02026f7c: dedicated I2C on PD13/PD15, stock 400 kHz divider. */
 update(0x8348,7,3);update(0x838c,0xa000,0xa000);update(0x810c,0xa000,0xa000);
 update(0x800c,0xa000,0xa000);update(0x804c,0xa000,0x8000);update(0x808c,0xa000,0xa000);
#if APX_BOARD_SILVERGEAR
 /* 02034fa0 -> 0202bde4: 144 MHz/(5*200 kHz)-1. */
 apx_spr_write(0x9090,APX_CPU_HZ_ESTIMATE/(5u*200000u)-1u);
#else
 apx_spr_write(0x9090,71);
#endif
 update(0x9094,0x80,0x80);
 update(0x8868,0x1fff,0x182e);apx_delay_ms(20);
 apx_trace(APX_TRACE_APP_BASE+0x10,0); /* sensor powered and clocked */
#if APX_BOARD_SILVERGEAR
 const struct sensor_profile *profile=NULL;
 unsigned id=0;int error=APX_CAMERA_UNSUPPORTED;
 for(unsigned n=0;n<sizeof(silvergear_sensors)/sizeof(silvergear_sensors[0]);++n) {
  const struct sensor_profile *candidate=&silvergear_sensors[n];
  bus_write_address=candidate->write_address;bus_read_address=candidate->read_address;
  if(candidate->id==0xc0){(void)sensor_write(0xfc,0x16);apx_delay_ms(10);}
  if(candidate->id==0x39)(void)sensor_write(0xfd,0);
  error=sensor_read(candidate->id_register,&id);
  if(!error&&id==candidate->id){profile=candidate;break;}
 }
 info.sensor_id=id;if(!profile)return failure(error?error:APX_CAMERA_UNSUPPORTED);
 update(0x884c,0x1f00,((288000000u/profile->clock)-1u)<<8);
 for(unsigned n=0;n<profile->count;++n) {
  error=sensor_write(profile->pairs[n][0],profile->pairs[n][1]);
  if(error)return failure(error);
  if(!n)apx_delay_ms(10);
 }
 /* Stock 02050ab4 applies board-dependent mirror/flip after the setup table. */
 /* Owner-confirmed physical mapping: stock index 0/PE10 is the selfie
  * camera. Its stock mirror flag is 1; stock index 1/PE11 is rear, flag 0. */
 unsigned stock_flip=selected==APX_CAMERA_FRONT;
 switch(profile->id) {
 case 0xa0:error=sensor_write(0x14,stock_flip?0x13:0x11);break;
 case 0xa6:error=sensor_write(0x4a,stock_flip?12:4);break;
 case 0xc0:break;
 case 0x39:error=sensor_write(0xfd,0);if(!error)error=sensor_write(0x31,stock_flip?2:0);break;
 case 0x46:
  error=sensor_write(0xf0,0x32);
  if(!error)error=sensor_write(0x21,stock_flip?0x66:0x60);
  if(!error)error=sensor_write(0xe0,stock_flip?8:4);
  if(!error)error=sensor_write(0xf0,stock_flip?0x35:0x18);
  break;
 }
 if(error)return failure(error);
#else
 unsigned id=0;int error=sensor_write(0xfd,0);if(!error)error=sensor_read(1,&id);
 apx_trace(APX_TRACE_APP_BASE+0x11,(uint32_t)error); /* sensor probe result */
 info.sensor_id=id;if(error)return failure(error);
 if(id!=0x39)return failure(APX_CAMERA_UNSUPPORTED);
 for(unsigned n=0;n+1<sizeof(sensor_setup);n+=2){
  error=sensor_write(sensor_setup[n],sensor_setup[n+1]);if(error)return failure(error);
  if(!n)apx_delay_ms(10);
 }
#endif
 /* 0202ae88/02035ed8: DVP inputs and byte traversal selected by the sensor resource. */
 update(0x81cc,0xffff,0);update(0x820c,0xffff,0);
 update(0x804c,0x5ffc,0x5bfc);update(0x810c,0x5ffc,0x5ffc);update(0x8840,1u<<27,0);
#if APX_BOARD_SILVERGEAR
 apx_spr_write(0xb000,profile->control);
#else
 apx_spr_write(0xb000,0x286);
#endif
apx_spr_write(0xb004,0);
 apx_spr_write(0xb01c,0x01df027f);apx_spr_write(0xb024,0x02d00500);apx_spr_write(0xb020,0x00640190);
#if APX_BOARD_SILVERGEAR
 info.native_width=profile->width;info.native_height=profile->height;
#else
 info.native_width=640;info.native_height=480;
#endif
 info.width=info.pitch=info.native_width;info.height=info.native_height;
 info.error=0;info.state=APX_CAMERA_READY;initialized=1;
 /* The setup table has just given the sensor its default controls: write back whatever was chosen for this camera. */
 if(info.sensor_id==0x39){int e=apply_controls(selected,1);(void)e;}
 return 0;
}
int apx_camera_buffer_busy(const void *buffer,size_t bytes) {
 if(info.state!=APX_CAMERA_CAPTURING||!bytes)return 0;
 uintptr_t address=(uintptr_t)buffer;
 return address<=info.buffer?bytes>info.buffer-address:address-info.buffer<info.bytes;
}
static int destination(void *frame,size_t capacity,unsigned bytes) {
 uintptr_t address=(uintptr_t)frame;
 if((address&31)||address<APX_RAM_BASE||address>=APX_RAM_END||capacity<bytes||bytes>APX_RAM_END-address)return APX_CAMERA_RANGE;
 if(apx_audio_buffer_busy(frame,bytes)||apx_mic_buffer_busy(frame,bytes))return APX_CAMERA_BUSY;
 return 0;
}
static void begin(void *frame,unsigned bytes,unsigned width,unsigned height,unsigned pitch,unsigned scaled) {
 info.buffer=(uintptr_t)frame;info.bytes=bytes;info.width=width;info.height=height;info.pitch=pitch;info.scaled=scaled;
 info.error=info.status=info.ticks=0;start=apx_ticks();deadline=start+32768;info.state=APX_CAMERA_CAPTURING;
}
int apx_camera_capture_start(void *frame,size_t capacity) {
 if(info.state==APX_CAMERA_CAPTURING)return APX_CAMERA_BUSY;
 int error=destination(frame,capacity,APX_CAMERA_FRAME_BYTES);if(error)return error;
 error=apx_camera_init();if(error)return error;
 halt();apx_dma_clean(frame,APX_CAMERA_FRAME_BYTES);apx_dma_invalidate(frame,APX_CAMERA_FRAME_BYTES);
 apx_spr_write(0xb004,0);
 apx_spr_write(0xb058,480);apx_spr_write(0xb05c,640*480);
 apx_spr_write(0xb050,(uintptr_t)frame);apx_spr_write(0xb054,(uintptr_t)frame+640*480);
 apx_spr_write(0xb00c,0xffffffffu);apx_spr_write(0xb014,0x8006f839);
 apx_spr_write(0xb018,1u<<19);
 begin(frame,APX_CAMERA_FRAME_BYTES,640,480,640,0);update(0xb000,1,1);return 0;
}
int apx_camera_scaled_start(void *frame,size_t capacity,unsigned width,unsigned height,unsigned pitch) {
 if(info.state==APX_CAMERA_CAPTURING)return APX_CAMERA_BUSY;
 if(width<16||width>640||height<16||height>480||(width&1)||(height&1)||
    (pitch&31)||pitch<width||pitch>640)return APX_CAMERA_RANGE;
 unsigned bytes=pitch*height*3/2;
 int error=destination(frame,capacity,bytes);if(error)return error;
 error=apx_camera_init();if(error)return error;
 if(!scaled_running||width!=info.width||height!=info.height||pitch!=info.pitch){
  halt();apx_spr_write(0xb004,0x00420000);
  apx_spr_write(0xb030,640u<<16);apx_spr_write(0xb034,480u<<16);
  apx_spr_write(0xb180,(height<<16)|width);
  apx_spr_write(0xb184,(640u<<16)/(width/2));apx_spr_write(0xb188,(480u<<16)/(height/2));
  for(unsigned n=0;n<24;++n)apx_spr_write(0xb18c+4*n,scaler_filter[n]);
  apx_spr_write(0xb068,pitch);update(0xb008,1u<<19,1u<<19);
  update(0xb000,1,1);scaled_running=1;
 }
 apx_dma_clean(frame,bytes);apx_dma_invalidate(frame,bytes);
 apx_spr_write(0xb060,(uintptr_t)frame);apx_spr_write(0xb064,(uintptr_t)frame+pitch*height);
 apx_spr_write(0xb00c,0xffffffffu);apx_spr_write(0xb014,0x8008005a);
 begin(frame,bytes,width,height,pitch,1);
 apx_spr_write(0xb018,0x00240000);return 0;
}
void apx_camera_poll(void) {
 if(info.state!=APX_CAMERA_CAPTURING)return;
 info.status=apx_spr_read(0xb00c);
 if(info.status&0x000e0970u){failure(APX_CAMERA_HARDWARE);return;}
 /* Frame start is bit 3. Native completion is bit 0; scaled completion is
  * bit 1, and its one-shot DMA enable (b018 bit 18) clears in hardware. */
 if((info.status&(info.scaled?2u:1u))&&(!info.scaled||!(apx_spr_read(0xb018)&(1u<<18)))){
  info.ticks=apx_ticks()-start;
  if(info.scaled){apx_spr_write(0xb014,0);apx_spr_write(0xb060,0);apx_spr_write(0xb064,0);}
  else halt();
  apx_dma_invalidate((void *)(uintptr_t)info.buffer,info.bytes);
  info.state=APX_CAMERA_DONE;++info.completed;return;}
 if(apx_deadline_expired(deadline)){++info.timeouts;failure(APX_CAMERA_TIMEOUT);}
}
void apx_camera_stop(void) {if(!claimed)return;halt();info.state=initialized?APX_CAMERA_READY:APX_CAMERA_OFF;info.error=0;}
void apx_camera_shutdown(void) {
 if(!claimed)return;
 halt();update(0x9094,0x80,0);update(0x8850,1u<<26,0);
 /* Restore only this board's camera pins and clock fields, with DMA disabled. */
 update(0x884c,0x1f00,saved_divider);update(0x8848,0x20,saved_select);update(0x8840,1u<<27,saved_gate);
 update(0x8868,0x1fff,saved_aux);update(0x8348,0xc07,saved_mux);
 for(unsigned n=0;n<9;++n)update(pin_regs[n],APX_BOARD_SILVERGEAR?0xfffcu:0xffffu,saved_pins[n]);
#if APX_BOARD_SILVERGEAR
 for(unsigned n=0;n<9;++n)update(pin_regs[n]+4,0xc00,saved_enable[n]);
#endif
 update(0x8850,1u<<26,saved_clock);initialized=claimed=0;info.state=APX_CAMERA_OFF;
}
unsigned apx_camera_count(void) {return APX_CAMERA_COUNT;}
int apx_camera_select(enum apx_camera_id id) {
 if((unsigned)id>=APX_CAMERA_COUNT)return APX_CAMERA_UNSUPPORTED;
 if(info.state==APX_CAMERA_CAPTURING||(APX_HAS_PRINTER&&apx_printer_busy()))return APX_CAMERA_BUSY;
 apx_camera_shutdown();selected=id;return apx_camera_init();
}
void apx_camera_get_info(struct apx_camera_info *out) {if(out){*out=info;out->selected=selected;out->count=APX_CAMERA_COUNT;}}
