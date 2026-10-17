#include <appotech/uart.h>
#include <appotech/soc.h>
#include <appotech/debug.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#define QUEUE 512u
#define TXQ 2048u /* transmit queue: bursts of console output (engine start-up) outrun the one-byte-per-poll drain */
static uint8_t tx[TXQ],rx[QUEUE];
static unsigned tx_in,tx_out,rx_in,rx_out,sending,receive;
static uint32_t tx_deadline;
static struct apx_uart_info info;
static const uint16_t regs[]={0x8340,0x8000,0x8040,0x8080,0x80c0,0x8100};
static uint32_t saved[6],saved_control,saved_divider,mux_mask;
static void update(unsigned r,uint32_t mask,uint32_t value){apx_spr_write(r,(apx_spr_read(r)&~mask)|(value&mask));}
int apx_uart_init(unsigned baud,unsigned route,int rx_route){
 if(baud<1200||baud>1000000||!route||route>31||rx_route< -1||rx_route>31)return -1;
 apx_uart_shutdown();memset(&info,0,sizeof(info));
 for(unsigned i=0;i<6;++i)saved[i]=apx_spr_read(regs[i]);
 saved_control=apx_spr_read(0x9080);saved_divider=apx_spr_read(0x9084);
 if(!saved_divider)saved_divider=APX_UART_BOOT_DIVIDER;
 mux_mask=31u<<5;if(rx_route>=0)mux_mask|=31;
 if(route==6){/* Stock 020246d4(0) explicitly prepares PA7 for TX. */
  update(0x8040,0x80,0);update(0x8080,0x80,0x80);update(0x8000,0x80,0x80);update(0x8100,0x80,0x80);}
 update(0x8340,mux_mask,route<<5|(rx_route>=0?(unsigned)rx_route:0));
 apx_spr_write(0x9080,0);unsigned divider=(APX_CPU_HZ_ESTIMATE+baud/2)/baud-1;
 apx_spr_write(0x9084,divider);apx_spr_write(0x908c,0x30);apx_spr_write(0x9080,0x10);
 memset(tx,0,sizeof(tx));memset(rx,0,sizeof(rx));tx_in=tx_out=rx_in=rx_out=sending=0;receive=rx_route>=0;
 info.initialized=1;info.baud=APX_CPU_HZ_ESTIMATE/(divider+1);info.divider=divider;info.tx_route=route;info.rx_route=rx_route<0?UINT32_MAX:(unsigned)rx_route;return 0;
}
void apx_uart_poll(void){
 if(!info.initialized)return;
 uint32_t status=apx_spr_read(0x908c);
 if(receive&&(status&1)){uint8_t value=apx_spr_read(0x9088);apx_spr_write(0x908c,0x10);++info.rx_bytes;
  if(rx_in-rx_out==QUEUE)++info.rx_dropped;else rx[rx_in++&(QUEUE-1)]=value;}
 if(sending){if(status&2){apx_spr_write(0x908c,1);sending=0;++info.tx_bytes;}
  else if(apx_deadline_expired(tx_deadline)){sending=0;tx_out=tx_in;++info.timeouts;}else return;}
 if(tx_out!=tx_in){apx_spr_write(0x9088,tx[tx_out++&(TXQ-1)]);sending=1;tx_deadline=apx_ticks()+328;}
}
int apx_uart_write(const void *data,size_t bytes){
 if(!info.initialized||(!data&&bytes)||bytes>TXQ)return -1;
 if(bytes>TXQ-(tx_in-tx_out))return 1;
 const uint8_t *p=data;for(size_t i=0;i<bytes;++i)tx[tx_in++&(TXQ-1)]=p[i];apx_uart_poll();return 0;
}
size_t apx_uart_read(void *data,size_t bytes){if(!data||!info.initialized)return 0;apx_uart_poll();uint8_t *p=data;size_t n=0;
 while(n<bytes&&rx_out!=rx_in)p[n++]=rx[rx_out++&(QUEUE-1)];
 return n;
}
int apx_uart_console_enable(int enabled){if(!info.initialized||(enabled!=0&&enabled!=1))return -1;info.console_enabled=enabled;if(enabled)printf("UART console mirror enabled\n");return 0;}
/* The console mirror is stdout, which only emits '\n'. A terminal needs CR+LF or every line starts where the previous one ended
 * (a staircase); a script that just reads bytes would not notice. Send "\r\n" for a bare '\n', but leave an existing "\r\n" alone. */
