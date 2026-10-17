#include <appotech/spi.h>
#include <appotech/soc.h>
#include <appotech/debug.h>
#include <stdio.h>
#include <string.h>
static struct apx_spi_info info;
static unsigned busy;
static uint8_t bounce[4096+64] __attribute__((section(".large_buffer"),aligned(32)));
#define DATA (bounce+32)
static const uint16_t saved_regs[]={0x8850,0x8844,0x8840,0x8344,0x8100,0x8040,0x8000};
static const uint32_t masks[]={0xf0000000,8,1u<<20,3u<<20,0xd00,0xd00,0x400};
static uint32_t saved[7];
static void update(unsigned r,uint32_t mask,uint32_t v){apx_spr_write(r,(apx_spr_read(r)&~mask)|(v&mask));}
static void cs(unsigned high){update(0x9000,1u<<14,high?1u<<14:0);}
static int wait_flag(unsigned mask,uint32_t ticks){uint32_t deadline=apx_ticks()+ticks;
 while(!(apx_spr_read(0x9248)&mask)){if(apx_deadline_expired(deadline)){++info.timeouts;return APX_SPI_TIMEOUT;}apx_maintenance();}
 return 0;
}
static int byte(unsigned value,unsigned receive,uint8_t *result){
 update(0x9000,0x80,receive?0x80:0);apx_spr_write(0x9250,value);apx_spr_write(0x9244,1);
 int e=wait_flag(1,164);if(!e&&result)*result=apx_spr_read(0x9250)&255;
 return e;
}
static int address_command(unsigned command,uint32_t address){int e=byte(command,0,NULL);
 for(int shift=16;!e&&shift>=0;shift-=8)e=byte((address>>shift)&255,0,NULL);
 return e;
}
static void dma_stop(void){update(0x9000,2,0);update(0x900c,~0xfff3ff00u,0);apx_spr_write(0x9258,0);}
static int dma(size_t length,unsigned receive){
 memset(bounce,0xa5,32);memset(DATA+4096,0xa5,32);apx_dma_clean(bounce,sizeof(bounce));
 if(receive)apx_dma_invalidate(DATA,length);
 update(0x9000,2,2);update(0x900c,0x85,receive?0x85:5);
 apx_spr_write(0x9254,length/4-1);apx_spr_write(0x9258,(uintptr_t)DATA);apx_spr_write(0x925c,0x06000000);
 apx_spr_write(0x9244,0x20000);apx_spr_write(0x9244,2);
 int e=wait_flag(2,3277);dma_stop();
 if(receive)apx_dma_invalidate(DATA,length);
 apx_dma_invalidate(bounce,32);apx_dma_invalidate(DATA+4096,32);
 for(unsigned i=0;i<32;++i)if(bounce[i]!=0xa5||DATA[4096+i]!=0xa5)e=APX_SPI_HARDWARE;
 if(!e)++info.dma_transfers;
 return e;
}
static int status(uint8_t *value){cs(0);int e=byte(5,0,NULL);if(!e)e=byte(255,1,value);cs(1);if(!e)info.status=*value;
 return e;}
int apx_spi_init(void){
 if(info.initialized)return 0;
 memset(&info,0,sizeof(info));busy=1;
 for(unsigned i=0;i<7;++i)saved[i]=apx_spr_read(saved_regs[i]);
 update(0x8850,0xf0000000,0x30000000);update(0x8844,8,0);update(0x8840,1u<<20,0);
 update(0x8344,3u<<20,1u<<20);update(0x8100,0xd00,0xd00);update(0x8040,0xd00,0);update(0x8000,0x400,0x400);
 apx_spr_write(0x9000,0x01115005);apx_spr_write(0x9004,0);apx_spr_write(0x9008,0);apx_spr_write(0x900c,0);
 apx_spr_write(0x9244,0x03ff0000);apx_spr_write(0x9248,0);apx_spr_write(0x924c,5);
 for(unsigned r=0x9250;r<=0x927c;r+=4)apx_spr_write(r,0);
 apx_spr_write(0x9260,0xffff);apx_spr_write(0x9264,0xffffffff);
 apx_spr_write(0x926c,0x0fffffff);apx_spr_write(0x9270,0x0fffffff);
 info.initialized=1;cs(0);int e=byte(0x9f,0,NULL);
 for(unsigned n=0;!e&&n<3;++n){uint8_t value;e=byte(255,1,&value);if(!e)info.jedec=info.jedec<<8|value;}
 cs(1);if(!e&&info.jedec!=0x5e4015)e=APX_SPI_HARDWARE;
 if(!e)e=status(&info.status);
 busy=0;if(e)apx_spi_shutdown();
 return e;
}
static int enter(void){if(!info.initialized)return APX_SPI_HARDWARE;if(busy)return APX_SPI_BUSY;busy=1;return 0;}
int apx_spi_status(uint8_t *value){if(!value)return APX_SPI_INVALID;int e=enter();if(e)return e;e=status(value);busy=0;
 return e;}
