#include <appotech/image.h>
#include <appotech/soc.h>
#include <appotech/storage.h>
#include <appotech/sd.h>
#include <appotech/usb.h>
#include <appotech/buttons.h>
#include <appotech/debug.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
static void *image_alloc(size_t n);
static void *image_realloc(void *p,size_t old,size_t n);
static void image_free(void *p);
static short *image_coeff(void *jpeg,int component,int x,int y);
static int image_component(void *jpeg,int component);
static void image_block(void *jpeg,int component,int x,int y,short *data);
static void cooperate(void);
static void image_gif_error(void);
#define STBI_MALLOC(n) image_alloc(n)
#define STBI_REALLOC_SIZED(p,o,n) image_realloc(p,o,n)
#define STBI_FREE(p) image_free(p)
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_SIMD
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_MAX_DIMENSIONS 2048
#define STBI_APX_COMPONENT(j,n) image_component(j,n)
#define STBI_APX_COEFF(j,n,x,y) image_coeff(j,n,x,y)
#define STBI_APX_BLOCK(j,n,x,y,d) image_block(j,n,x,y,d)
#define STBI_APX_PROGRESS() cooperate()
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_STATIC
#define STBI_APX_SCRATCH static __attribute__((section(".large_buffer"),aligned(32)))
#define STBI_APX_GIF_ERROR() image_gif_error()
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image_sdk.h"
struct plane {
 FIL file;char path[64];DWORD links[512];
 uint8_t *data,*pixels;uint32_t bytes,row_bytes,page_bytes,slot_bytes,page,loaded,dirty,open,owned;
 uint32_t packed_size[256],checksum[256];
 uint32_t written[8];int pixel_row;
};
static struct plane planes[3] __attribute__((section(".large_buffer"),aligned(32)));
static struct apx_image_info info;
static stbi__context context;
static stbi__jpeg *decoder;
static uint8_t *arena,*output,*rgb,*packed;
static unsigned packed_capacity;
static size_t arena_size,arena_used;
static const uint8_t *source;
static size_t source_bytes;
static unsigned active,paged,phase,component,row,reading,scan_active,scan_x,scan_y,scan_w,scan_h,scan_restart;
static uint32_t start,last_work,deadline,saved_sd_clock;
static jmp_buf failure;
static void image_fail(unsigned error,unsigned line){info.image.error=error;info.image.status=line;longjmp(failure,1);}
#define fail(error) image_fail(error,__LINE__)
static void image_gif_error(void){fail(APX_JPEG_INVALID);}
static void cooperate(void){apx_usb_poll();apx_buttons_poll();apx_maintenance();
 if(apx_ticks()-last_work>32768u*600u)fail(APX_JPEG_TIMEOUT);
}
static void *image_alloc(size_t n){
 if(n>SIZE_MAX-15)return NULL;
 n=(n+15)&~(size_t)15;
 if(n>arena_size-arena_used)return NULL;
 void *p=arena+arena_used;arena_used+=n;info.workspace_used=arena_used;return p;
}
static void *image_realloc(void *p,size_t old,size_t n){void *q=image_alloc(n);if(q&&p)memcpy(q,p,old<n?old:n);return q;}
static void image_free(void *p){(void)p;}
static void transfer(struct plane *p,void *data,unsigned offset,unsigned bytes,int writing){
 if(f_lseek(&p->file,offset)||f_tell(&p->file)!=offset)fail(APX_JPEG_HARDWARE);
 for(unsigned done=0;done<bytes;){UINT count=bytes-done>4096?4096:bytes-done,n=0;
  FRESULT e=writing?f_write(&p->file,(uint8_t *)data+done,count,&n):f_read(&p->file,(uint8_t *)data+done,count,&n);
  if(e||n!=count)fail(APX_JPEG_HARDWARE);
  done+=n;cooperate();}
 if(writing)++info.backing_writes;else ++info.backing_reads;
}
/* Lossless coefficient-page RLE. Long zero runs avoid repeatedly writing
 * mostly-empty progressive DC/early-AC scans to the card. Literal samples
 * retain all 16 coefficient bits; this does not change JPEG image quality. */
