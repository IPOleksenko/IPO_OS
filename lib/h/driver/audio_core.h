#ifndef LIB_DRIVER_AUDIO_CORE_H
#define LIB_DRIVER_AUDIO_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define AUDIO_FORMAT_U8               1
#define AUDIO_FORMAT_S16              2
#define AUDIO_FORMAT_S24              3
#define AUDIO_FORMAT_S32              4
#define AUDIO_FORMAT_F32              5

#define AUDIO_BACKEND_NONE            0
#define AUDIO_BACKEND_PC_SPEAKER      1
#define AUDIO_BACKEND_SB16            2
#define AUDIO_BACKEND_AC97            3

typedef struct audio_stream {
    int          format;         /* AUDIO_FORMAT_* */
    int          channels;       /* 1 (mono), 2 (stereo), etc. */
    uint32_t     sample_rate;    /* 8000 .. 192000 Hz */
    const uint8_t *data;         /* Pointer to PCM samples */
    size_t       data_size;      /* Total size in bytes */
    size_t       cursor;         /* Current byte offset */
    bool         is_playing;
    bool         looping;
    float        volume;         /* 0.0f .. 1.0f */
    double       fractional_pos; /* Fractional sample accumulator for resampler */
    size_t       ring_size;      /* 0 = linear buffer, >0 = circular ring buffer in samples */
    size_t       buffered_samples;/* Total samples decoded/written so far */
    bool         is_eof;         /* True when streaming decoder reached end of file */
    struct audio_stream *next;
} audio_stream_t;

/* System-wide audio interface */
bool            audio_init(void);
int             audio_get_active_backend(void);
const char     *audio_get_backend_name(void);

audio_stream_t *audio_stream_create(int format, int channels, uint32_t sample_rate, const void *data, size_t size);
audio_stream_t *audio_stream_create_ring(int format, int channels, uint32_t sample_rate,
                                         const void *ring_buf, size_t ring_size_samples,
                                         size_t total_expected_samples);
void            audio_stream_update_buffered(audio_stream_t *stream, size_t buffered_samples, bool is_eof);
void            audio_stream_destroy(audio_stream_t *stream);
void            audio_stream_play(audio_stream_t *stream);
void            audio_stream_pause(audio_stream_t *stream);
void            audio_stream_stop(audio_stream_t *stream);
void            audio_stream_set_volume(audio_stream_t *stream, float volume);
void            audio_stream_seek(audio_stream_t *stream, double progress_fraction);
double          audio_stream_get_progress(audio_stream_t *stream);

/* Resamples, mixes all active streams and outputs audio to hardware */
void            audio_tick(void);

/* Stops Sound Blaster 16 DMA output immediately */
void            sb16_stop(void);

/* Stops all audio playback across all hardware backends */
void            audio_stop_all(void);

#endif /* LIB_DRIVER_AUDIO_CORE_H */