int apx_spi_read(uint32_t address,void *buffer,size_t length){
 if((!buffer&&length)||address>APX_SPI_CAPACITY||length>APX_SPI_CAPACITY-address)return APX_SPI_INVALID;
 int e=enter();if(e)return e;uint8_t *out=buffer;
 while(length&&!e){size_t n=length>4096?4096:length;cs(0);e=address_command(3,address);
  size_t bulk=n&~31u;if(!e&&bulk){e=dma(bulk,1);if(!e)memcpy(out,DATA,bulk);}
  for(size_t i=bulk;!e&&i<n;++i)e=byte(255,1,out+i);
  cs(1);if(!e){out+=n;address+=n;length-=n;info.bytes_read+=n;}}
 busy=0;
 return e;
}
static int ready(uint32_t ticks){uint32_t deadline=apx_ticks()+ticks;uint8_t value;int e;
 do{e=status(&value);if(e||!(value&1))return e;if(apx_deadline_expired(deadline)){++info.timeouts;return APX_SPI_TIMEOUT;}apx_maintenance();}while(1);
}
static int write_enable(void){int e=ready(32768);if(e)return e;cs(0);e=byte(6,0,NULL);cs(1);uint8_t value;
 if(!e)e=status(&value);
 if(!e&&!(value&2))e=APX_SPI_HARDWARE;
 return e;
}
int apx_spi_program(uint32_t address,const void *buffer,size_t length){
 if(!buffer||!length||address>=APX_SPI_CAPACITY||length>APX_SPI_PAGE-(address%APX_SPI_PAGE)||length>APX_SPI_CAPACITY-address)return APX_SPI_INVALID;
 int e=enter();if(e)return e;e=write_enable();if(!e){cs(0);e=address_command(2,address);
  const uint8_t *p=buffer;for(size_t i=0;!e&&i<length;++i)e=byte(p[i],0,NULL);cs(1);if(!e)e=ready(32768);}
 busy=0;
 return e;
}
int apx_spi_erase_sector(uint32_t address){
 if(address>=APX_SPI_CAPACITY||address%APX_SPI_SECTOR)return APX_SPI_INVALID;
 int e=enter();if(e)return e;e=write_enable();if(!e){cs(0);e=address_command(0x20,address);cs(1);if(!e)e=ready(5u*32768);}
 busy=0;
 return e;
}
void apx_spi_get_info(struct apx_spi_info *out){if(out)*out=info;}
void apx_spi_shutdown(void){if(!info.initialized)return;dma_stop();cs(1);apx_spr_write(0x9000,apx_spr_read(0x9000)&~1u);
 for(unsigned i=0;i<7;++i)update(saved_regs[i],masks[i],saved[i]);
 info.initialized=busy=0;}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
uint32_t apx_spi_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *size){
 *size=0;if(!info.initialized)return APXD_UNSUPPORTED;
 if(!command&&!length){uint8_t value;int e=apx_spi_status(&value);if(e)return APXD_IO;
  int n=snprintf((char *)output,APXD_MAX_PAYLOAD,"{\"jedec\":%lu,\"capacity\":2097152,\"page_bytes\":256,\"sector_bytes\":4096,\"status\":%u,\"bytes_read\":%lu,\"dma_transfers\":%lu,\"timeouts\":%lu,\"host_read_only\":false,\"write_protocol\":1}",
   (unsigned long)info.jedec,value,(unsigned long)info.bytes_read,(unsigned long)info.dma_transfers,(unsigned long)info.timeouts);
  if(n<0||n>=APXD_MAX_PAYLOAD)return APXD_INVALID;
 *size=n;return 0;}
 if(command==1&&length==8){uint32_t address=get32(input),bytes=get32(input+4);
  if(bytes>APXD_MAX_PAYLOAD||address>APX_SPI_CAPACITY||bytes>APX_SPI_CAPACITY-address)return APXD_RANGE;
  if(apx_spi_read(address,output,bytes))return APXD_IO;
 *size=bytes;return 0;}
 /* A separate marker on every destructive frame makes intent explicit. */
 if((command==2||command==3)&&length>=8&&!memcmp(input,"APXF",4)){
  uint32_t address=get32(input+4);int e;
  if(command==2){
   if(length==8||length>8+APX_SPI_PAGE||address>=APX_SPI_CAPACITY||length-8>APX_SPI_PAGE-address%APX_SPI_PAGE)return APXD_RANGE;
   e=apx_spi_program(address,input+8,length-8);
  }else{
   if(length!=8||address>=APX_SPI_CAPACITY||address%APX_SPI_SECTOR)return APXD_RANGE;
   e=apx_spi_erase_sector(address);
  }
  return e?APXD_IO:APXD_OK;
 }
 return APXD_INVALID;
}