static unsigned pack(const uint8_t *input,unsigned bytes){
 unsigned at=0,out=0;
 while(at<bytes){unsigned zeros=0;while(at+zeros*2<bytes&&zeros<128&&!input[at+zeros*2]&&!input[at+zeros*2+1])++zeros;
  if(zeros>=3){if(out==packed_capacity)fail(APX_JPEG_RANGE);packed[out++]=0x80|(zeros-1);at+=zeros*2;}
  else{unsigned begin=at,n=0;
   while(at<bytes&&n<128){if(n&&at+5<bytes&&!input[at]&&!input[at+1]&&!input[at+2]&&!input[at+3]&&!input[at+4]&&!input[at+5])break;at+=2;++n;}
   if(out+1+n*2>packed_capacity)fail(APX_JPEG_RANGE);
   packed[out++]=n-1;memcpy(packed+out,input+begin,n*2);out+=n*2;}
 }return out;
}
static void unpack(uint8_t *out,unsigned bytes,unsigned encoded){
 unsigned at=0,pos=0;
 while(pos<encoded){unsigned token=packed[pos++],n=((token&127)+1)*2;if(n>bytes-at)fail(APX_JPEG_HARDWARE);
  if(token&128)memset(out+at,0,n);else{if(n>encoded-pos)fail(APX_JPEG_HARDWARE);memcpy(out+at,packed+pos,n);pos+=n;}at+=n;}
 if(at!=bytes)fail(APX_JPEG_HARDWARE);
}
static uint32_t checksum(const uint8_t *data,unsigned bytes){
 static const uint32_t table[16]={0,0x1db71064,0x3b6e20c8,0x26d930ac,0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,
  0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c};
 uint32_t crc=~0u;while(bytes--){crc^=*data++;crc=(crc>>4)^table[crc&15];crc=(crc>>4)^table[crc&15];}return ~crc;
}
static void evict_file_cache(struct plane *p,unsigned offset){
 /* Visit a different sector through FatFs so a subsequent read verifies
  * backing media, including a previously dirty partial-sector buffer. */
 unsigned other=offset<512?512:0;uint8_t byte;UINT got;
 if(f_size(&p->file)<=other||f_lseek(&p->file,other)||f_read(&p->file,&byte,1,&got)||got!=1)fail(APX_JPEG_HARDWARE);
 cooperate();
}
static void flush(struct plane *p){if(p->open&&p->loaded&&p->dirty){
 unsigned offset=p->page*p->page_bytes,n=p->bytes-offset;if(n>p->page_bytes)n=p->page_bytes;
 unsigned encoded=0;uint32_t hash=0;
 for(unsigned attempt=0;;++attempt){encoded=pack(p->data,n);hash=checksum(packed,encoded);
  transfer(p,packed,p->page*p->slot_bytes,encoded,1);evict_file_cache(p,p->page*p->slot_bytes);
  transfer(p,packed,p->page*p->slot_bytes,encoded,0);
  if(checksum(packed,encoded)==hash)break;
  if(attempt==2)fail(APX_JPEG_HARDWARE);
  ++info.backing_retries;
 }
 p->packed_size[p->page]=encoded;p->checksum[p->page]=hash;
 p->written[p->page/32]|=1u<<(p->page%32);p->dirty=0;}}
