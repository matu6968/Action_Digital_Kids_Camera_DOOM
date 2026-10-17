#ifndef APPOTECH_UART_H
#define APPOTECH_UART_H
#include <stddef.h>
#include <stdint.h>
/* UART0 8N1. Stock mux selectors 1..31 are alternate routes; physical pins
 * for those routes remain to be identified. TX selector6 is stock PA7.
 * RX selector -1 disables receive sampling and preserves its mux field.
 * The write-only baud divider uses the board's known inherited setting
 * on shutdown when it cannot be read back.
 * Initialization is explicit: no UART pin is changed merely by linking. */
struct apx_uart_info {uint32_t initialized,baud,divider,tx_route,rx_route,tx_bytes,rx_bytes,tx_queued,rx_queued,rx_dropped,timeouts,control,status,console_enabled,console_dropped;};
int apx_uart_init(unsigned baud,unsigned tx_route,int rx_route);
void apx_uart_poll(void);
int apx_uart_write(const void *data,size_t bytes);
size_t apx_uart_read(void *data,size_t bytes);
void apx_uart_get_info(struct apx_uart_info *info);
void apx_uart_shutdown(void);
/* Optional nonblocking stdout/stderr mirror; USB log ring stays authoritative. */
int apx_uart_console_enable(int enabled);
void apx_uart_console_put(char value);
uint32_t apx_uart_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *size);
#endif
