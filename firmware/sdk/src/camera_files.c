#include <appotech/camera_files.h>
#include <appotech/camera.h>
#include <appotech/jpeg.h>
#include <appotech/display.h>
#include <appotech/storage.h>
#include <appotech/debug.h>
#include <appotech/usb.h>
#include <appotech/buttons.h>
#include <appotech/soc.h>
#include <stdio.h>
#include <string.h>
static uint8_t frame[APX_CAMERA_FRAME_BYTES+64] __attribute__((section(".jpeg_buffer"),aligned(32)));
static uint8_t stream[524288+64] __attribute__((section(".jpeg_buffer"),aligned(32)));
#define PREVIEW_BYTES (224u*176u*3u/2u)
static uint8_t preview[2][PREVIEW_BYTES+64] __attribute__((section(".large_buffer"),aligned(32)));
#define FRAME (frame+32)
#define STREAM (stream+32)
static uint16_t xmap[320],ymap[240];
static unsigned map_width,map_height;
static unsigned preview_on,displaying,pending,capturing,job,error,photo_width,photo_height,photo_selector,test_pattern;
static uint32_t next_pattern;
static uint32_t frames,sequence,jpeg_bytes,preview_buffer;
__attribute__((weak,noinline)) void apx_camera_preview_overlay(uint8_t *y,uint8_t *vu,unsigned pitch){(void)y;(void)vu;(void)pitch;}
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void cooperate(void) {apx_usb_poll();apx_buttons_poll();apx_display_poll();apx_maintenance();}
static uint32_t failure(unsigned e) {return e?0x500u+e:0;}
static int guards(uint8_t *buffer,unsigned bytes) {
 apx_dma_invalidate(buffer,32);apx_dma_invalidate(buffer+32+bytes,32);
 for(unsigned n=0;n<32;++n)if(buffer[n]!=0xa5||buffer[32+bytes+n]!=0xa5)return 0;
 return 1;
}
static int start_frame(void) {
 memset(frame,0xa5,32);memset(FRAME+APX_CAMERA_FRAME_BYTES,0xa5,32);
 apx_dma_clean(frame,sizeof(frame));apx_dma_invalidate(frame,sizeof(frame));
 return apx_camera_capture_start(FRAME,APX_CAMERA_FRAME_BYTES);
}
/* SMPTE-style bars in NV21 (BT.601 limited range): white, yellow, cyan, green,
 * magenta, red, blue, black. Luma, then interleaved V,U for each 2x2 block. */
