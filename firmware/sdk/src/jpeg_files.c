#include <appotech/jpeg.h>
#include <appotech/image.h>
#include <appotech/jpeg_files.h>
#include <appotech/storage.h>
#include <appotech/display.h>
#include <appotech/debug.h>
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/buttons.h>
#include <stdio.h>
#include <string.h>
#define STREAM_CAPACITY (1024u*1024u)
static uint8_t stream[STREAM_CAPACITY] __attribute__((section(".jpeg_buffer"),aligned(32)));
static uint8_t pixels[131072] __attribute__((section(".jpeg_buffer"),aligned(32)));
static char output_path[1024];
static unsigned job,phase,view_w,view_h,view_pitch,software;
static unsigned denied_reasons;
static uint32_t job_error,input_bytes,job_sequence;
static int valid_path(const char *p) {return p&&*p&&strlen(p)<sizeof(output_path);}
static int busy(void) {return job&&job<5;}
static uint32_t failure(unsigned error) {return error?0x200u+error:0;}
static void cooperate(void) {apx_usb_poll();apx_buttons_poll();apx_display_poll();apx_maintenance();}
void apx_jpeg_files_init(void) {job=phase=job_error=denied_reasons=0;}
static FRESULT read_file(const char *path) {
 FRESULT result=apx_storage_mount();if(result)return result;
 FIL file;result=f_open(&file,path,FA_READ);if(result)return result;
 FSIZE_t size=f_size(&file);input_bytes=0;
 if(size<4||size>STREAM_CAPACITY-32)result=FR_INVALID_PARAMETER;
 while(!result&&input_bytes<size) {UINT got=0,n=size-input_bytes>4096?4096:(UINT)(size-input_bytes);
  result=f_read(&file,stream+input_bytes,n,&got);if(!result&&got!=n)result=FR_DISK_ERR;
  input_bytes+=got;cooperate();}
 FRESULT close=f_close(&file);if(!result)result=close;
 if(!result)memset(stream+input_bytes,0,32);
 return result;
}
uint32_t apx_jpeg_preview_file(const char *path) {
 return apx_jpeg_preview_file_ex(path,0);
}
uint32_t apx_jpeg_preview_file_ex(const char *path,unsigned flags) {
 if(flags&~APX_IMAGE_ALLOW_SLOW)return APXD_INVALID;
 if(busy()||apx_display_status()==APX_DISPLAY_BUSY)return failure(APX_JPEG_BUSY);
 if(!valid_path(path))return APXD_INVALID;
 FRESULT result=read_file(path);if(result)return 0x100u+result;
 struct apx_image_info decoded;int error=apx_image_inspect(stream,input_bytes,&decoded);if(error)return failure(error);
 struct apx_jpeg_info image=decoded.image,hardware;unsigned use_software=apx_jpeg_inspect(stream,input_bytes,&hardware)!=0;
 denied_reasons=0;
 if(use_software&&decoded.slow_reasons&&!(flags&APX_IMAGE_ALLOW_SLOW)){
  denied_reasons=decoded.slow_reasons;return failure(APX_JPEG_SLOW);}
 unsigned sw=apx_display_width(),sh=apx_display_height();
 if(sw>224||sh>220)return failure(APX_JPEG_RANGE);
 if((uint64_t)image.width*sh>(uint64_t)image.height*sw) {view_w=sw&~1u;view_h=(view_w*image.height/image.width)&~1u;}
 else {view_h=sh&~1u;view_w=(view_h*image.width/image.height)&~1u;}
 if(!view_w||!view_h)return failure(APX_JPEG_RANGE);
 /* Avoid upscaling in the decoder. Native-size small images are centered. */
 if(view_w>image.width||view_h>image.height) {view_w=image.width&~1u;view_h=image.height&~1u;}
 if(!view_w||!view_h)return failure(APX_JPEG_RANGE);
 view_pitch=(view_w+31)&~31u;
 if(use_software){unsigned offset=(input_bytes+31)&~31u;
  error=apx_image_decode_start_ex(stream,input_bytes,pixels,sizeof(pixels),view_w,view_h,stream+offset,STREAM_CAPACITY-offset,flags);}
 else error=apx_jpeg_decode_start(stream,input_bytes,pixels,sizeof(pixels),view_w,view_h);
 if(error)return failure(error);
 software=use_software;job=1;phase=job_error=0;++job_sequence;return 0;
}
/* RGB bars with an asymmetric checker/frame, converted to full-range NV21.
 * Sample geometry stays fixed; hardware performs all target scaling/DCT. */
