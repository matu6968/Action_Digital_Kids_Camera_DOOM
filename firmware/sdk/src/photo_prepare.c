#include "photo_prepare.h"
#include <string.h>
/* Ported from the stock Silvergear application (SHA256 9f9b9e62f9ae552106fbc56d848a17de2ab619f6b1e0fb299ce78fb45f0bc3cd):
 * 0206c6a4 sharpen, 020680c8 CLAHE, 020688a8 quantiser, wrappers 0205436c (Gray) and 02053e0c (Dot).
 * Control flow and arithmetic follow the native listing, including its quirks (see photo_prepare.h). */

static int clamp255(int v) {return v<0?0:v>255?255:v;}
__attribute__((weak,noinline)) void apx_photo_yield(void) {}

int apx_photo_sharpen(uint8_t *img,unsigned w,unsigned h,int strength,int offset,uint8_t *scratch,size_t scratch_bytes) {
 if(!img||!scratch||((w|h)&1)||w<3||h<3||scratch_bytes<(size_t)w*h)return -1;
 if(strength<1)strength=1;else if(strength>5)strength=5;
 if(offset<0)offset=0;else if(offset>50)offset=50;
 static const int gain[6]={0,8,16,24,32,40};
 const int g=gain[strength];
 memcpy(scratch,img,(size_t)w*h);
 const uint8_t *s=scratch;
 for(unsigned y=1;y<h-1;++y){apx_photo_yield();for(unsigned x=1;x<w-1;++x) {
  const uint8_t *c=s+(size_t)y*w+x;
  int centre=c[0],top=c[-(int)w],bottom=c[w],left=c[-1],right=c[1];
  int tl=c[-(int)w-1],tr=c[-(int)w+1],bl=c[w-1],br=c[w+1];
  int gx=(-2*left-tl-bl)+tr+2*right+br,gy=(-2*top-tl-tr)+bl+2*bottom+br;
  int magnitude=(gx<0?-gx:gx)+(gy<0?-gy:gy);
  int d=centre-(top+bottom+left+right+centre)/5,delta;
  if(magnitude<13) {
   /* Flat area: halve a deviation of two or more (unsigned test in the stock code). */
   delta=((unsigned)(d+1)>2u)?(d>>1):0;
  } else {
   int a=(d*g)>>3,b=(d*g)>>4;
   delta=b;
   if(centre<201){delta=a;if(centre<80)delta=a+b;}
  }
  img[(size_t)y*w+x]=(uint8_t)clamp255(centre+delta);
 }}
 /* Border pixels are brightened by 1.5 (2v - v/2), top and bottom rows, then left and right columns. */
 for(unsigned x=0;x<w;++x) {
  int v=s[x];img[x]=(uint8_t)clamp255(v*2-(v>>1));
  v=s[(size_t)(h-1)*w+x];img[(size_t)(h-1)*w+x]=(uint8_t)clamp255(v*2-(v>>1));
 }
 for(unsigned y=1;y<h-1;++y) {
  int v=s[(size_t)y*w];img[(size_t)y*w]=(uint8_t)clamp255(v*2-(v>>1));
  v=s[(size_t)y*w+w-1];img[(size_t)y*w+w-1]=(uint8_t)clamp255(v*2-(v>>1));
 }
 if(offset)for(size_t i=0;i<(size_t)w*h;++i)img[i]=(uint8_t)clamp255(img[i]+offset);
 return 0;
}

static void clahe_interpolate(uint8_t *p,unsigned pitch,const uint32_t *lu,const uint32_t *ru,const uint32_t *lb,const uint32_t *rb,
 unsigned sx,unsigned sy,const uint8_t *lut) {
 const uint32_t num=sx*sy;
 for(unsigned cy=0;cy<sy;++cy) {
  for(unsigned cx=0;cx<sx;++cx) {
   unsigned g=lut[*p];
   *p++=(uint8_t)(((sy-cy)*((sx-cx)*lu[g]+cx*ru[g])+cy*((sx-cx)*lb[g]+cx*rb[g]))/num);
  }
  p+=pitch-sx;
  apx_photo_yield();
 }
}

