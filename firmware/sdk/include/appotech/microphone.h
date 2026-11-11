#ifndef APPOTECH_MICROPHONE_H
#define APPOTECH_MICROPHONE_H
#include <stddef.h>
#include <stdint.h>
enum apx_mic_state { APX_MIC_OFF,APX_MIC_READY,APX_MIC_CAPTURING,APX_MIC_DONE,APX_MIC_ERROR };
enum apx_mic_error { APX_MIC_OK,APX_MIC_INVALID,APX_MIC_RANGE,APX_MIC_BUSY,APX_MIC_TIMEOUT,APX_MIC_HARDWARE,APX_MIC_UNSUPPORTED };
struct apx_mic_info { uint32_t state,error,rate,bytes,status,ticks,completed,timeouts,buffer; };
/* Independent PCM16 little-endian mono microphone DMA at 8/16/32 kHz.
 * Destination: 32-byte aligned DDR, 32..65536 bytes in full cache lines.
 * Do not access/reuse it while CAPTURING. DONE means all bytes are valid and
 * the CPU cache has been invalidated. Stop/error discard the unfinished job.
 * Speaker DMA can run concurrently in a disjoint buffer. */
int apx_mic_init(void);
int apx_mic_capture_start(int16_t *pcm,size_t bytes,unsigned rate);
/* Continuous capture: the same, but when a buffer is full only the DMA stops (the analog stage and the sample FIFO keep running); apx_mic_rearm() then starts the next buffer
 * with no analog restart (a normal start puts a ~3 ms decaying DC step of several thousand counts at the start of every capture) and, called promptly, without losing samples
 * (the FIFO holds them). The buffer rules are the same. Stop with apx_mic_stop(). */
int apx_mic_capture_start_continuous(int16_t *pcm,size_t bytes,unsigned rate);
int apx_mic_rearm(int16_t *pcm,size_t bytes);
int apx_mic_rearm_prepared(int16_t *pcm,size_t bytes); /* rearm for a buffer already cleaned and invalidated by the caller: no cache work, a few microseconds */
void apx_mic_poll(void);
void apx_mic_stop(void);
void apx_mic_shutdown(void);
void apx_mic_get_info(struct apx_mic_info *info);
int apx_mic_buffer_busy(const void *buffer,size_t bytes);
#endif
