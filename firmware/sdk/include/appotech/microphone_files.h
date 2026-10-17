#ifndef APPOTECH_MICROPHONE_FILES_H
#define APPOTECH_MICROPHONE_FILES_H
#include <stddef.h>
#include <stdint.h>
/* Optional guarded 64 KiB capture workspace and APXD PCM/WAV service.
 * WAV saves create a new file and never replace an existing one. */
int apx_microphone_files_init(void);
uint32_t apx_microphone_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *output_length);
#endif