int apx_photo_clahe(uint8_t *img,unsigned w,unsigned h,unsigned low,unsigned high,
 unsigned nx,unsigned ny,unsigned bins,float clip,uint32_t *hist,size_t hist_bytes) {
 low&=0xff;high&=0xff;
 if(!img||!hist)return -1;
 if(nx>0x40)return -1;
 if(ny>0x40)return -2;
 if(!nx||!ny||nx<2||ny<2)return -7;
 unsigned xs=w/nx;
 if(w!=xs*nx)return -3;
 if(ny&h)return -4; /* stock tests (ny & height), not height % ny */
 if(!(low<high))return -6;
 if(clip==1.0f)return 0;
 if(!bins)bins=128;
 if(hist_bytes<(size_t)nx*ny*bins*sizeof(uint32_t))return -8;
 unsigned ys=h/ny;
 uint32_t limit;
 if(clip>0.0f){
  float tile=(float)(xs*ys),scaled=clip*tile;
  limit=(uint32_t)(scaled/(float)bins);if(!limit)limit=1;
 } else limit=0x4000;
 uint8_t lut[256];
 {unsigned step=((high-low)/bins+1)&0xff;
  for(unsigned g=low;g<=high;++g)lut[g]=(uint8_t)((int)(g-low)/(int)step);}
 for(unsigned ty=0;ty<ny;++ty)for(unsigned tx=0;tx<nx;++tx) {
  uint32_t *hh=hist+(size_t)(ty*nx+tx)*bins;
  const uint8_t *tile=img+(size_t)ty*ys*w+(size_t)tx*xs;
  memset(hh,0,bins*sizeof(uint32_t));apx_photo_yield();
  for(unsigned y=0;y<ys;++y)for(unsigned x=0;x<xs;++x)++hh[lut[tile[(size_t)y*w+x]]];
  /* Clip the histogram and redistribute the excess (stock arithmetic, not the textbook one). */
  uint32_t excess=0;
  for(unsigned i=0;i<bins;++i)if((int32_t)(hh[i]-limit)>0)excess+=hh[i]-limit;
  uint32_t incr=excess/bins;
  for(unsigned i=0;i<bins;++i) {
   uint32_t v=hh[i];
   if(limit<v)hh[i]=limit;
   else if(limit-incr<v){excess=(excess-limit)+v;hh[i]=limit;}
   else{excess-=incr;hh[i]=v+incr;}
  }
  while(excess) {
   uint32_t *p=hh;
   while(p<hh+bins&&excess) {
    uint32_t step=bins/excess;if(!step)step=1;
    for(uint32_t *q=p;excess&&q<hh+bins;q+=step)if(*q<limit){--excess;++*q;}
    ++p;
   }
  }
  float scale=(float)(high-low)/(float)(xs*ys);
  uint32_t sum=0;
  for(unsigned i=0;i<bins;++i) {
   sum+=hh[i];
   uint32_t v=(uint32_t)((float)low+(float)(int32_t)sum*scale);
   hh[i]=v>high?high:v;
  }
 }
 uint8_t *p=img;
 unsigned sx=xs>>1,sy=ys>>1;
 for(unsigned uy=0;uy<=ny;++uy) {
  unsigned yu,yb;
  if(uy==0){sy=ys>>1;yu=yb=0;}
  else if(uy==ny){sy=ys>>1;yu=yb=ny-1;}
  else{sy=ys;yu=uy-1;yb=uy;}
  for(unsigned ux=0;ux<=nx;++ux) {
   unsigned xl,xr;
   if(ux==0){sx=xs>>1;xl=xr=0;}
   else if(ux==nx){sx=xs>>1;xl=xr=nx-1;}
   else{sx=xs;xl=ux-1;xr=ux;}
   clahe_interpolate(p,w,hist+(size_t)(yu*nx+xl)*bins,hist+(size_t)(yu*nx+xr)*bins,
    hist+(size_t)(yb*nx+xl)*bins,hist+(size_t)(yb*nx+xr)*bins,sx,sy,lut);
   p+=sx;
  }
  p+=(size_t)(sy-1)*w;
 }
 return 0;
}

