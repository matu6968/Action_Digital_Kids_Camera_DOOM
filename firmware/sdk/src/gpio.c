#include <appotech/gpio.h>
#include <appotech/soc.h>
static int valid(unsigned group,uint32_t mask) { return group<5&&mask&&mask<=0xffff; }
static inline __attribute__((always_inline)) void update(unsigned reg,unsigned group,uint32_t mask,uint32_t bits) {
    reg+=4*group;apx_spr_write(reg,(apx_spr_read(reg)&~mask)|(bits&mask));
}
int apx_gpio_write(unsigned group,uint32_t mask,uint32_t high) {
    if(!valid(group,mask)||(high&~mask))return -1;
    update(0x8000,group,mask,high);return 0;
}
int apx_gpio_read(unsigned group,uint32_t mask,uint32_t *high) {
    if(!valid(group,mask)||!high)return -1;
    *high=apx_spr_read(0x8000+4*group)&mask;return 0;
}
int apx_gpio_set_override(unsigned group,uint32_t mask,enum apx_gpio_override drive) {
    if(!valid(group,mask)||drive<APX_GPIO_NORMAL||drive>APX_GPIO_FORCE_LOW)return -1;
    /* Clear the opposite force before asserting the selected force. */
    if(drive==APX_GPIO_FORCE_HIGH) {
        update(0x8200,group,mask,0);update(0x81c0,group,mask,mask);
    } else {
        update(0x81c0,group,mask,0);
        update(0x8200,group,mask,drive==APX_GPIO_FORCE_LOW?mask:0);
    }
    return 0;
}
int apx_gpio_configure(unsigned group,uint32_t mask,enum apx_gpio_direction direction,
                       enum apx_gpio_pull pull,uint32_t initial_high) {
    if(!valid(group,mask)||direction<APX_GPIO_OUTPUT||direction>APX_GPIO_INPUT||
       pull<APX_GPIO_FLOATING||pull>APX_GPIO_PULL_DOWN||(initial_high&~mask))return -1;
    /* Vendor 0202ebc0. Disable drive before changing path/pulls/latch.
     * 8100/8140 electrical roles remain undocumented; stock GPIO clears both. */
    static const uint16_t regs[]={0x8040,0x81c0,0x8200,0x8380,0x8100,0x8140,0x8080,0x80c0};
    /* Table bit 0 disables drive, bit 3 selects GPIO, bits 6/7 select pulls.
     * A loop avoids pulling the general override API into display-only builds. */
    unsigned set=9u|(pull==APX_GPIO_PULL_UP?64u:pull==APX_GPIO_PULL_DOWN?128u:0u);
    for(unsigned i=0;i<8;++i)update(regs[i],group,mask,(set&(1u<<i))?mask:0);
    if(direction==APX_GPIO_OUTPUT) {
        update(0x8000,group,mask,initial_high);update(0x8040,group,mask,0);
    }
    return 0;
}
