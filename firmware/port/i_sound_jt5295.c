/*
 * i_sound_jt5295.c - software mixer and DOOM's sound hooks.
 *
 * This replaces GBADoom's i_audio.c, which is entirely inside #ifdef GBA and calls Maxmod. The mixer is the classic
 * DOOM one: eight channels, nearest-neighbour resampling, summed into signed 16-bit mono.
 *
 * Output: the SDK's gapless DAC streaming (apx_audio_stream_*, sdk/src/audio.c). Two blocks of MIX_BLOCK samples are
 * kept in flight: one playing, one queued. plat_audio_poll() is called from plat_poll() (about every millisecond while the
 * engine is in its tic loop, and once per frame otherwise) and refills whichever block just finished. A block lasts
 * MIX_BLOCK / 16000 s = 64 ms, which is also the longest the engine may go without polling before the DAC runs dry
 * (it then restarts by itself and the SDK counts an underrun; the stats line prints it).
 *
 * Sample data is not copied: DOOM's DS* lumps are read straight out of the WAD in RAM. The trimmed WAD has the sounds
 * resampled to 8 kHz (tools/wad_trim.py), mixed here at 16 kHz.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <appotech/audio.h>

#include "doomtype.h"
#include "sounds.h"
#include "i_sound.h"
#include "w_wad.h"
#include "lprintf.h"

#include "plat.h"

const int snd_samplerate = 16000;

#define MIX_RATE       16000u
#define MIX_BLOCK      1024u                       /* samples per DMA block: 64 ms */
#define NUM_CHANNELS   MAX_CHANNELS
#define OUTPUT_VOLUME  70u                         /* SDK speaker volume, percent */

typedef struct {
    const uint8_t *data;      /* unsigned 8-bit PCM inside the WAD */
    uint32_t       len;       /* samples */
    uint32_t       pos;       /* 16.16 */
    uint32_t       step;      /* 16.16 increment per output sample */
    int            vol;       /* 0..127 as DOOM hands it over */
    int            active;
} chan_t;

static chan_t   chans[NUM_CHANNELS];
static int      sound_ready;
static int16_t  mixbuf[2][MIX_BLOCK] DOOM_BUFFER;
static unsigned next_block;
static int      lump_cache[NUMSFX];

#if DOOM_STATS
uint32_t audio_blocks_mixed;
#endif

/* A DOOM sound lump: 8-byte header, then unsigned 8-bit PCM.
 *   +0 u16 format (3)   +2 u16 sample rate   +4 u32 sample count */
static int lump_of(int sfxid)
{
    if (lump_cache[sfxid] != -2) return lump_cache[sfxid];

    char name[9];
    const char *n = S_sfx[sfxid].name;
    int lump = -1;
    if (n) {
        name[0] = 'D'; name[1] = 'S';
        unsigned i = 0;
        for (; i < 6 && n[i]; i++) {
            char c = n[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            name[2 + i] = c;
        }
        name[2 + i] = 0;
        lump = W_CheckNumForName(name);
        if (lump >= 0 && W_LumpLength(lump) <= 8) lump = -1;   /* emptied by tools/wad_trim.py */
    }
    lump_cache[sfxid] = lump;
    return lump;
}

int I_StartSound(int id, int channel, int vol, int sep)
{
    (void)sep;                                 /* mono, no panning */
    if (!sound_ready) return -1;
    if (channel < 0 || channel >= NUM_CHANNELS) return -1;
    if (id <= 0 || id >= NUMSFX) return -1;

    const int lump = lump_of(id);
    if (lump < 0) return -1;

    const uint8_t *p = (const uint8_t *)W_CacheLumpNum(lump);
    if (!p) return -1;

    const unsigned rate = (unsigned)p[2] | ((unsigned)p[3] << 8);
    uint32_t count = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    const int32_t avail = W_LumpLength(lump) - 8;
    if (avail <= 0 || !rate) return -1;
    if (count > (uint32_t)avail) count = (uint32_t)avail;

    chan_t *c = &chans[channel];
    c->active = 0;                             /* stop before rewriting the fields */
    c->data = p + 8;
    c->len  = count;
    c->pos  = 0;
    c->step = (rate << 16) / MIX_RATE;
    c->vol  = vol;
    c->active = 1;
    return channel;
}

static void mix_block(int16_t *dst)
{
    int any = 0;
    for (unsigned ch = 0; ch < NUM_CHANNELS; ch++) any |= chans[ch].active;
    if (!any) { memset(dst, 0, MIX_BLOCK * sizeof(int16_t)); return; }

    for (unsigned i = 0; i < MIX_BLOCK; i++) {
        int32_t acc = 0;
        for (unsigned ch = 0; ch < NUM_CHANNELS; ch++) {
            chan_t *c = &chans[ch];
            if (!c->active) continue;
            const uint32_t idx = c->pos >> 16;
            if (idx >= c->len) { c->active = 0; continue; }
            acc += ((int32_t)c->data[idx] - 128) * c->vol * 2;
            c->pos += c->step;
        }
        if (acc > 32767) acc = 32767;
        if (acc < -32768) acc = -32768;
        dst[i] = (int16_t)acc;
    }
}

void plat_audio_poll(void)
{
    if (!sound_ready) return;

    if (!apx_audio_stream_active()) {
        /* start, or restart after the DAC ran dry */
        mix_block(mixbuf[0]);
        if (apx_audio_stream_start(mixbuf[0], sizeof mixbuf[0], MIX_RATE) == 0) next_block = 1;
        return;
    }
    if (apx_audio_stream_ready()) {
        mix_block(mixbuf[next_block]);
        if (apx_audio_stream_queue(mixbuf[next_block], sizeof mixbuf[next_block]) == 0) {
            next_block ^= 1u;
#if DOOM_STATS
            audio_blocks_mixed++;
#endif
        }
    }
}

void I_InitSound(void)
{
    for (unsigned i = 0; i < NUM_CHANNELS; i++) chans[i].active = 0;
    for (unsigned i = 0; i < NUMSFX; i++) lump_cache[i] = -2;
    const int e = apx_audio_init();
    if (e) { lprintf(LO_INFO, "I_InitSound: audio init failed (%d), sound off", e); return; }
    apx_audio_volume(OUTPUT_VOLUME);
    sound_ready = 1;
    lprintf(LO_INFO, "I_InitSound: DAC ready, %u Hz mono", MIX_RATE);
}

/* Music: DOOM's MUS needs a synthesizer, which is a poor trade on a core that is busy rendering. Effects only. */
void I_PlaySong(int handle, int looping) { (void)handle; (void)looping; }
void I_PauseSong(int handle)             { (void)handle; }
void I_ResumeSong(int handle)            { (void)handle; }
void I_StopSong(int handle)              { (void)handle; }
void I_SetMusicVolume(int volume)        { (void)volume; }