static void load(struct plane *p,unsigned page){
 if(p->loaded&&p->page==page)return;
 flush(p);p->page=page;p->loaded=1;p->dirty=0;
 unsigned offset=page*p->page_bytes,n=p->bytes-offset;if(n>p->page_bytes)n=p->page_bytes;
 if(p->written[page/32]&(1u<<(page%32))){unsigned encoded=p->packed_size[page];if(!encoded||encoded>packed_capacity)fail(APX_JPEG_HARDWARE);
  for(unsigned attempt=0;;++attempt){transfer(p,packed,page*p->slot_bytes,encoded,0);
   if(checksum(packed,encoded)==p->checksum[page])break;
   if(attempt==2)fail(APX_JPEG_HARDWARE);
   ++info.backing_retries;evict_file_cache(p,page*p->slot_bytes);
  }
  unpack(p->data,n,encoded);}
 else memset(p->data,0,n);
}
static unsigned close_files(void){unsigned error=0,owned=0;
 for(unsigned n=0;n<3;++n)owned|=planes[n].owned;
 if(owned&&!apx_sd_get_info()->initialized){
  /* Controller failure invalidates every old FIL. Abandon that volume before
   * remounting; the private files were preallocated/synced before data writes. */
  if(apx_storage_unmount())return 1;
  for(unsigned n=0;n<3;++n)planes[n].open=0;
  if(apx_storage_mount())return 1;
  apx_sd_set_clock(12000000);
 }
 for(unsigned n=0;n<3;++n){struct plane *p=&planes[n];if(p->open){
   FRESULT result=f_close(&p->file);
   if(result&&result!=FR_INVALID_OBJECT)error=1;else p->open=0;
  }
  if(p->owned&&!p->open){FRESULT e=f_unlink(p->path);if(e&&e!=FR_NO_FILE)error=1;else p->owned=0;}
 }
 if(saved_sd_clock&&!error){if(apx_sd_get_info()->initialized)apx_sd_set_clock(saved_sd_clock);saved_sd_clock=0;}
 return error;
}
static int image_component(void *jpeg,int n){
 if(!paged)return 0;
 stbi__jpeg *j=jpeg;if(j->s->img_n!=1&&j->s->img_n!=3)fail(APX_JPEG_UNSUPPORTED);
 if(j->rgb==3||(j->app14_color_transform==0&&!j->jfif))fail(APX_JPEG_UNSUPPORTED);
 if(n==0){
  size_t total=0;
  for(int c=0;c<j->s->img_n;++c){unsigned w=j->img_mcu_x*j->img_comp[c].h;
   total+=(size_t)w*j->img_mcu_y*j->img_comp[c].v*128u;}
  int memory=total+16384<arena_size-arena_used;
  if(!memory){if(apx_storage_mount())fail(APX_JPEG_HARDWARE);
   saved_sd_clock=apx_sd_get_info()->clock_hz;
   if(apx_sd_set_clock(12000000))fail(APX_JPEG_HARDWARE);}
  packed_capacity=0;
  for(int c=0;c<j->s->img_n;++c){struct plane *p=&planes[c];unsigned w=j->img_mcu_x*j->img_comp[c].h;
   p->row_bytes=w*128;p->bytes=p->row_bytes*j->img_mcu_y*j->img_comp[c].v;
   p->page_bytes=memory?p->bytes:p->row_bytes*j->img_comp[c].v;
   p->slot_bytes=(p->page_bytes+p->page_bytes/256+31+16)&~31u;
   if(p->slot_bytes>packed_capacity)packed_capacity=p->slot_bytes;
   p->data=image_alloc(p->page_bytes);p->pixels=image_alloc(w*64);p->pixel_row=-1;
   if(!p->data||!p->pixels||j->img_mcu_y>256)fail(APX_JPEG_RANGE);
   if(memory){memset(p->data,0,p->bytes);p->loaded=1;p->page=0;}
   else{FRESULT e=FR_EXIST;for(unsigned attempt=0;attempt<32&&e==FR_EXIST;++attempt){
     snprintf(p->path,sizeof(p->path),"0:/APXJ%08lx%u%02u.TMP",(unsigned long)start,c,attempt);
     e=f_open(&p->file,p->path,FA_READ|FA_WRITE|FA_CREATE_NEW);}
    if(e)fail(APX_JPEG_HARDWARE);
    p->open=p->owned=1;++info.backing_files;
    unsigned file_bytes=((p->bytes+p->page_bytes-1)/p->page_bytes)*p->slot_bytes;
    if(f_lseek(&p->file,file_bytes)||f_tell(&p->file)!=file_bytes||f_sync(&p->file))fail(APX_JPEG_HARDWARE);
    p->links[0]=512;p->file.cltbl=p->links;
    if(f_lseek(&p->file,CREATE_LINKMAP)){p->file.cltbl=NULL;} /* Fragmented card: bounded normal seeking. */
   }
  }
  if(!memory){packed=image_alloc(packed_capacity);if(!packed)fail(APX_JPEG_RANGE);}
 }return 1;
}
static short *image_coeff(void *jpeg,int n,int x,int y){
 stbi__jpeg *j=jpeg;if(!paged)return j->img_comp[n].coeff+64*(x+y*j->img_comp[n].coeff_w);
 struct plane *p=&planes[n];unsigned offset=(y*j->img_comp[n].coeff_w+x)*128;
 if(offset>p->bytes-128)fail(APX_JPEG_RANGE);
 load(p,offset/p->page_bytes);if(!reading)p->dirty=1;return (short *)(p->data+offset%p->page_bytes);
}
static void image_block(void *jpeg,int n,int x,int y,short *data){stbi__jpeg *j=jpeg;
 if(paged)memcpy(image_coeff(j,n,x,y),data,128);
 else j->idct_block_kernel(j->img_comp[n].data+j->img_comp[n].w2*y*8+x*8,j->img_comp[n].w2,data);
}
static void pixel_row(unsigned n,unsigned y){struct plane *p=&planes[n];unsigned by=y/8;
 if(p->pixel_row==(int)by)return;
 short block[64];unsigned w=decoder->img_comp[n].coeff_w;
 for(unsigned x=0;x<w;++x){memcpy(block,image_coeff(decoder,n,x,by),128);
  if(decoder->progressive)stbi__jpeg_dequantize(block,decoder->dequant[decoder->img_comp[n].tq]);
  decoder->idct_block_kernel(p->pixels+x*8,w*8,block);}
 p->pixel_row=by;cooperate();
}
static void render(void){
 unsigned n=component,w=n?info.image.output_width/2:info.image.output_width,h=n?info.image.output_height/2:info.image.output_height;
 unsigned sy=(2u*row+1u)*decoder->img_comp[n].y/(2u*h);if(sy>=(unsigned)decoder->img_comp[n].y)sy=decoder->img_comp[n].y-1;
 pixel_row(n,sy);unsigned stride=decoder->img_comp[n].coeff_w*8;
 const uint8_t *pixels=planes[n].pixels+(sy%8)*stride;
 unsigned pitch=info.image.pitch;uint8_t *dest=output+(n?pitch*info.image.output_height+row*pitch:row*pitch);
 for(unsigned x=0;x<w;++x){unsigned sx=(2u*x+1u)*decoder->img_comp[n].x/(2u*w);
  if(sx>=(unsigned)decoder->img_comp[n].x)sx=decoder->img_comp[n].x-1;
  dest[n?x*2+(n==1):x]=pixels[sx];}
 if(++row==h){row=0;if(++component==(unsigned)decoder->s->img_n)phase=4;}
}
static void rgb_render(void){unsigned w=info.image.output_width,h=info.image.output_height,pitch=info.image.pitch;
 unsigned sw=info.image.width,sh=info.image.height;
 for(unsigned dy=0;dy<2;++dy)for(unsigned x=0;x<w;++x){unsigned sy=(row+dy)*sh/h,sx=x*sw/w;const uint8_t *p=rgb+3*(sy*sw+sx);
  output[(row+dy)*pitch+x]=(77u*p[0]+150u*p[1]+29u*p[2])>>8;}
 for(unsigned x=0;x<w;x+=2){int us=0,vs=0;
  for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx){const uint8_t *p=rgb+3*(((row+dy)*sh/h)*sw+(x+dx)*sw/w);
   us+=-43*p[0]-85*p[1]+128*p[2];vs+=128*p[0]-107*p[1]-21*p[2];}
  int u=128+(us>>10),v=128+(vs>>10);uint8_t *p=output+pitch*h+(row/2)*pitch+x;
  p[0]=v<0?0:v>255?255:v;p[1]=u<0?0:u>255?255:u;}
 row+=2;if(row==h)phase=4;cooperate();
}
static unsigned be16(const uint8_t *p){return (unsigned)p[0]<<8|p[1];}
static uint32_t be32(const uint8_t *p){return (uint32_t)be16(p)<<16|be16(p+2);}
static unsigned le16(const uint8_t *p){return (unsigned)p[1]<<8|p[0];}
static uint32_t le32(const uint8_t *p){return (uint32_t)le16(p+2)<<16|le16(p);}
int apx_image_inspect(const void *input,size_t bytes,struct apx_image_info *out){
 if(!input||!out||bytes<4||bytes>1048576)return APX_JPEG_RANGE;
 const uint8_t *p=input;unsigned w=0,h=0;memset(out,0,sizeof(*out));
 if(p[0]==255&&p[1]==216){size_t i=2;
  while(i+4<=bytes&&p[i]==255){while(i<bytes&&p[i]==255)++i;if(i+3>bytes)return APX_JPEG_INVALID;
   unsigned m=p[i++],n=be16(p+i);if(n<2||n>bytes-i)return APX_JPEG_INVALID;
   if(m==0xc0||m==0xc1||m==0xc2){if(n<8||p[i+2]!=8)return APX_JPEG_UNSUPPORTED;
    h=be16(p+i+3);w=be16(p+i+5);out->progressive=m==0xc2;}
   if(m==0xdb){size_t at=i+2,end=i+n;
    while(at<end){unsigned table=p[at++],wide=table>>4;if(wide>1||(table&15)>3||64u*(wide+1)>end-at)return APX_JPEG_INVALID;
     uint32_t sum=0;for(unsigned k=0;k<64;++k){unsigned q=wide?be16(p+at):p[at];if(!q)return APX_JPEG_INVALID;sum+=q;at+=wide+1;}
     /* JPEG has no canonical quality field: conservative precision heuristic. */
     if(sum<=64u*8u)out->slow_reasons|=APX_IMAGE_SLOW_QUANTIZATION;
    }}
   if(m==0xda||m==0xd9)break;
   i+=n;}
 }else if(bytes>=24&&!memcmp(p,"\x89PNG\r\n\x1a\n",8)&&!memcmp(p+12,"IHDR",4)){w=be32(p+16);h=be32(p+20);}
 else if(bytes>=10&&(!memcmp(p,"GIF87a",6)||!memcmp(p,"GIF89a",6))){w=le16(p+6);h=le16(p+8);}
 else if(bytes>=26&&p[0]=='B'&&p[1]=='M'){unsigned dib=le32(p+14);
  if(dib==12){w=le16(p+18);h=le16(p+20);}else if(bytes>=30){w=le32(p+18);uint32_t v=le32(p+22);h=(v&0x80000000u)?0u-v:v;}}
 else if(bytes>=18&&p[1]<=1&&(p[2]==1||p[2]==2||p[2]==3||p[2]==9||p[2]==10||p[2]==11)){w=le16(p+12);h=le16(p+14);}
 else if(p[0]=='P'&&(p[1]=='5'||p[1]=='6')){size_t i=2;unsigned values[3]={0};
  for(unsigned n=0;n<3;++n){for(;;){while(i<bytes&&(p[i]==' '||p[i]=='\t'||p[i]=='\r'||p[i]=='\n'))++i;
    if(i<bytes&&p[i]=='#'){while(i<bytes&&p[i]!='\n')++i;}else break;}
   unsigned digits=0;while(i<bytes&&p[i]>='0'&&p[i]<='9'){if(++digits>5)return APX_JPEG_RANGE;values[n]=values[n]*10+p[i++]-'0';}
   if(!digits)return APX_JPEG_INVALID;}w=values[0];h=values[1];if(!values[2]||values[2]>65535)return APX_JPEG_UNSUPPORTED;}
 if(!w||!h)return APX_JPEG_INVALID;
 if(w>2048||h>1536)return APX_JPEG_RANGE;
 if(w>APX_IMAGE_DEFAULT_MAX_DIMENSION||h>APX_IMAGE_DEFAULT_MAX_DIMENSION||w*h>APX_IMAGE_DEFAULT_MAX_PIXELS)
  out->slow_reasons|=APX_IMAGE_SLOW_DIMENSIONS;
 if(bytes>APX_IMAGE_DEFAULT_MAX_BYTES)out->slow_reasons|=APX_IMAGE_SLOW_INPUT;
 out->image.width=w;out->image.height=h;return 0;
}
static void scan_begin(void){
 if(!stbi__process_scan_header(decoder))fail(APX_JPEG_INVALID);
 stbi__jpeg_reset(decoder);scan_x=scan_y=scan_restart=0;
 if(decoder->scan_n==1){unsigned n=decoder->order[0];scan_w=(decoder->img_comp[n].x+7)/8;scan_h=(decoder->img_comp[n].y+7)/8;}
 else{if(decoder->progressive&&decoder->spec_start)fail(APX_JPEG_INVALID);scan_w=decoder->img_mcu_x;scan_h=decoder->img_mcu_y;}
 scan_active=1;
}
static void scan_step(void){
 short block[64];
 for(unsigned work=0;work<16&&scan_active;++work){
  unsigned count=decoder->scan_n;
  for(unsigned k=0;k<count;++k){unsigned n=decoder->order[k],horizontal=count==1?1:decoder->img_comp[n].h,vertical=count==1?1:decoder->img_comp[n].v;
   for(unsigned y=0;y<vertical;++y)for(unsigned x=0;x<horizontal;++x){unsigned bx=scan_x*horizontal+x,by=scan_y*vertical+y;
    short *p=image_coeff(decoder,n,bx,by);int ok;
    if(decoder->progressive){if(!decoder->spec_start)ok=stbi__jpeg_decode_block_prog_dc(decoder,p,&decoder->huff_dc[decoder->img_comp[n].hd],n);
     else{unsigned ac=decoder->img_comp[n].ha;ok=stbi__jpeg_decode_block_prog_ac(decoder,p,&decoder->huff_ac[ac],decoder->fast_ac[ac]);}}
    else{unsigned ac=decoder->img_comp[n].ha;ok=stbi__jpeg_decode_block(decoder,block,&decoder->huff_dc[decoder->img_comp[n].hd],&decoder->huff_ac[ac],decoder->fast_ac[ac],n,decoder->dequant[decoder->img_comp[n].tq]);
     if(ok)memcpy(p,block,128);}
    if(!ok)fail(APX_JPEG_INVALID);
   }
  }
  if(++scan_x==scan_w){scan_x=0;++scan_y;}
  int more=scan_y<scan_h;
  if(--decoder->todo<=0&&more){if(decoder->code_bits<24)stbi__grow_buffer_unsafe(decoder);
   if(decoder->marker!=0xd0+(scan_restart++&7))fail(APX_JPEG_INVALID);
   stbi__jpeg_reset(decoder);}
  if(!more){scan_active=0;if(++info.scans>256)fail(APX_JPEG_INVALID);
   if(decoder->marker==STBI__MARKER_none)decoder->marker=stbi__skip_jpeg_junk_at_end(decoder);}
  cooperate();
 }
}
int apx_image_decode_start(const void *input,size_t bytes,void *out,size_t capacity,unsigned w,unsigned h,void *workspace,size_t available){
 return apx_image_decode_start_ex(input,bytes,out,capacity,w,h,workspace,available,0);
}
int apx_image_decode_start_ex(const void *input,size_t bytes,void *out,size_t capacity,unsigned w,unsigned h,void *workspace,size_t available,unsigned flags){
 if(flags&~APX_IMAGE_ALLOW_SLOW)return APX_JPEG_INVALID;
 if(active)return APX_JPEG_BUSY;
 if(close_files())return APX_JPEG_HARDWARE;
 struct apx_image_info image;int e=apx_image_inspect(input,bytes,&image);if(e)return e;
 if(image.slow_reasons&&!(flags&APX_IMAGE_ALLOW_SLOW))return APX_JPEG_SLOW;
 unsigned pitch=(w+31)&~31u;size_t needed=(size_t)pitch*h*3/2;
 if(!w||!h||(w|h)&1||w>224||h>220||w>image.image.width||h>image.image.height||capacity<needed||!out||!workspace||available<32768||((uintptr_t)workspace&15))return APX_JPEG_RANGE;
 uintptr_t a=(uintptr_t)input,b=(uintptr_t)out,c=(uintptr_t)workspace;
 if(bytes>UINTPTR_MAX-a||needed>UINTPTR_MAX-b||available>UINTPTR_MAX-c||
  (a<b+needed&&b<a+bytes)||(a<c+available&&c<a+bytes)||(b<c+available&&c<b+needed))return APX_JPEG_RANGE;
 info=image;info.image.output_width=w;info.image.output_height=h;info.image.pitch=pitch;info.image.state=APX_JPEG_DECODING;
 arena=workspace;arena_size=available;arena_used=0;output=out;source=input;source_bytes=bytes;
 memset(planes,0,sizeof(planes));memset(output,0,pitch*h);memset(output+pitch*h,128,pitch*h/2);
 active=1;phase=component=row=reading=scan_active=0;paged=source[0]==255&&source[1]==216;rgb=NULL;decoder=NULL;
 start=last_work=apx_ticks();deadline=start+32768u*600u;return 0;
}
void apx_image_poll(void){
 if(!active)return;
 if(setjmp(failure)){close_files();active=paged=0;info.image.state=APX_JPEG_ERROR;info.image.ticks=apx_ticks()-start;return;}
 if(apx_deadline_expired(deadline))fail(APX_JPEG_TIMEOUT);
 if(!phase){if(paged){decoder=image_alloc(sizeof(*decoder));if(!decoder)fail(APX_JPEG_RANGE);memset(decoder,0,sizeof(*decoder));
   stbi__start_mem(&context,source,source_bytes);decoder->s=&context;stbi__setup_jpeg(decoder);decoder->restart_interval=0;
   if(!stbi__decode_jpeg_header(decoder,STBI__SCAN_load))fail(APX_JPEG_INVALID);
   phase=1;
  }else{int w,h,c;rgb=stbi_load_from_memory(source,source_bytes,&w,&h,&c,3);if(!rgb)fail(APX_JPEG_RANGE);phase=3;}return;}
 if(phase==1){if(scan_active){scan_step();return;}int m=stbi__get_marker(decoder);
  if(stbi__SOS(m))scan_begin();
  else if(stbi__EOI(m)){if(!info.scans)fail(APX_JPEG_INVALID);phase=2;}
  else if(!stbi__process_marker(decoder,m))fail(APX_JPEG_INVALID);
  cooperate();return;}
 if(phase==2){reading=1;render();return;}
 if(phase==3){rgb_render();return;}
 if(close_files())fail(APX_JPEG_HARDWARE);
 active=paged=0;info.image.state=APX_JPEG_DONE;info.image.bytes=info.image.pitch*info.image.output_height*3/2;info.image.ticks=apx_ticks()-start;
}
void apx_image_get_info(struct apx_image_info *out){if(out)*out=info;}
void apx_image_shutdown(void){close_files();active=paged=0;info.image.state=APX_JPEG_IDLE;decoder=NULL;}
uint32_t apx_image_prepare_stop(void){apx_image_shutdown();for(unsigned n=0;n<3;++n)if(planes[n].open||planes[n].owned)return APXD_IO;return 0;}
