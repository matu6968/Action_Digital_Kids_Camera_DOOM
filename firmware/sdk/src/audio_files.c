#include <appotech/audio_files.h>
#include <appotech/audio.h>
#include <appotech/storage.h>
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/buttons.h>
#include <appotech/display.h>
#include <appotech/debug.h>
#include <stdio.h>
#include <string.h>
static int16_t pcm[32768] __attribute__((section(".large_buffer"),aligned(32)));
static uint32_t loaded,sequence;
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static unsigned get16(const uint8_t *p) {return p[0]|(unsigned)p[1]<<8;}
static uint32_t failure(unsigned error) {return error?0x300u+error:0;}
static int busy(void) {struct apx_audio_info i;apx_audio_get_info(&i);return i.state==APX_AUDIO_PLAYING||i.state==APX_AUDIO_DRAINING;}
static void cooperate(void) {apx_usb_poll();apx_buttons_poll();apx_display_poll();apx_maintenance();}
int apx_audio_files_init(void) {loaded=sequence=0;return apx_audio_init();}
static uint32_t play(unsigned rate) {
 if(!loaded||(loaded&1)||loaded>sizeof(pcm)-32)return failure(APX_AUDIO_RANGE);
 unsigned padded=(loaded+31)&~31u;padded+=32;memset((uint8_t *)pcm+loaded,0,padded-loaded);
 int error=apx_audio_play(pcm,padded,rate);if(!error)++sequence;return failure(error);
}
uint32_t apx_audio_tone(unsigned frequency,unsigned milliseconds,unsigned rate) {
 if(busy())return failure(APX_AUDIO_BUSY);
 if((rate!=8000&&rate!=16000&&rate!=32000)||frequency<40||frequency>rate/4||!milliseconds||milliseconds>4000)return failure(APX_AUDIO_INVALID);
 unsigned frames=rate*milliseconds/1000;
 if(!frames||frames>32752)return failure(APX_AUDIO_RANGE);
 /* Integer triangle wave, symmetric and faded over 10 ms at either end. */
 uint32_t phase=0,step=(uint32_t)(((uint64_t)frequency<<24)/rate);unsigned fade=rate/100;
 for(unsigned i=0;i<frames;++i){int value=(phase>>8)&0xffff;
  if(value<32768)value=value*2-32768;else value=98303-value*2;
  unsigned scale=i<fade?i:frames-1-i<fade?frames-1-i:fade;
  pcm[i]=(int32_t)value*(int32_t)scale/(int32_t)fade/2;
  phase=(phase+step)&0xffffff;if(!(i&255))cooperate();}
 loaded=frames*2;return play(rate);
}
static FRESULT exact(FIL *file,void *data,UINT n) {UINT got=0;FRESULT r=f_read(file,data,n,&got);return r?r:got==n?FR_OK:FR_DISK_ERR;}
uint32_t apx_audio_wav_file(const char *path) {
 if(busy())return failure(APX_AUDIO_BUSY);
 if(!path||!*path||strlen(path)>1023)return APXD_INVALID;
 FRESULT result=apx_storage_mount();if(result)return 0x100u+result;
 FIL file;result=f_open(&file,path,FA_READ);if(result)return 0x100u+result;
 uint8_t header[18];unsigned rate=0,error=0,fmt=0,count=0;FSIZE_t data_offset=0,offset=12;
 uint32_t bytes=0,total=0;result=exact(&file,header,12);
 if(!result){total=get32(header+4);if(memcmp(header,"RIFF",4)||memcmp(header+8,"WAVE",4)||total<4)error=APX_AUDIO_INVALID;}
 if(!result&&!error&&(FSIZE_t)total+8>f_size(&file))error=APX_AUDIO_INVALID;
 FSIZE_t end=(FSIZE_t)total+8;
 while(!result&&!error&&offset+8<=end&&++count<=128){
  result=f_lseek(&file,offset);if(!result)result=exact(&file,header,8);if(result)break;
  uint32_t length=get32(header+4);FSIZE_t payload=offset+8;
  if(length>end-payload){error=APX_AUDIO_INVALID;break;}
  if(!memcmp(header,"fmt ",4)){
   if(fmt||length<16){error=APX_AUDIO_INVALID;break;}
   result=exact(&file,header,16);if(result)break;rate=get32(header+4);
   if(get16(header)!=1||get16(header+2)!=1||get16(header+14)!=16){error=APX_AUDIO_UNSUPPORTED;break;}
   if((rate!=8000&&rate!=16000&&rate!=32000)||get16(header+12)!=2||get32(header+8)!=rate*2){error=APX_AUDIO_UNSUPPORTED;break;}fmt=1;
  } else if(!memcmp(header,"data",4)){
   if(data_offset||!length||(length&1)){error=APX_AUDIO_INVALID;break;}data_offset=payload;bytes=length;
  }
  offset=payload+length+(length&1);cooperate();
 }
 if(!result&&!error&&(!fmt||!data_offset||offset!=end))error=APX_AUDIO_INVALID;
 if(!result&&!error&&bytes>sizeof(pcm)-32)error=APX_AUDIO_RANGE;
 if(!result&&!error){result=f_lseek(&file,data_offset);loaded=0;
  while(!result&&loaded<bytes){unsigned n=bytes-loaded>4096?4096:bytes-loaded;
   result=exact(&file,(uint8_t *)pcm+loaded,n);if(!result)loaded+=n;cooperate();}}
 FRESULT closed=f_close(&file);if(!result)result=closed;
 if(result)return 0x100u+result;
 if(error)return failure(error);
 return play(rate);
}
static int path_frame(const uint8_t *p,size_t n) {return n&&n<=1024&&p[n-1]==0&&!memchr(p,0,n-1);}
uint32_t apx_audio_command(unsigned command,const uint8_t *in,size_t len,uint8_t *out,size_t *size) {
 *size=0;
 if(!command&&!len){struct apx_audio_info i;apx_audio_get_info(&i);
  int n=snprintf((char *)out,APXD_MAX_PAYLOAD,"{\"state\":%lu,\"error\":%lu,\"rate\":%lu,\"volume\":%lu,\"muted\":%lu,"
   "\"bytes\":%lu,\"status\":%lu,\"remaining\":%lu,\"ticks\":%lu,\"completed\":%lu,\"timeouts\":%lu,"
   "\"sequence\":%lu,\"loaded_bytes\":%lu,\"buffer\":%lu,\"capacity\":65536,\"format\":\"s16le-mono\",\"rates\":[8000,16000,32000]}",
   (unsigned long)i.state,(unsigned long)i.error,(unsigned long)i.rate,(unsigned long)i.volume,(unsigned long)i.muted,
   (unsigned long)i.bytes,(unsigned long)i.status,(unsigned long)i.remaining,(unsigned long)i.ticks,(unsigned long)i.completed,
   (unsigned long)i.timeouts,(unsigned long)sequence,(unsigned long)loaded,(unsigned long)pcm);
  if(n<0||n>=APXD_MAX_PAYLOAD)return APXD_INVALID;
  *size=n;return 0;
 }
 if(command==1&&len==12)return apx_audio_tone(get32(in),get32(in+4),get32(in+8));
 if(command==2&&path_frame(in,len))return apx_audio_wav_file((const char *)in);
 if(command==3&&!len){apx_audio_stop();return 0;}
 if(command==4&&len==4)return failure(apx_audio_volume(get32(in)));
 if(command==5&&len==4){unsigned muted=get32(in);return muted>1?APXD_INVALID:failure(apx_audio_mute(muted));}
 if(command==6&&len>=6&&!(len&1)){
  if(busy())return failure(APX_AUDIO_BUSY);
  unsigned offset=get32(in),bytes=len-4;
  if((offset&&offset!=loaded)||offset>sizeof(pcm)-32||bytes>sizeof(pcm)-32-offset)return failure(APX_AUDIO_RANGE);
  if(!offset)loaded=0;
  memcpy((uint8_t *)pcm+offset,in+4,bytes);loaded=offset+bytes;return 0;
 }
 if(command==7&&len==4){if(busy())return failure(APX_AUDIO_BUSY);return play(get32(in));}
 return APXD_INVALID;
}
