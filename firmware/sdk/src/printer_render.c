#include "printer_render.h"
#include <string.h>
#include "printer-render.inc"

static int clamp(int v){return v<0?0:v>255?255:v;}
unsigned apx_printer_gray_threshold(unsigned plane){return plane<32?printer_gray_thresholds[plane]:0;}
int apx_printer_render_init(struct apx_printer_render *r,const uint8_t *source,size_t capacity,
 unsigned w,unsigned h,unsigned pitch,unsigned ow,unsigned oh,unsigned clockwise,unsigned mode,unsigned photo_curve){
 if(!r||!source||!w||!h||pitch<w||!ow||ow>APX_PRINTER_WIDTH||!oh||oh>4096||clockwise>1||
    mode>APX_PRINTER_DOT||photo_curve>2||(uint64_t)pitch*(h-1)+w>capacity||
    (uint64_t)pitch*(h-1)+w>UINT32_MAX)return APX_PRINTER_INVALID;
 memset(r,0,sizeof(*r));r->source=source;r->source_width=w;r->source_height=h;
 r->pitch=pitch;r->width=ow;r->height=oh;r->clockwise=clockwise;r->mode=mode;r->photo_curve=photo_curve;
 for(unsigned x=0;x<ow;++x)r->offset[x]=clockwise?(h-1-(uint64_t)x*h/ow)*pitch:(uint64_t)x*w/ow;
 /* Stock02068dfc..8e44; signed division truncates toward zero. */
 for(int v=0;v<256;++v)r->tone[v]=clamp(v-(v*(255-v)*3+12750)/25500);
 return 0;
}
/* Dot error diffusion for one row. Identical arithmetic to the stock filter (02068d58), but with the three error rows
 * resolved once per row and an unchecked path for pixels whose whole kernel lies inside the row; the old per-tap
 * y%3 and bounds checks cost about 6 ms per row on the 144 MHz core. */
static __attribute__((optimize("O2"))) void dot_row(struct apx_printer_render *r,unsigned y){
 const int width=(int)r->width;
 int32_t *e0=r->errors[y%3],*e1=y+1<r->height?r->errors[(y+1)%3]:NULL,*e2=y+2<r->height?r->errors[(y+2)%3]:NULL;
 const int direction=(y&1)?-1:1;
 static const int next[]={3,5,7,5,3},last[]={1,3,5,3,1};
 int x=direction>0?0:width-1;
 for(int n=0;n<width;++n,x+=direction){
  int v=r->tone[clamp(r->row[x]+(e0[x+2]+24)/48)];
  int threshold=117; /* Stock02053e0c passes112;02068e6c adds5. */
  if(v<92)threshold=clamp(threshold+((92-v)*10+31)/64);
  else if(v>=170){
   threshold=clamp(threshold+printer_dot_matrix[(y&3)*4+(x&3)]*3/8);
   if(v>=185)threshold=clamp(threshold-((v-184)*6+31)/64);
  }
  int output=v>threshold?255:0,error=v-output;
  if(!output&&v<92&&error>0)error=(error*90+50)/100;
  if(output==255&&v>184&&error<0)error=(error*92+50)/100;
  r->row[x]=output;
  /* Stock calls this FloydSteinbergDithering, but its48-weight kernel
   * spans two future rows, with serpentine scan and clipped image edges. */
  const int xa=x+direction,xb=x+2*direction;
  if(xa>=0&&xa<width)e0[xa+2]+=error*7;
  if(xb>=0&&xb<width)e0[xb+2]+=error*5;
  if(x>=2&&x<width-2){
   if(e1)for(int k=-2;k<=2;++k)e1[x+k*direction+2]+=error*next[k+2];
   if(e2)for(int k=-2;k<=2;++k)e2[x+k*direction+2]+=error*last[k+2];
  } else {
   for(int k=-2;k<=2;++k){
    int xk=x+k*direction;
    if(xk<0||xk>=width)continue;
    if(e1)e1[xk+2]+=error*next[k+2];
    if(e2)e2[xk+2]+=error*last[k+2];
   }
  }
 }
 memset(e0,0,sizeof(r->errors[0]));
}
int apx_printer_render_row(struct apx_printer_render *r,unsigned y){
 if(!r||!r->source||y!=r->next_row||y>=r->height)return APX_PRINTER_INVALID;
 uint32_t base=r->clockwise?(uint64_t)y*r->source_width/r->height:
                              (uint64_t)y*r->source_height/r->height*r->pitch;
 for(unsigned x=0;x<r->width;++x){
  unsigned v=r->source[base+r->offset[x]];
  /* Stock Gray Print's final luma mapping02054144..4178. Dot Print instead
   * applies02068d58 below. Plain grayscale jobs omit the photo-only curve. */
  if(r->mode==APX_PRINTER_GRAY&&r->photo_curve){
   if(r->photo_curve==2)v=(v*v+127)/255;
   v=((v*200)>>8)+10;
  }
  r->row[x]=v;
 }
 if(r->mode==APX_PRINTER_DOT)dot_row(r,y);
 ++r->next_row;return 0;
}
