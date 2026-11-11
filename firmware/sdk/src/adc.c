#include <appotech/adc.h>
#include <appotech/soc.h>
#include "board.h"
static unsigned users,active,started;
static uint32_t saved_control,saved_gate,deadline,settled;
static int valid(unsigned owner){return owner==APX_ADC_BUTTONS||owner==APX_ADC_BATTERY||owner==APX_ADC_PRINTER;}
int apx_adc_acquire(unsigned owner){
 if(!valid(owner))return -1;
 if(!users){saved_gate=apx_spr_read(0x8840)&(1u<<29);
  apx_spr_write(0x8840,apx_spr_read(0x8840)&~(1u<<29));
  saved_control=apx_spr_read(0x9170);
  apx_spr_write(0x9174,APX_BUTTON_ADC_DIVIDER);apx_spr_write(0x9170,0x10);}
 users|=owner;return 0;
}
int apx_adc_begin(unsigned owner,unsigned channel){
 if(!valid(owner)||!(users&owner)||channel>11)return -1;
 if(active)return 1;
 active=owner;started=0;uint32_t now=apx_ticks();deadline=now+164;settled=now+1;
 /* Stock bootstrap waits between channel selection and start. Allow one
  * reference tick for the analog mux and another for busy assertion/result
  * publication; an immediately clear busy flag is not a fresh conversion. */
 apx_spr_write(0x9170,0x10u|channel);return 0;
}
int apx_adc_result(unsigned owner,uint16_t *result){
 if(!valid(owner)||!result||active!=owner)return -1;
 if(apx_deadline_expired(deadline)){apx_spr_write(0x9170,0x10);active=0;return -1;}
 if(!apx_deadline_expired(settled))return 0;
 if(!started){apx_spr_write(0x9170,apx_spr_read(0x9170)|0x80u);started=1;settled=apx_ticks()+1;return 0;}
 if(!(apx_spr_read(0x9170)&0x40)){*result=apx_spr_read(0x9178)&1023;active=0;return 1;}
 return 0;
}
void apx_adc_release(unsigned owner){
 if(!valid(owner)||!(users&owner))return;
 if(active==owner){apx_spr_write(0x9170,0x10);active=0;}
 users&=~owner;
 if(!users){apx_spr_write(0x9170,saved_control&~0xc0u);
  apx_spr_write(0x8840,(apx_spr_read(0x8840)&~(1u<<29))|saved_gate);}
}
