#include <appotech/buttons.h>
#include <appotech/adc.h>
#include <appotech/gpio.h>
#include <appotech/soc.h>
#include <appotech/debug.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#define SAMPLE_TICKS 164u
#define DEBOUNCE_TICKS 656u
#define QUEUE_SIZE 16u
static const uint16_t centers[5]={0,306,484,658,817};
static const uint16_t saved_regs[]={0x8044,0x8084,0x80c4,0x8104,0x8144,0x81c4,0x8204,0x8384};
static uint32_t saved[8];
static struct apx_buttons_info info;
static struct apx_button_event queue[QUEUE_SIZE];
static unsigned initialized,active,head,count;
static uint32_t next_sample,candidate,candidate_since;
static void update(unsigned reg,uint32_t mask,uint32_t value) {
    apx_spr_write(reg,(apx_spr_read(reg)&~mask)|(value&mask));
}
int apx_led_set(int enabled) {
    if(!initialized||(enabled!=0&&enabled!=1))return -1;
    if(apx_gpio_set_override(APX_STATUS_LED_GROUP,APX_STATUS_LED_MASK,
        enabled?APX_GPIO_FORCE_HIGH:APX_GPIO_FORCE_LOW))return -1;
    info.led=enabled;return 0;
}
int apx_led_get(void) { return initialized?info.led:-1; }
void apx_buttons_init(void) {
    if(initialized)return;
    for(unsigned i=0;i<8;++i)saved[i]=apx_spr_read(saved_regs[i]);
    apx_adc_acquire(APX_ADC_BUTTONS);
    memset(&info,0,sizeof(info));info.adc=0xffff;
    head=count=active=0;candidate=0;candidate_since=next_sample=apx_ticks();
    apx_gpio_configure(APX_BUTTON_GROUP,APX_POWER_BUTTON_MASK|APX_BUTTON_AUX_INPUT_MASK,
                       APX_GPIO_INPUT,APX_GPIO_FLOATING,0);
    apx_gpio_configure(APX_STATUS_LED_GROUP,APX_STATUS_LED_MASK,
                       APX_GPIO_INPUT,APX_GPIO_FLOATING,0);

    initialized=1;apx_led_set(0);
}
static void debounce(uint32_t raw,uint32_t now) {
    info.raw_state=raw;
    if(raw!=candidate) { candidate=raw;candidate_since=now; }
    if(candidate==info.state||(uint32_t)(now-candidate_since)<DEBOUNCE_TICKS)return;
    struct apx_button_event event={now,candidate,candidate&~info.state,info.state&~candidate};
    info.state=candidate;
    if(count==QUEUE_SIZE) { head=(head+1)%QUEUE_SIZE;--count;++info.events_dropped; }
    queue[(head+count)%QUEUE_SIZE]=event;++count;
}
void apx_buttons_poll(void) {
    if(!initialized)return;
    uint32_t now=apx_ticks(),pins=0;
    apx_gpio_read(APX_BUTTON_GROUP,APX_POWER_BUTTON_MASK,&pins);
    info.power=pins!=0;
    uint32_t raw=info.raw_state&31u;
    if(active) {
        uint16_t value;int result=apx_adc_result(APX_ADC_BUTTONS,&value);
        if(result>0) {
            info.adc=value;raw=0;
            for(unsigned i=0;i<5;++i)
                if((int)info.adc>=(int)centers[i]-29&&info.adc<=centers[i]+29)raw|=1u<<i;
            active=0;next_sample=now+SAMPLE_TICKS;
        } else if(result<0) {
            ++info.adc_timeouts;info.adc=0xffff;raw=0;active=0;
            next_sample=now+SAMPLE_TICKS;
        }
    } else if((int32_t)(now-next_sample)>=0) {
        if(!apx_adc_begin(APX_ADC_BUTTONS,APX_BUTTON_ADC_CHANNEL))active=1;
    }
    debounce(raw|(info.power?APX_BUTTON_POWER:0),now);
}
void apx_buttons_get_info(struct apx_buttons_info *out) { if(out)*out=info; }
int apx_buttons_next(struct apx_button_event *event) {
    if(!event||!count)return 0;
    *event=queue[head];head=(head+1)%QUEUE_SIZE;--count;return 1;
}
void apx_buttons_shutdown(void) {
    if(!initialized)return;
    apx_adc_release(APX_ADC_BUTTONS);
    /* Divider 9174 is write-only on this board; stock also uses 71.
     * Restore only the clock bit we own and the affected GPIO pin fields. */

    update(0x81c4,0x23,0);update(0x8204,0x23,0);
    for(unsigned i=0;i<8;++i)update(saved_regs[i],0x23,saved[i]);
    initialized=active=count=0;
}
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put32(uint8_t *p,uint32_t value) { for(unsigned i=0;i<4;++i)p[i]=value>>(8*i); }
uint32_t apx_gpio_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *size) {
    *size=0;
    if(!initialized)return APXD_UNSUPPORTED;
    if(command==0&&length==0) {
        int n=snprintf((char *)output,APXD_MAX_PAYLOAD,
            "{\"state\":%lu,\"raw_state\":%lu,\"adc\":%u,\"power\":%u,\"led\":%u,"
            "\"adc_timeouts\":%lu,\"events_dropped\":%lu,\"buttons\":[\"SHUTTER\",\"DOWN\",\"UP\",\"LEFT\",\"RIGHT\",\"POWER\"]}",
            (unsigned long)info.state,(unsigned long)info.raw_state,info.adc,info.power,info.led,
            (unsigned long)info.adc_timeouts,(unsigned long)info.events_dropped);
        if(n<0||n>=APXD_MAX_PAYLOAD)return APXD_INVALID;
        *size=n;return APXD_OK;
    }
    if(command==1&&length==4) {
        uint32_t value=get32(input);if(value>1)return APXD_RANGE;
        return apx_led_set(value)?APXD_IO:APXD_OK;
    }
    if(command==2&&length==0) {
        struct apx_button_event event;
        while(apx_buttons_next(&event)) {
            put32(output+*size,event.ticks);put32(output+*size+4,event.state);
            put32(output+*size+8,event.pressed);put32(output+*size+12,event.released);*size+=16;
        }
        return APXD_OK;
    }
    return APXD_INVALID;
}
