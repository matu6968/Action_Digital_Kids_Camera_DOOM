#include <appotech/microphone_files.h>
#include <appotech/microphone.h>
#include <appotech/audio.h>
#include <appotech/storage.h>
#include <appotech/debug.h>
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/buttons.h>
#include <appotech/display.h>
#include <stdio.h>
#include <string.h>
static uint8_t workspace[65536+64] __attribute__((section(".large_buffer"),aligned(32)));
static uint32_t sequence;
int apx_microphone_files_init(void) {return apx_mic_init();}
#define PCM (workspace+32)
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;++i)p[i]=n>>(i*8);}
static uint32_t failure(unsigned error) {return error?0x400u+error:0;}
static void cooperate(void) {apx_usb_poll();apx_buttons_poll();apx_display_poll();apx_maintenance();}
static int guards(unsigned bytes) {
 apx_dma_invalidate(workspace,32);apx_dma_invalidate(PCM+bytes,32);
 for(unsigned i=0;i<32;++i)if(workspace[i]!=0xa5||PCM[bytes+i]!=0xa5)return 0;
 return 1;
}
static FRESULT write_all(FIL *f,const void *data,unsigned bytes) {
 UINT written=0;FRESULT result=f_write(f,data,bytes,&written);
 return result?result:written==bytes?FR_OK:FR_DISK_ERR;
}
static uint32_t save(const char *path,const struct apx_mic_info *i) {
 FRESULT result=apx_storage_mount();if(result)return 0x100u+result;
 FIL file;result=f_open(&file,path,FA_WRITE|FA_CREATE_NEW);if(result)return 0x100u+result;
 uint8_t header[44]={0};memcpy(header,"RIFF",4);put32(header+4,i->bytes+36);
 memcpy(header+8,"WAVEfmt ",8);put32(header+16,16);header[20]=header[22]=1;
 put32(header+24,i->rate);put32(header+28,i->rate*2);header[32]=2;header[34]=16;
 memcpy(header+36,"data",4);put32(header+40,i->bytes);result=write_all(&file,header,44);
 for(unsigned offset=0;!result&&offset<i->bytes;){unsigned n=i->bytes-offset>4096?4096:i->bytes-offset;
  result=write_all(&file,PCM+offset,n);offset+=n;cooperate();}
 if(!result)result=f_sync(&file);
 FRESULT closed=f_close(&file);if(!result)result=closed;
 return result?0x100u+result:0;
}
uint32_t apx_microphone_command(unsigned command,const uint8_t *in,size_t len,uint8_t *out,size_t *size) {
 *size=0;struct apx_mic_info i;apx_mic_get_info(&i);
 int good=i.state!=APX_MIC_DONE||(i.buffer==(uintptr_t)PCM&&guards(i.bytes));
 if(!command&&!len){
  int n=snprintf((char *)out,APXD_MAX_PAYLOAD,"{\"state\":%lu,\"error\":%lu,\"rate\":%lu,\"bytes\":%lu,"
   "\"status\":%lu,\"ticks\":%lu,\"completed\":%lu,\"timeouts\":%lu,\"sequence\":%lu,"
   "\"buffer\":%lu,\"capacity\":65536,\"guard_ok\":%s,\"format\":\"s16le-mono\",\"rates\":[8000,16000,32000]}",
   (unsigned long)i.state,(unsigned long)i.error,(unsigned long)i.rate,(unsigned long)i.bytes,
   (unsigned long)i.status,(unsigned long)i.ticks,(unsigned long)i.completed,(unsigned long)i.timeouts,
   (unsigned long)sequence,(unsigned long)PCM,good?"true":"false");
  if(n<0||n>=APXD_MAX_PAYLOAD)return APXD_INVALID;
  *size=n;return 0;
 }
 if(command==1&&len==8){
  if(i.state==APX_MIC_CAPTURING||apx_audio_buffer_busy(workspace,sizeof(workspace)))return failure(APX_MIC_BUSY);
  unsigned bytes=get32(in),rate=get32(in+4);
  if(bytes<32||bytes>65536||(bytes&31))return failure(APX_MIC_RANGE);
  if(rate!=8000&&rate!=16000&&rate!=32000)return failure(APX_MIC_UNSUPPORTED);
  memset(workspace,0xa5,bytes+64);
  apx_dma_clean(workspace,bytes+64);apx_dma_invalidate(workspace,bytes+64);
  int error=apx_mic_capture_start((int16_t *)PCM,bytes,rate);if(!error)++sequence;return failure(error);
 }
 if(command==2&&!len){apx_mic_stop();return 0;}
 if(command<3||command>5)return APXD_INVALID;
 if(i.state==APX_MIC_CAPTURING)return failure(APX_MIC_BUSY);
 if(i.state!=APX_MIC_DONE)return failure(APX_MIC_INVALID);
 if(!good)return failure(APX_MIC_HARDWARE);
 if(command==3&&len==8){unsigned offset=get32(in),bytes=get32(in+4);
  if((offset|bytes)&1||bytes>APXD_MAX_PAYLOAD||offset>i.bytes||bytes>i.bytes-offset)return failure(APX_MIC_RANGE);
  memcpy(out,PCM+offset,bytes);*size=bytes;return 0;
 }
 if(command==4&&len>1&&len<=1024&&in[len-1]==0&&!memchr(in,0,len-1))return save((const char *)in,&i);
 if(command==5&&!len)return failure(apx_audio_play((int16_t *)PCM,i.bytes,i.rate));
 return APXD_INVALID;
}