void apx_uart_console_put(char value){if(!info.initialized||!info.console_enabled)return;
 static char previous;
 const unsigned need=(value=='\n'&&previous!='\r')?2u:1u;
 /* A full queue is drained here (about 87 us per byte at 115200) instead of dropping the text; only a stalled transmitter,
  * which poll() gives up on after 10 ms per byte, makes the wait end with a drop. */
 for(uint32_t deadline=apx_ticks()+656;TXQ-(tx_in-tx_out)<need&&!apx_deadline_expired(deadline);)apx_uart_poll();
 if(TXQ-(tx_in-tx_out)<need){++info.console_dropped;previous=value;return;}
 if(need==2)tx[tx_in++&(TXQ-1)]='\r';
 previous=value;
 tx[tx_in++&(TXQ-1)]=(uint8_t)value;
}
void apx_uart_get_info(struct apx_uart_info *out){if(!out)return;apx_uart_poll();*out=info;
 out->tx_queued=tx_in-tx_out+sending;out->rx_queued=rx_in-rx_out;out->control=apx_spr_read(0x9080);out->status=apx_spr_read(0x908c);
}
void apx_uart_shutdown(void){if(!info.initialized)return;apx_spr_write(0x9080,0);apx_spr_write(0x908c,0x30);
 update(0x8340,mux_mask,saved[0]);if(info.tx_route==6)for(unsigned i=1;i<6;++i)update(regs[i],0x80,saved[i]);
 apx_spr_write(0x9084,saved_divider);apx_spr_write(0x9080,saved_control);info.initialized=sending=0;tx_in=tx_out=rx_in=rx_out=0;
}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
uint32_t apx_uart_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *size){
 *size=0;
 if(command==0&&!length){struct apx_uart_info i;apx_uart_get_info(&i);int n=snprintf((char *)output,APXD_MAX_PAYLOAD,
 "{\"initialized\":%lu,\"baud\":%lu,\"divider\":%lu,\"tx_route\":%lu,\"rx_route\":%lu,\"tx_bytes\":%lu,\"rx_bytes\":%lu,\"tx_queued\":%lu,\"rx_queued\":%lu,\"rx_dropped\":%lu,\"timeouts\":%lu,\"control\":%lu,\"status\":%lu,\"format\":\"8N1\",\"polling\":true,\"console_enabled\":%lu,\"console_dropped\":%lu}",
 (unsigned long)i.initialized,(unsigned long)i.baud,(unsigned long)i.divider,(unsigned long)i.tx_route,(unsigned long)i.rx_route,
 (unsigned long)i.tx_bytes,(unsigned long)i.rx_bytes,(unsigned long)i.tx_queued,(unsigned long)i.rx_queued,(unsigned long)i.rx_dropped,
 (unsigned long)i.timeouts,(unsigned long)i.control,(unsigned long)i.status,(unsigned long)i.console_enabled,(unsigned long)i.console_dropped);
 if(n<0||n>=APXD_MAX_PAYLOAD)return APXD_INVALID;
 *size=n;return 0;}
 if(command==1){if(length!=12)return APXD_INVALID;return apx_uart_init(get32(input),get32(input+4),(int32_t)get32(input+8))?APXD_RANGE:0;}
 if(command==4&&!length){apx_uart_shutdown();return 0;}
 if(!info.initialized)return APXD_UNSUPPORTED;
 if(command==5&&length==4)return apx_uart_console_enable(get32(input))?APXD_RANGE:0;
 if(command==2&&length<=QUEUE){int e=apx_uart_write(input,length);return e?APXD_IO:0;}
 if(command==3&&length==4){unsigned bytes=get32(input);if(bytes>APXD_MAX_PAYLOAD)return APXD_RANGE;*size=apx_uart_read(output,bytes);return 0;}
 return APXD_INVALID;
}
