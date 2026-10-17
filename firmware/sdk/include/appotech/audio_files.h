#ifndef APPOTECH_AUDIO_FILES_H
#define APPOTECH_AUDIO_FILES_H
#include <stddef.h>
#include <stdint.h>
/* Optional 64 KiB staged PCM/tone/WAV service. WAV input: PCM16 mono,
 * 8/16/32 kHz, at most 65504 data bytes. All card access is read-only.
 * The PCM staging buffer is exclusively owned while audio plays/drains. */
int apx_audio_files_init(void);
uint32_t apx_audio_tone(unsigned frequency,unsigned milliseconds,unsigned rate);
uint32_t apx_audio_wav_file(const char *path);
uint32_t apx_audio_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *output_length);
#endif