static void color(unsigned x,unsigned y,int *r,int *g,int *b) {
 static const uint8_t bars[8][3]={{255,0,0},{0,255,0},{0,0,255},{255,255,0},{0,255,255},{255,0,255},{255,255,255},{0,0,0}};
 const uint8_t *c=bars[x*8/220];*r=c[0];*g=c[1];*b=c[2];
 if(x<4||y<4||x>=216||y>=172) {*r=*g=*b=255;}
 if(x>=16&&x<80&&y>=120&&y<176) {*r=*g=*b=((x/8+y/8)&1)?240:16;}
 if(x>=150&&x<202&&y>=28&&y<80) {*r=255;*g=128;*b=0;}
}
static uint8_t clamp(int n) {return n<0?0:n>255?255:n;}
static void sample(void) {
 memset(pixels,0,224*176);memset(pixels+224*176,128,224*88);
 for(unsigned y=0;y<176;y+=2) {
  for(unsigned x=0;x<220;x+=2){int us=0,vs=0;
   for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx){int r,g,b;color(x+dx,y+dy,&r,&g,&b);
    pixels[(y+dy)*224+x+dx]=(77*r+150*g+29*b)>>8;us+=-43*r-85*g+128*b;vs+=128*r-107*g-21*b;}
   pixels[224*176+(y/2)*224+x]=clamp(128+(vs>>10));pixels[224*176+(y/2)*224+x+1]=clamp(128+(us>>10));}
  cooperate();
 }
}
uint32_t apx_jpeg_encode_sample_file(const char *path,unsigned w,unsigned h,unsigned selector) {
 if(busy())return failure(APX_JPEG_BUSY);
 if(!valid_path(path)||!w||!h||(w|h)&1||w>2048||h>1536||selector>15)return APXD_INVALID;
 FRESULT result=apx_storage_mount();if(result)return 0x100u+result;
 FILINFO existing;result=f_stat(path,&existing);if(!result)return 0x100u+FR_EXIST;if(result!=FR_NO_FILE)return 0x100u+result;
 if(apx_display_status()==APX_DISPLAY_BUSY)return failure(APX_JPEG_BUSY);
 software=0;sample();strcpy(output_path,path);input_bytes=224*176*3/2;
 int error=apx_jpeg_encode_start(pixels,pixels+224*176,220,176,224,w,h,selector,stream,sizeof(stream));
 if(error)return failure(error);
 job=2;phase=job_error=0;++job_sequence;return 0;
}
static FRESULT save_file(unsigned bytes) {
 FIL file;FRESULT result=f_open(&file,output_path,FA_WRITE|FA_CREATE_NEW);if(result)return result;
 unsigned offset=0;
 while(!result&&offset<bytes){UINT done=0,n=bytes-offset>4096?4096:bytes-offset;
  result=f_write(&file,stream+offset,n,&done);if(!result&&done!=n)result=FR_DISK_ERR;
  offset+=done;cooperate();}
 if(!result)result=f_sync(&file);
 FRESULT close=f_close(&file);if(!result)result=close;return result;
}
void apx_jpeg_files_poll(void) {
 apx_jpeg_poll();apx_image_poll();if(!busy())return;
 struct apx_jpeg_info i;if(software&&job!=2){struct apx_image_info soft;apx_image_get_info(&soft);i=soft.image;}else apx_jpeg_get_info(&i);
 if(software&&job==1&&i.state==APX_JPEG_IDLE){job=phase=job_error=0;return;}
 if(i.state==APX_JPEG_ERROR){job_error=failure(i.error);job=6;return;}
 if(job==2&&i.state==APX_JPEG_DONE){job=4;FRESULT result=save_file(i.bytes);job_error=result?0x100u+result:0;job=result?6:5;return;}
 if(job==1&&i.state==APX_JPEG_DONE){job=3;phase=0;}
 if(job!=3)return;
 enum apx_display_state state=apx_display_status();
 if(state==APX_DISPLAY_OFF||state==APX_DISPLAY_ERROR){apx_display_init();return;}
 if(state!=APX_DISPLAY_READY)return;
 if(!phase){if(apx_display_fill(0,0,apx_display_width(),apx_display_height(),0))return;phase=1;return;}
 if(phase==1){int error=apx_display_blit_nv21((apx_display_width()-view_w)/2,(apx_display_height()-view_h)/2,
    view_w,view_h,pixels,pixels+view_pitch*view_h,view_pitch);
  if(error){job_error=failure(APX_JPEG_RANGE);job=6;}else phase=2;return;}
 job=5;
}
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int path_frame(const uint8_t *p,size_t n) {return n&&n<=1024&&p[n-1]==0&&!memchr(p,0,n-1);}
uint32_t apx_jpeg_command(unsigned command,const uint8_t *in,size_t len,uint8_t *out,size_t *size) {
 *size=0;
 if(!command&&!len){struct apx_jpeg_info i;struct apx_image_info soft;apx_image_get_info(&soft);
  if(software)i=soft.image;else apx_jpeg_get_info(&i);
  int n=snprintf((char *)out,APXD_MAX_PAYLOAD,"{\"job\":%u,\"sequence\":%lu,\"job_error\":%lu,\"engine\":%lu,\"error\":%lu,\"status\":%lu,"
    "\"source\":[%lu,%lu],\"output\":[%lu,%lu],\"pitch\":%lu,\"bytes\":%lu,\"ticks\":%lu,\"input_bytes\":%lu,"
    "\"framebuffer\":%lu,\"stream\":%lu,\"capacity\":1048576,\"progressive\":true,\"sampling\":[\"420\",\"422\",\"444\"],\"software\":%u,\"source_progressive\":%lu,\"scans\":%lu,\"backing_files\":%lu,\"backing_reads\":%lu,\"backing_writes\":%lu,\"workspace_used\":%lu,\"backing_retries\":%lu,\"software_default_max_pixels\":38720,\"software_default_max_dimension\":220,\"software_default_max_bytes\":131072,\"software_allow_slow\":true,\"last_reject_reasons\":%u,\"software_formats\":[\"JPEG\",\"PNG\",\"BMP\",\"TGA\",\"GIF-first-frame\",\"PNM\"]}",
    job,(unsigned long)job_sequence,(unsigned long)job_error,(unsigned long)i.state,(unsigned long)i.error,(unsigned long)i.status,
    (unsigned long)i.width,(unsigned long)i.height,(unsigned long)i.output_width,(unsigned long)i.output_height,(unsigned long)i.pitch,
    (unsigned long)i.bytes,(unsigned long)i.ticks,(unsigned long)input_bytes,(unsigned long)pixels,(unsigned long)stream,software,(unsigned long)soft.progressive,(unsigned long)soft.scans,(unsigned long)soft.backing_files,(unsigned long)soft.backing_reads,(unsigned long)soft.backing_writes,(unsigned long)soft.workspace_used,(unsigned long)soft.backing_retries,denied_reasons);
  if(n<0||n>=APXD_MAX_PAYLOAD)return APXD_INVALID;
  *size=n;return 0;
 }
 if(command==1&&path_frame(in,len))return apx_jpeg_preview_file((const char *)in);
 if(command==4&&len>=5&&path_frame(in+4,len-4))return apx_jpeg_preview_file_ex((const char *)in+4,get32(in));
 if(command==2&&len>=13&&path_frame(in+12,len-12))return apx_jpeg_encode_sample_file((const char *)in+12,get32(in),get32(in+4),get32(in+8));
 if(command==3&&!len){apx_image_shutdown();apx_jpeg_shutdown();if(job==3){apx_display_shutdown();apx_display_init();}job=phase=job_error=0;return 0;}
 return APXD_INVALID;
}
