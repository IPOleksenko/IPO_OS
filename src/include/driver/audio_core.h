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
    int          channels;       /* 1 = mono, 2 = stereo */
    uint32_t     sample_rate;    /* 8000..192000 Hz */
    const uint8_t *data;
    size_t       data_size;      /* bytes */
    size_t       cursor;         /* byte offset */
    bool         is_playing;
    bool         looping;
    float        volume;         /* 0.0f..1.0f */
    double       fractional_pos; /* resampler accumulator */
    size_t       ring_size;      /* 0 = linear buffer, >0 = ring buffer capacity in samples */
    size_t       buffered_samples;
    bool         is_eof;
    void        *owner;          /* owning process */
    struct audio_stream *next;
} audio_stream_t;

bool            audio_init(void);
int             audio_get_active_backend(void);
const char     *audio_get_backend_name(void);
void            audio_cleanup_streams_for_owner(void *owner);

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

/* Background DMA refill and stream mixing */
void            audio_poll(void);
void            audio_tick(void);

uint32_t        audio_get_refill_count(void);
bool            audio_is_running(void);

/* Halt ISA DMA audio output immediately */
void            sb16_stop(void);

void            audio_stop_all(void);

#endif /* LIB_DRIVER_AUDIO_CORE_H */

