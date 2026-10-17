#ifndef APPOTECH_AUDIO_H
#define APPOTECH_AUDIO_H
#include <stddef.h>
#include <stdint.h>
enum apx_audio_state { APX_AUDIO_OFF,APX_AUDIO_READY,APX_AUDIO_PLAYING,APX_AUDIO_DRAINING,APX_AUDIO_DONE,APX_AUDIO_ERROR };
enum apx_audio_error { APX_AUDIO_OK,APX_AUDIO_INVALID,APX_AUDIO_RANGE,APX_AUDIO_BUSY,APX_AUDIO_TIMEOUT,APX_AUDIO_HARDWARE,APX_AUDIO_UNSUPPORTED };
struct apx_audio_info { uint32_t state,error,rate,volume,muted,bytes,status,remaining,ticks,completed,timeouts; };
/* Independent mono signed 16-bit little-endian speaker output, polling DMA.
 * The initial clock profile supports 8000/16000/32000 samples per second.
 * PCM is aligned DDR, 32..65536 bytes, in complete 32-byte cache lines.
 * Keep it unchanged until DONE/ERROR; trailing silence avoids an abrupt stop.
 * Stop/shutdown release DMA ownership. No vendor audio routines are called. */
int apx_audio_init(void);
int apx_audio_play(const int16_t *pcm,size_t bytes,unsigned rate);
int apx_audio_volume(unsigned percent);
int apx_audio_mute(int muted);
void apx_audio_poll(void);
void apx_audio_stop(void);
void apx_audio_shutdown(void);
void apx_audio_get_info(struct apx_audio_info *info);
/* Gapless streaming (not in the upstream SDK; see src/audio.c). stream_start() is apx_audio_play() that leaves the DAC
 * running when the block ends if another block was queued in time. stream_queue() hands over the next block while one is
 * playing: 0 = accepted, APX_AUDIO_BUSY = the previous queued block has not started yet (try later), APX_AUDIO_TIMEOUT =
 * the DAC ran dry (restart with stream_start; counted in stream_underruns()). Blocks follow the apx_audio_play() rules
 * (32-byte aligned DDR, 32..65536 bytes, multiples of 32); the buffer of a queued block must stay unchanged until it has
 * played, a playing one too. Keep calling apx_audio_poll()/apx_maintenance(). */
int apx_audio_stream_start(const int16_t *pcm,size_t bytes,unsigned rate);
int apx_audio_stream_queue(const int16_t *pcm,size_t bytes);
int apx_audio_stream_ready(void);      /* a block can be queued now */
int apx_audio_stream_active(void);
uint32_t apx_audio_stream_underruns(void);
/* True when this range overlaps PCM owned by active speaker DMA. */
int apx_audio_buffer_busy(const void *buffer,size_t bytes);
#endif
