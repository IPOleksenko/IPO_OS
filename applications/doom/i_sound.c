/**
 * i_sound.c - Sound Blaster 16 audio driver for DOOM on IPO_OS
 *
 * Pre-converts 11025 Hz U8 DOOM SFX into 22050 Hz S16 PCM to hit
 * the high-performance integer Fast-Path 2 in IPO_OS audio_core.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "doomdef.h"
#include "doomstat.h"
#include "sounds.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#include <driver/audio_core.h>

#define NUM_CHANNELS 4

typedef struct {
    int16_t  *samples;
    uint32_t num_samples;
} cached_sfx_t;

typedef struct {
    audio_stream_t *stream;
    int handle;
    int sfx_id;
    int vol;
} doom_channel_t;

static cached_sfx_t   s_cached_sfx[NUMSFX];
static doom_channel_t s_channels[NUM_CHANNELS];
static int            s_next_handle = 1;
static boolean        s_sound_inited = false;

void I_InitSound(void) {
    int i;
    audio_init();
    memset(s_cached_sfx, 0, sizeof(s_cached_sfx));
    for (i = 0; i < NUM_CHANNELS; i++) {
        s_channels[i].stream = NULL;
        s_channels[i].handle = 0;
        s_channels[i].sfx_id = 0;
        s_channels[i].vol = 0;
    }
    s_sound_inited = true;
}

void I_UpdateSound(void) {
    int i;
    if (!s_sound_inited) return;

    /* Clean up idle streams so audio_core's stream list never accumulates dead streams */
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s_channels[i].stream && !s_channels[i].stream->is_playing) {
            audio_stream_destroy(s_channels[i].stream);
            s_channels[i].stream = NULL;
            s_channels[i].handle = 0;
        }
    }

    audio_tick();
}

void I_SubmitSound(void) {
    I_UpdateSound();
}

void I_ShutdownSound(void) {
    int i;
    if (!s_sound_inited) return;

    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s_channels[i].stream) {
            audio_stream_destroy(s_channels[i].stream);
            s_channels[i].stream = NULL;
            s_channels[i].handle = 0;
        }
    }

    for (i = 0; i < NUMSFX; i++) {
        if (s_cached_sfx[i].samples) {
            free(s_cached_sfx[i].samples);
            s_cached_sfx[i].samples = NULL;
            s_cached_sfx[i].num_samples = 0;
        }
    }

    audio_stop_all();
    sb16_stop();
    s_sound_inited = false;
}

void I_SetChannels(void) {
}