static void draw_test_pattern(uint8_t *y) {
 static const uint8_t luma[8]={235,210,170,145,106,81,41,16};
 static const uint8_t cb[8]={128,16,166,54,202,90,240,128},cr[8]={128,146,16,34,222,240,110,128};
 uint8_t *vu=y+224*176;
 for(unsigned x=0;x<224;++x){unsigned bar=x*8/224;
  for(unsigned row=0;row<176;++row)y[row*224+x]=luma[bar];
  if(!(x&1))for(unsigned row=0;row<88;++row){vu[row*224+x]=cr[bar];vu[row*224+x+1]=cb[bar];}
 }
}
int apx_camera_files_init(void) {
 int e=apx_camera_init();
 /* No answering sensor (disconnected flex, or an unknown part): like the stock
  * firmware, keep the preview alive with a test pattern instead of failing. */
 test_pattern=e==APX_CAMERA_NACK||e==APX_CAMERA_UNSUPPORTED;
 if(e&&!test_pattern){job=5;error=e;return e;}
 memset(frame,0xa5,32);memset(FRAME+APX_CAMERA_FRAME_BYTES,0xa5,32);
 for(unsigned n=0;n<2;++n){memset(preview[n],0xa5,32);memset(preview[n]+32+PREVIEW_BYTES,0xa5,32);
  apx_dma_clean(preview[n],32);apx_dma_clean(preview[n]+32+PREVIEW_BYTES,32);}
 apx_dma_clean(frame,32);apx_dma_clean(FRAME+APX_CAMERA_FRAME_BYTES,32);
 preview_on=1;job=1;error=0;next_pattern=0;
 return e; /* the sensor error stays visible to the caller; the service is running */
}
static void scale_frame(uint8_t *output,unsigned width,unsigned height,unsigned pitch) {
 if(map_width!=width||map_height!=height){
  for(unsigned x=0;x<width;++x)xmap[x]=x*640u/width;
  for(unsigned y=0;y<height;++y)ymap[y]=y*480u/height;
  map_width=width;map_height=height;
 }
 /* Small-photo output may share FRAME: every luma store precedes its input,
  * and the reduced chroma destination stays below the original chroma plane. */
 for(unsigned y=0;y<height;++y){
  const uint8_t *source=FRAME+640u*ymap[y];uint8_t *dest=output+pitch*y;
  for(unsigned x=0;x<width;++x)dest[x]=source[xmap[x]];
  if(!(y&7))cooperate();
 }
 for(unsigned y=0;y<height/2;++y){
  const uint8_t *source=FRAME+640u*480u+640u*(ymap[y*2]/2);uint8_t *dest=output+pitch*height+pitch*y;
  for(unsigned x=0;x<width;x+=2){unsigned offset=xmap[x]&~1u;dest[x]=source[offset];dest[x+1]=source[offset+1];}
  if(!(y&7))cooperate();
 }
}
void apx_camera_files_poll(void) {
 apx_camera_poll();apx_jpeg_poll();
 if(displaying&&apx_display_status()!=APX_DISPLAY_BUSY)displaying=0;
 if(job==5)return;
 if(test_pattern){
  /* Redraw before the overlay each time: the hook paints on the frame it is given. */
  if(job==1&&preview_on&&!displaying&&apx_display_status()==APX_DISPLAY_READY&&(int32_t)(apx_ticks()-next_pattern)>=0){
   uint8_t *pixels=preview[0]+32;
   draw_test_pattern(pixels);
   apx_camera_preview_overlay(pixels,pixels+224*176,224);
   apx_dma_clean(pixels,PREVIEW_BYTES);
   if(apx_display_blit_nv21(0,0,220,176,pixels,pixels+224*176,224)){error=APX_CAMERA_HARDWARE;job=5;return;}
   displaying=1;preview_buffer=(uintptr_t)pixels;++frames;next_pattern=apx_ticks()+3277; /* ~10 Hz */
  }
  return;
 }
 if(job==1&&apx_display_status()==APX_DISPLAY_ERROR){error=APX_CAMERA_HARDWARE;job=5;apx_camera_stop();return;}
 if(job==3){struct apx_jpeg_info jpeg;apx_jpeg_get_info(&jpeg);
  if(jpeg.state==APX_JPEG_ERROR){error=32+jpeg.error;job=5;}
  if(jpeg.state==APX_JPEG_DONE){if(!guards(stream,524288)){error=APX_CAMERA_HARDWARE;job=5;return;}
   jpeg_bytes=jpeg.bytes;job=4;++sequence;}
  return;
 }
 struct apx_camera_info i;apx_camera_get_info(&i);
 if(i.state==APX_CAMERA_ERROR){error=i.error;job=5;return;}
 if(i.state==APX_CAMERA_DONE){
  if(job!=1&&!guards(frame,APX_CAMERA_FRAME_BYTES)){error=APX_CAMERA_HARDWARE;job=5;apx_camera_stop();return;}
  if(job==2){
   unsigned width=640,height=480,pitch=640;
   if(photo_width<640){width=photo_width;height=photo_height;pitch=(width+31)&~31u;scale_frame(FRAME,width,height,pitch);}
   memset(stream,0xa5,sizeof(stream));apx_dma_clean(stream,sizeof(stream));
   int e=apx_jpeg_encode_start(FRAME,FRAME+pitch*height,width,height,pitch,photo_width,photo_height,photo_selector,STREAM,524288);
   apx_camera_stop();if(e){error=32+e;job=5;}else job=3;return;}
  if(job==6){job=7;return;}
  if(job==1&&capturing){
   if(!guards(preview[capturing-1],PREVIEW_BYTES)){error=APX_CAMERA_HARDWARE;job=5;apx_camera_stop();return;}
   pending=capturing;capturing=0;
  }
 }
 if(preview_on&&job==1){
  if(pending&&!displaying&&apx_display_status()==APX_DISPLAY_READY){
   uint8_t *pixels=preview[pending-1]+32;
   apx_camera_preview_overlay(pixels,pixels+224*176,224);
   int e=apx_display_blit_nv21(0,0,220,176,pixels,pixels+224*176,224);
   if(e){error=APX_CAMERA_HARDWARE;job=5;apx_camera_stop();return;}
   displaying=pending;pending=0;preview_buffer=(uintptr_t)pixels;++frames;
  }
  if(!capturing&&!pending){
   unsigned slot=displaying==1?1:0;
   int e=apx_camera_scaled_start(preview[slot]+32,PREVIEW_BYTES,220,176,224);
   if(e){error=e;job=5;}else capturing=slot+1;
  }
 }
}
static uint32_t save(const char *path) {
 FRESULT result=apx_storage_mount();if(result)return 0x100u+result;
 FIL file;result=f_open(&file,path,FA_WRITE|FA_CREATE_NEW);if(result)return 0x100u+result;
 for(unsigned offset=0;!result&&offset<jpeg_bytes;){UINT written=0;unsigned count=jpeg_bytes-offset>4096?4096:jpeg_bytes-offset;
  result=f_write(&file,STREAM+offset,count,&written);if(!result&&written!=count)result=FR_DISK_ERR;
  offset+=count;cooperate();}
 if(!result)result=f_sync(&file);
 FRESULT closed=f_close(&file);if(!result)result=closed;
 return result?0x100u+result:0;
}
uint32_t apx_camera_command(unsigned command,const uint8_t *in,size_t len,uint8_t *out,size_t *size) {
 *size=0;struct apx_camera_info i;apx_camera_get_info(&i);
 if(!command&&!len){int n=snprintf((char *)out,APXD_MAX_PAYLOAD,
  "{\"state\":%lu,\"error\":%u,\"sensor_id\":%lu,\"width\":640,\"height\":480,\"pitch\":640,"
  "\"format\":\"nv21\",\"job\":%u,\"preview\":%s,\"frames\":%lu,\"sequence\":%lu,\"jpeg_bytes\":%lu,"
  "\"buffer\":%lu,\"bytes\":460800,\"status\":%lu,\"ticks\":%lu,\"completed\":%lu,\"timeouts\":%lu,\"i2c_errors\":%lu,"
  "\"guard_ok\":%s,\"control\":%lu,\"pipeline\":%lu,\"dma\":%lu,\"photo_width\":%u,\"photo_height\":%u,"
  "\"preview_dma\":true,\"preview_width\":220,\"preview_height\":176,\"preview_pitch\":224,"
  "\"preview_bytes\":%u,\"preview_buffer\":%lu,\"preview_buffers\":[%lu,%lu],\"preview_guard_ok\":%s,"
  "\"capture_scaled\":%lu,\"capture_buffer\":%lu,\"capture_width\":%lu,\"capture_height\":%lu,\"capture_pitch\":%lu}",
  (unsigned long)i.state,error?error:(unsigned)i.error,(unsigned long)i.sensor_id,job,preview_on?"true":"false",
  (unsigned long)frames,(unsigned long)sequence,(unsigned long)jpeg_bytes,(unsigned long)FRAME,(unsigned long)i.status,
  (unsigned long)i.ticks,(unsigned long)i.completed,(unsigned long)i.timeouts,(unsigned long)i.i2c_errors,
  i.state==APX_CAMERA_CAPTURING||!i.completed||guards(frame,APX_CAMERA_FRAME_BYTES)?"true":"false",
  (unsigned long)apx_spr_read(0xb000),(unsigned long)apx_spr_read(0xb004),(unsigned long)apx_spr_read(0xb018),photo_width,photo_height,
  PREVIEW_BYTES,(unsigned long)preview_buffer,(unsigned long)(preview[0]+32),(unsigned long)(preview[1]+32),
  guards(preview[0],PREVIEW_BYTES)&&guards(preview[1],PREVIEW_BYTES)?"true":"false",
  (unsigned long)i.scaled,(unsigned long)i.buffer,(unsigned long)i.width,(unsigned long)i.height,(unsigned long)i.pitch);
  if(n<0||n>=APXD_MAX_PAYLOAD)return APXD_INVALID;
  *size=n;return 0;
 }
 if(command==5&&!len){
  apx_camera_stop();capturing=pending=0;if(job==3)apx_jpeg_shutdown();preview_on=0;job=0;error=jpeg_bytes=0;return 0;
 }
 if(command==1&&len==4&&get32(in)<=1){
  if(job==2||job==3||job==6)return failure(APX_CAMERA_BUSY);
  apx_camera_stop();capturing=pending=0;preview_on=get32(in);job=preview_on?1:0;error=0;return 0;
 }
 if(command==2&&len==12){
  unsigned w=get32(in),h=get32(in+4),selector=get32(in+8);
  if(job==2||job==3||job==6)return failure(APX_CAMERA_BUSY);
  static const uint16_t sizes[][2]={{220,176},{320,240},{640,480},{1280,720},{1280,960},{1600,1200},{1920,1080},{2048,1536}};
  unsigned valid=0;for(unsigned n=0;n<sizeof(sizes)/sizeof(sizes[0]);++n)if(w==sizes[n][0]&&h==sizes[n][1])valid=1;
  if(!valid||selector>15)return failure(APX_CAMERA_RANGE);
  apx_camera_stop();capturing=pending=0;preview_on=0;photo_width=w;photo_height=h;photo_selector=selector;jpeg_bytes=0;error=0;
  int e=start_frame();if(e){job=5;error=e;return failure(e);}job=2;return 0;
 }
 if(command==6&&!len){
  if(job==2||job==3||job==6)return failure(APX_CAMERA_BUSY);
  apx_camera_stop();capturing=pending=0;preview_on=0;jpeg_bytes=0;error=0;int e=start_frame();job=e?5:6;error=e;return failure(e);
 }
 if(command==3&&len==8){
  unsigned offset=get32(in),bytes=get32(in+4);
  if(job!=4)return failure(APX_CAMERA_BUSY);
  if(!guards(stream,524288))return failure(APX_CAMERA_HARDWARE);
  if(bytes>APXD_MAX_PAYLOAD||offset>jpeg_bytes||bytes>jpeg_bytes-offset)return failure(APX_CAMERA_RANGE);
  memcpy(out,STREAM+offset,bytes);*size=bytes;return 0;
 }
 if(command==7&&len==8){
  unsigned offset=get32(in),bytes=get32(in+4);
  if(job!=7)return failure(APX_CAMERA_BUSY);
  if(!guards(frame,APX_CAMERA_FRAME_BYTES))return failure(APX_CAMERA_HARDWARE);
  if(bytes>APXD_MAX_PAYLOAD||offset>APX_CAMERA_FRAME_BYTES||bytes>APX_CAMERA_FRAME_BYTES-offset)return failure(APX_CAMERA_RANGE);
  memcpy(out,FRAME+offset,bytes);*size=bytes;return 0;
 }
 if(command==4&&len>1&&len<=1024&&in[len-1]==0&&!memchr(in,0,len-1)){
  if(job!=4)return failure(APX_CAMERA_BUSY);
  if(!guards(stream,524288))return failure(APX_CAMERA_HARDWARE);
  return save((const char *)in);
 }
 return APXD_INVALID;
}
