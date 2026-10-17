#include <appotech/jpeg.h>
#include <appotech/soc.h>
#include <string.h>
#define JD 0xc000u
struct header {
 unsigned width,height,vertical,offset,restart;
 uint8_t q[2][64],counts[4][16],symbols[4][162],qt[3],dc[3],ac[3];
 unsigned qmask,hmask;
};
static struct apx_jpeg_info info;
static void *output_buffer;
static unsigned output_bytes;
#include "jpeg_tables.inc"
static uint32_t start,deadline;
static unsigned be16(const uint8_t *p) { return (unsigned)p[0]<<8|p[1]; }
static int scan_end(const uint8_t *p,size_t start,size_t n,unsigned restart) {
 unsigned next=0;
 for(size_t i=start;i<n;++i){if(p[i]!=255)continue;
  while(i<n&&p[i]==255)++i;
  if(i==n)return APX_JPEG_INVALID;
  unsigned marker=p[i];if(!marker)continue;
  if(marker==0xd9)return 0;
  if(marker>=0xd0&&marker<=0xd7){if(!restart||marker!=0xd0+next)return APX_JPEG_INVALID;next=(next+1)&7;continue;}
  return APX_JPEG_UNSUPPORTED;
 }
 return APX_JPEG_INVALID;
}
static int parse(const uint8_t *p,size_t n,struct header *h) {
 memset(h,0,sizeof(*h));
 if(n<4||p[0]!=255||p[1]!=216)return APX_JPEG_INVALID;
 size_t i=2;unsigned frame=0;
 while(i<n) {
  if(p[i++]!=255)return APX_JPEG_INVALID;
  while(i<n&&p[i]==255)++i;
  if(i==n)return APX_JPEG_INVALID;
  unsigned marker=p[i++];
  if(i+2>n)return APX_JPEG_INVALID;
  size_t length=be16(p+i);if(length<2||length>n-i)return APX_JPEG_INVALID;
  const uint8_t *s=p+i+2;size_t size=length-2;i+=length;
  if(marker==0xc2)return APX_JPEG_UNSUPPORTED;
  if(marker==0xc0) {
   if(frame||size!=15||s[0]!=8||s[5]!=3)return APX_JPEG_UNSUPPORTED;
   h->height=be16(s+1);h->width=be16(s+3);
   if(!h->width||!h->height||h->width>2048||h->height>1536)return APX_JPEG_RANGE;
   if(s[6]!=1||s[9]!=2||s[12]!=3||(s[7]!=0x21&&s[7]!=0x22)||s[10]!=17||s[13]!=17)return APX_JPEG_UNSUPPORTED;
   h->vertical=s[7]&15;
   for(unsigned j=0;j<3;++j){if(s[8+3*j]>1)return APX_JPEG_UNSUPPORTED;h->qt[j]=s[8+3*j];}
   frame=1;
  } else if(marker==0xdb) {
   if(size%65)return APX_JPEG_UNSUPPORTED;
   for(size_t j=0;j<size;j+=65){unsigned id=s[j];if(id>1)return APX_JPEG_UNSUPPORTED;
    for(unsigned k=0;k<64;++k)if(!s[j+1+k])return APX_JPEG_INVALID;
    memcpy(h->q[id],s+j+1,64);h->qmask|=1u<<id;}
  } else if(marker==0xc4) {
   size_t j=0;
   while(j<size){if(size-j<17)return APX_JPEG_INVALID;unsigned id=s[j];if(id&~0x11u)return APX_JPEG_UNSUPPORTED;
    unsigned t=(id>>4)*2+(id&1),count=0,code=0;
    for(unsigned k=0;k<16;++k){unsigned c=s[j+1+k];if(code+c>=(1u<<(k+1)))return APX_JPEG_INVALID;code=(code+c)*2;count+=c;}
    if(count>(t<2?12u:162u)||!count||count>size-j-17)return APX_JPEG_INVALID;
    unsigned seen[8]={0};
    for(unsigned k=0;k<count;++k){unsigned value=s[j+17+k];
     if(seen[value/32]&(1u<<(value%32)))return APX_JPEG_INVALID;
     seen[value/32]|=1u<<(value%32);
     if((t<2&&value>11)||(t>=2&&((value&15)>10||(!(value&15)&&value!=0&&value!=0xf0))))return APX_JPEG_INVALID;}
    memcpy(h->counts[t],s+j+1,16);memcpy(h->symbols[t],s+j+17,count);h->hmask|=1u<<t;j+=17+count;
   }
  } else if(marker==0xdd) {if(size!=2)return APX_JPEG_INVALID;h->restart=be16(s);}
  else if(marker==0xda) {
   if(!frame||size!=10||s[0]!=3||s[7]!=0||s[8]!=63||s[9]!=0)return APX_JPEG_UNSUPPORTED;
   for(unsigned j=0;j<3;++j){if(s[1+j*2]!=j+1||s[2+j*2]&~0x11u)return APX_JPEG_UNSUPPORTED;
    h->dc[j]=s[2+j*2]>>4;h->ac[j]=s[2+j*2]&15;
    if(!(h->qmask&(1u<<h->qt[j]))||!(h->hmask&(1u<<h->dc[j]))||!(h->hmask&(4u<<h->ac[j])))return APX_JPEG_INVALID;
   }
   if(i>=n)return APX_JPEG_INVALID;
   h->offset=i;return scan_end(p,i,n,h->restart);
  } else if((marker>=0xc0&&marker<=0xcf)||marker==0xd8||marker==0xd9||marker==0||marker==0x01)return APX_JPEG_UNSUPPORTED;
 }
 return APX_JPEG_INVALID;
}
static const uint32_t filter[40]={
 0x40000000,0x3ffa02ff,0x39f604ff,0x32f504ff,0x28f504ff,0x1df703ff,0x12fa0200,0x08fd0100,
 0,0x0001fd08,0x0002fa12,0xff03f71d,0xff04f528,0xff04f532,0xff04f639,0xff023ffa,
 0x00004000,0x00063dfd,0xfe0f37fc,0xfd192ffb,0xfc2424fc,0xfb2f19fd,0xfc370ffe,0xfd3d0600,
 0x40000000,0x3ffa02ff,0x39f604ff,0x32f504ff,0x28f504ff,0x1df703ff,0x12fa0200,0x08fd0100,
 0,0x0001fd08,0x0002fa12,0xff03f71d,0xff04f528,0xff04f532,0xff04f639,0xff023ffa
};
static void initialize(void) {
 apx_spr_write(0x4800,apx_spr_read(0x4800)&~((1u<<16)|(1u<<22)));
 apx_spr_write(0x8840,apx_spr_read(0x8840)&~(1u<<31));
 apx_spr_write(0x8880,apx_spr_read(0x8880)&~4u);apx_delay_ms(1);
 apx_spr_write(0x8880,apx_spr_read(0x8880)|4u);apx_delay_ms(1);
 apx_spr_write(JD,0);apx_spr_write(0xb800,0);
 for(unsigned j=0;j<8;++j)apx_spr_write(JD+0x84+j*4,filter[j]);
 for(unsigned j=8;j<40;++j)apx_spr_write(JD+0xc4+(j-8)*4,filter[j]);
 apx_spr_write(JD+0x148,0x22105105);apx_spr_write(JD+0xb0,4092);
}
static void tables(const struct header *h) {
 volatile uint8_t *t=(volatile uint8_t *)0x66400;
 for(unsigned j=0;j<796;++j)t[j]=0;
 for(unsigned j=0;j<128;++j)t[j]=h->q[j/64][j%64];
 static const unsigned symbol_offsets[]={448,460,472,634};
 for(unsigned j=0;j<4;++j){unsigned code=0,total=0;
  for(unsigned k=0;k<16;++k){unsigned o=256+j*32+k*2;t[o]=code;t[o+1]=code>>8;t[384+j*16+k]=total;
   unsigned count=h->counts[j][k];total+=count;code=(code+count)*2;}
  for(unsigned k=0;k<total;++k)t[symbol_offsets[j]+k]=h->symbols[j][k];
 }
}
static unsigned horizontal_ratio(unsigned source,unsigned target) {
 unsigned exponent=0,reduced=source;
 while(reduced>1920){++exponent;reduced=(source+(1u<<exponent)-1)>>exponent;}
 unsigned chunks=reduced/(target*4),extra=0;
 while(chunks){++extra;chunks>>=1;}
 exponent+=extra;reduced=(source+(1u<<exponent)-1)>>exponent;
 return (exponent<<18)|(((reduced<<16)+target-1)/target);
}
static int range(const void *p,size_t n) {uintptr_t a=(uintptr_t)p;return !(a&31)&&a>=APX_RAM_BASE&&a<APX_RAM_END&&n<=APX_RAM_END-a;}
int apx_jpeg_decode_start(const void *jpeg,size_t bytes,void *output,size_t capacity,unsigned w,unsigned hgt) {
 if(info.state==APX_JPEG_DECODING||info.state==APX_JPEG_ENCODING)return APX_JPEG_BUSY;
 if(bytes<4||bytes>APX_RAM_END-APX_RAM_BASE||!w||!hgt||(w|hgt)&1||w>224||hgt>220)return APX_JPEG_RANGE;
 unsigned pitch=(w+31)&~31u;size_t needed=(size_t)pitch*hgt*3/2;
 if(capacity<needed||!range(jpeg,(bytes+63)&~31u)||!range(output,needed))return APX_JPEG_RANGE;
 uintptr_t a=(uintptr_t)jpeg,b=(uintptr_t)output;
 if(a<b+needed&&b<a+bytes)return APX_JPEG_RANGE;
 struct header header;int error=parse(jpeg,bytes,&header);if(error)return error;
 if(w>header.width||hgt>header.height)return APX_JPEG_RANGE;
 memset(&info,0,sizeof(info));info.width=header.width;info.height=header.height;
 info.output_width=w;info.output_height=hgt;info.pitch=pitch;
 initialize();tables(&header);
 apx_dma_clean(jpeg,(bytes+63)&~31u);apx_dma_invalidate(output,needed);
 apx_spr_write(JD,0x60000023u|(header.vertical==1?0x10000u:0));
 apx_spr_write(JD+4,0);apx_spr_write(JD+0xc,(header.width<<16)|header.height);
 apx_spr_write(JD+0x10,b);apx_spr_write(JD+0x14,b+pitch*hgt);
 apx_spr_write(JD+0x20,(a+header.offset)&~3u);apx_spr_write(JD+0x24,0);apx_spr_write(JD+0x2c,((a+header.offset)&3)*8);
 apx_spr_write(JD+0x28,0);apx_spr_write(JD+0x34,header.restart);apx_spr_write(JD+0x3c,0x80000160);
 unsigned control=0x902402|(header.vertical<<3);
 static const unsigned shifts[3][3]={{6,8,9},{16,18,19},{26,28,29}};
 for(unsigned j=0;j<3;++j)control|=(unsigned)header.qt[j]<<shifts[j][0]|(unsigned)header.dc[j]<<shifts[j][1]|(unsigned)header.ac[j]<<shifts[j][2];
 apx_spr_write(JD+0x4c,control);apx_spr_write(JD+0x78,(w<<16)|hgt);
 apx_spr_write(JD+0x7c,horizontal_ratio(header.width,w));
 apx_spr_write(JD+0x80,((header.height<<16)+hgt-1)/hgt);
 apx_spr_write(JD+0xc0,(header.width<<16)|pitch);
 output_buffer=output;output_bytes=needed;start=apx_ticks();deadline=start+65536;info.state=APX_JPEG_DECODING;
 apx_spr_write(JD,apx_spr_read(JD)|0x20000063u);return 0;
}
static void stop(void) {
 apx_spr_write(JD,apx_spr_read(JD)|128u);apx_spr_write(JD,0);apx_spr_write(JD+0xb0,4092);
 apx_spr_write(JD+0x10,0);apx_spr_write(JD+0x14,0);apx_spr_write(JD+0x20,0);apx_spr_write(JD+0x24,0);
 apx_spr_write(0xb800,0);info.state=APX_JPEG_IDLE;output_buffer=NULL;
}
void apx_jpeg_shutdown(void) {
 stop();apx_spr_write(JD+0x3c,0);apx_spr_write(JD+0x4c,0);apx_spr_write(JD+0xc0,0);
 apx_spr_write(0x8880,apx_spr_read(0x8880)&~4u);apx_delay_ms(1);
 apx_spr_write(0x8880,apx_spr_read(0x8880)|4u);
 apx_spr_write(0x8840,apx_spr_read(0x8840)|(1u<<31));
}
void apx_jpeg_poll(void) {
 if(info.state!=APX_JPEG_DECODING&&info.state!=APX_JPEG_ENCODING)return;
 info.status=apx_spr_read(JD+0x40);
 if(info.status&0x3120u || (info.state==APX_JPEG_ENCODING&&(info.status&0x89u)&&!(info.status&0x40u)))info.error=APX_JPEG_HARDWARE;
 else if(info.status&0x40u)info.error=0;
 else if(apx_deadline_expired(deadline))info.error=APX_JPEG_TIMEOUT;
 else return;
 void *buffer=output_buffer;unsigned error=info.error;unsigned encoded=info.state==APX_JPEG_ENCODING;
 if(encoded&&!error){uintptr_t end=apx_spr_read(JD+0x1c);uintptr_t base=(uintptr_t)buffer;
  if(end<base||end>base+output_bytes)error=APX_JPEG_HARDWARE;else info.bytes=end-base;}
 stop();info.error=error;info.state=error?APX_JPEG_ERROR:APX_JPEG_DONE;
 apx_dma_invalidate(buffer,output_bytes);
 if(encoded&&!error){const uint8_t *data=buffer;
  /* The hardware end pointer includes up to seven 64-bit alignment bytes. */
  unsigned padding=0;
  while(info.bytes>=4&&padding<7&&!(data[info.bytes-2]==255&&data[info.bytes-1]==217)) {--info.bytes;++padding;}
  if(info.bytes<4||data[0]!=255||data[1]!=216||data[info.bytes-2]!=255||data[info.bytes-1]!=217){info.error=APX_JPEG_HARDWARE;info.state=APX_JPEG_ERROR;}}
 info.ticks=apx_ticks()-start;
}
void apx_jpeg_get_info(struct apx_jpeg_info *out) {*out=info;}
static void encoder_tables(void) {
 volatile uint8_t *t=(volatile uint8_t *)0x66000;
 for(unsigned i=0;i<1024;++i)t[i]=0;
 for(unsigned i=0;i<128;++i)t[i]=encode_quant[i];
 for(unsigned i=0;i<64;++i)t[128+i]=encode_counts[i];
 static const unsigned offsets[]={192,220,248,426};
 unsigned symbol=0;
 for(unsigned table=0;table<4;++table) {
  unsigned code=0,index=0;volatile uint16_t *packed=(volatile uint16_t *)(uintptr_t)(table<2?0x66c00+table*32:0x66800+(table-2)*512);
  for(unsigned i=0;i<(table<2?16u:256u);++i)packed[i]=0;
  for(unsigned length=1;length<=16;++length) {
   unsigned count=encode_counts[table*16+length-1];
   for(unsigned i=0;i<count;++i) {unsigned value=encode_symbols[symbol++];t[offsets[table]+index++]=value;
    packed[value]=((length-1)<<12)|(code&0xfff);++code;}
   code<<=1;
  }
 }
}
int apx_jpeg_encode_start(const void *luma,const void *chroma,unsigned sw,unsigned sh,
                         unsigned pitch,unsigned w,unsigned h,unsigned selector,void *jpeg,size_t capacity) {
 if(info.state==APX_JPEG_DECODING||info.state==APX_JPEG_ENCODING)return APX_JPEG_BUSY;
 if(!sw||!sh||!w||!h||(sw|sh|w|h)&1||sw>2048||sh>1536||w>2048||h>1536||selector>15||
    sw>w||sh>h||pitch!=((sw+31)&~31u)||capacity<4096||capacity&31||!range(jpeg,capacity)||
    !range(luma,(size_t)pitch*sh)||!range(chroma,(size_t)pitch*(sh/2)))return APX_JPEG_RANGE;
 uintptr_t a=(uintptr_t)jpeg,y=(uintptr_t)luma,vu=(uintptr_t)chroma;
 if((a<y+(size_t)pitch*sh&&y<a+capacity)||(a<vu+(size_t)pitch*(sh/2)&&vu<a+capacity))return APX_JPEG_RANGE;
 memset(&info,0,sizeof(info));info.width=sw;info.height=sh;info.output_width=w;info.output_height=h;info.pitch=pitch;
 initialize();encoder_tables();
 apx_dma_clean(luma,(size_t)pitch*sh);apx_dma_clean(chroma,(size_t)pitch*(sh/2));apx_dma_invalidate(jpeg,capacity);
 apx_spr_write(JD,0x0040ec01u|(selector<<23));
 apx_spr_write(JD+0xc,(w<<16)|h);apx_spr_write(JD+0x78,(sw<<16)|sh);
 apx_spr_write(JD+0x7c,((sw<<16)+w-1)/w);apx_spr_write(JD+0x80,((sh<<16)+h-1)/h);
 apx_spr_write(JD+0x3c,0x80000055u);apx_spr_write(JD+0xb0,0xffffffffu);apx_spr_write(JD+0x30,0x03000258);
 apx_spr_write(JD+0x10,a);apx_spr_write(JD+0x14,(a+capacity)|0x30000000u);
 apx_spr_write(JD+0x20,y);apx_spr_write(JD+0x24,vu);
 output_buffer=jpeg;output_bytes=capacity;start=apx_ticks();deadline=start+65536;info.state=APX_JPEG_ENCODING;
 apx_spr_write(JD,apx_spr_read(JD)|0x100);apx_spr_write(JD+0xa4,8);apx_spr_write(JD+0xa4,800);return 0;
}

int apx_jpeg_inspect(const void *jpeg,size_t bytes,struct apx_jpeg_info *result) {
 if(!jpeg||!result||bytes<4||bytes>1048576)return APX_JPEG_RANGE;
 struct header h;int error=parse(jpeg,bytes,&h);memset(result,0,sizeof(*result));
 result->width=h.width;result->height=h.height;result->error=error;return error;
}