/* Quantise one value to `levels` steps; stock rounding ((v*(L-1)+127)/255*255+(L-1)/2)/(L-1). */
static int quant_level(int v,int levels) {
 int steps=levels-1;
 return clamp255((((v*steps+0x7f)/0xff)*0xff+(steps>>1))/steps);
}
void apx_photo_quantise(uint8_t *img,unsigned w,unsigned h,unsigned levels) {
 if(!img||w<3||h<3)return;
 if(levels<2)levels=16;
 const long n=(long)w*h;
 #define ADD(idx,delta) do{long i_=(idx);if(i_>=0&&i_<n)img[i_]=(uint8_t)clamp255(img[i_]+(delta));}while(0)
 for(unsigned y=1;y<h-1;++y) {
  apx_photo_yield();
  for(unsigned x=1;x<w-1;++x) {
   long p=(long)y*w+x;
   int v=img[p],q=quant_level(v,levels),e=v-q;
   img[p]=(uint8_t)q;
   /* Error weights /32, truncated toward zero. The left side of the next two rows is mirrored
    * (outer 4, inner 2; and 2, 1) relative to the right side, exactly as the stock code does. */
   int e8=(e*8)/32,e4=(e*4)/32,e2=(e*2)/32,e1=e/32;
   ADD(p+1,e8);ADD(p+2,e4);
   ADD(p+w-2,e4);ADD(p+w-1,e2);ADD(p+w,e8);ADD(p+w+1,e4);ADD(p+w+2,e2);
   ADD(p+2L*w-2,e2);ADD(p+2L*w-1,e1);ADD(p+2L*w,e4);ADD(p+2L*w+1,e2);ADD(p+2L*w+2,e1);
  }
  /* First and last column of the row are quantised without diffusion. */
  long a=(long)y*w,b=(long)y*w+w-1;
  img[a]=(uint8_t)quant_level(img[a],levels);
  img[b]=(uint8_t)quant_level(img[b],levels);
 }
 for(unsigned x=0;x<w;++x){long i=(long)(h-1)*w+x;img[i]=(uint8_t)quant_level(img[i],levels);}
 #undef ADD
}

/* Stock Dot wrapper 02053e0c: which sizes get the CLAHE stage, and with how many tiles (clip 2.0, 64 bins). */
static int dot_tiles(unsigned w,unsigned h) {
 if(w==512&&h==384)return 8;
 if(w==672&&h==384)return 8;
 if(w==384&&h==672)return 8;
 if(w==384&&h==216)return 8;
 if(w==216&&h==384)return 8;
 if((w==320&&(h==180||h==240)))return 10;
 return 0;
}

int apx_photo_prepare(enum apx_printer_mode mode,uint8_t *img,unsigned w,unsigned h,void *scratch,size_t scratch_bytes) {
 if(!img||!scratch||!w||!h||scratch_bytes<(size_t)w*h)return -1;
 if(mode==APX_PRINTER_DOT) {
  int tiles=dot_tiles(w,h);
  if(tiles&&(uintptr_t)scratch%4==0)(void)apx_photo_clahe(img,w,h,0,255,tiles,tiles,64,2.0f,scratch,scratch_bytes);
  return 0;
 }
 int e=apx_photo_sharpen(img,w,h,4,0,scratch,scratch_bytes);if(e)return e;
 if((uintptr_t)scratch%4==0)(void)apx_photo_clahe(img,w,h,0,255,12,12,64,1.5f,scratch,scratch_bytes); /* rejects sizes stock rejects */
 apx_photo_quantise(img,w,h,16);
 return 0;
}

int apx_photo_scale(const uint8_t *src,unsigned sw,unsigned sh,unsigned spitch,uint8_t *dst,unsigned dw,unsigned dh) {
 if(!src||!dst||!sw||!sh||!dw||!dh||spitch<sw)return -1;
 const uint32_t xr=dw>1?(uint32_t)(((uint64_t)(sw-1)<<16)/(dw-1)):0,yr=dh>1?(uint32_t)(((uint64_t)(sh-1)<<16)/(dh-1)):0;
 for(unsigned y=0;y<dh;++y) {
  uint32_t fy=y*yr;unsigned y0=fy>>16,y1=y0+1<sh?y0+1:y0,wy=(fy>>8)&255;
  const uint8_t *r0=src+(size_t)y0*spitch,*r1=src+(size_t)y1*spitch;
  for(unsigned x=0;x<dw;++x) {
   uint32_t fx=x*xr;unsigned x0=fx>>16,x1=x0+1<sw?x0+1:x0,wx=(fx>>8)&255;
   unsigned top=r0[x0]*(256-wx)+r0[x1]*wx,bottom=r1[x0]*(256-wx)+r1[x1]*wx;
   dst[(size_t)y*dw+x]=(uint8_t)((top*(256-wy)+bottom*wy+32768)>>16);
  }
 }
 return 0;
}