int I_GetSfxLumpNum(sfxinfo_t *sfx) {
    char namebuf[16];
    int lump;

    sprintf(namebuf, "ds%s", sfx->name);
    lump = W_CheckNumForName(namebuf);
    if (lump < 0) {
        lump = W_CheckNumForName("dspistol");
    }
    return lump;
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority) {
    int i;
    int free_chan;
    sfxinfo_t *sfx;
    int lump_len;
    unsigned char *raw;
    uint32_t orig_rate;
    uint32_t orig_count;
    float volume_frac;
    audio_stream_t *stream;
    int handle;

    (void)sep;
    (void)pitch;
    (void)priority;

    if (!s_sound_inited || id <= 0 || id >= NUMSFX) {
        return -1;
    }

    /* Convert and cache sound in Fast-Path 2 format (22050 Hz S16 mono) */
    if (!s_cached_sfx[id].samples) {
        sfx = &S_sfx[id];
        if (sfx->lumpnum < 0) {
            sfx->lumpnum = I_GetSfxLumpNum(sfx);
        }
        if (sfx->lumpnum < 0) {
            return -1;
        }

        lump_len = W_LumpLength(sfx->lumpnum);
        if (lump_len <= 8) {
            return -1;
        }

        raw = (unsigned char *)W_CacheLumpNum(sfx->lumpnum, PU_STATIC);
        if (!raw) {
            return -1;
        }

        orig_rate  = (uint32_t)raw[2] | ((uint32_t)raw[3] << 8);
        orig_count = (uint32_t)raw[4] | ((uint32_t)raw[5] << 8) | ((uint32_t)raw[6] << 16) | ((uint32_t)raw[7] << 24);

        if (orig_count > (uint32_t)(lump_len - 8)) {
            orig_count = (uint32_t)(lump_len - 8);
        }
        if (orig_count == 0) {
            return -1;
        }

        /* Upsample from 11025 to 22050 Hz, and from 8-bit unsigned to 16-bit signed */
        if (orig_rate <= 11025) {
            uint32_t out_samples = orig_count * 2;
            int16_t *buf = (int16_t *)malloc(out_samples * sizeof(int16_t));
            if (!buf) return -1;

            for (i = 0; i < (int)orig_count; i++) {
                int16_t s16 = ((int16_t)raw[8 + i] - 128) << 8;
                buf[i * 2 + 0] = s16;
                buf[i * 2 + 1] = s16;
            }
            s_cached_sfx[id].samples = buf;
            s_cached_sfx[id].num_samples = out_samples;
        } else {
            /* Already 22050 Hz */
            int16_t *buf = (int16_t *)malloc(orig_count * sizeof(int16_t));
            if (!buf) return -1;

            for (i = 0; i < (int)orig_count; i++) {
                buf[i] = ((int16_t)raw[8 + i] - 128) << 8;
            }
            s_cached_sfx[id].samples = buf;
            s_cached_sfx[id].num_samples = orig_count;
        }
    }

    if (!s_cached_sfx[id].samples || s_cached_sfx[id].num_samples == 0) {
        return -1;
    }

    /* Find an idle channel or reuse the oldest */
    free_chan = -1;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (!s_channels[i].stream || !s_channels[i].stream->is_playing) {
            free_chan = i;
            break;
        }
    }
    if (free_chan < 0) {
        free_chan = 0;
    }

    if (s_channels[free_chan].stream) {
        audio_stream_destroy(s_channels[free_chan].stream);
        s_channels[free_chan].stream = NULL;
    }

    /* Create stream using Fast-Path 2: AUDIO_FORMAT_S16, 1 channel, 22050 Hz */
    stream = audio_stream_create(AUDIO_FORMAT_S16, 1, 22050,
                                 s_cached_sfx[id].samples,
                                 s_cached_sfx[id].num_samples * sizeof(int16_t));
    if (!stream) {
        return -1;
    }

    volume_frac = (float)vol / 15.0f;
    if (volume_frac > 1.0f) volume_frac = (float)vol / 127.0f;
    if (volume_frac > 1.0f) volume_frac = 1.0f;
    if (volume_frac < 0.0f) volume_frac = 0.0f;

    audio_stream_set_volume(stream, volume_frac);
    audio_stream_play(stream);

    handle = s_next_handle++;
    if (s_next_handle <= 0) s_next_handle = 1;

    s_channels[free_chan].stream = stream;
    s_channels[free_chan].handle = handle;
    s_channels[free_chan].sfx_id = id;
    s_channels[free_chan].vol = vol;

    return handle;
}

void I_StopSound(int handle) {
    int i;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s_channels[i].handle == handle && s_channels[i].stream) {
            audio_stream_stop(s_channels[i].stream);
            audio_stream_destroy(s_channels[i].stream);
            s_channels[i].stream = NULL;
            s_channels[i].handle = 0;
            break;
        }
    }
}

int I_SoundIsPlaying(int handle) {
    int i;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s_channels[i].handle == handle && s_channels[i].stream) {
            return s_channels[i].stream->is_playing ? 1 : 0;
        }
    }
    return 0;
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch) {
    int i;
    (void)sep;
    (void)pitch;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s_channels[i].handle == handle && s_channels[i].stream) {
            float volume_frac = (float)vol / 15.0f;
            if (volume_frac > 1.0f) volume_frac = (float)vol / 127.0f;
            if (volume_frac > 1.0f) volume_frac = 1.0f;
            if (volume_frac < 0.0f) volume_frac = 0.0f;
            audio_stream_set_volume(s_channels[i].stream, volume_frac);
            break;
        }
    }
}

/* Music Stubs */
void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_SetMusicVolume(int volume) { (void)volume; }
void I_PauseSong(int handle) { (void)handle; }
void I_ResumeSong(int handle) { (void)handle; }
int  I_RegisterSong(void *data) { (void)data; return 1; }
void I_PlaySong(int handle, int looping) { (void)handle; (void)looping; }
void I_StopSong(int handle) { (void)handle; }
void I_UnRegisterSong(int handle) { (void)handle; }
