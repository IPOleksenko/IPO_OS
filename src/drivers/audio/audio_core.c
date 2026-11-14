#include <driver/audio_core.h>
#include <driver/sound.h>
#include <system/timer.h>
#include <memory/kmalloc.h>
#include <memory/address_space.h>
#include <kernel/process.h>
#include <stdio.h>
#include <ioport.h>
#include <string.h>

#define SB16_BASE_PORT       0x220
#define SB16_MIXER_ADDR      (SB16_BASE_PORT + 0x04)
#define SB16_MIXER_DATA      (SB16_BASE_PORT + 0x05)
#define SB16_RESET_PORT      (SB16_BASE_PORT + 0x06)
#define SB16_READ_DATA_PORT  (SB16_BASE_PORT + 0x0A)
#define SB16_WRITE_DATA_PORT (SB16_BASE_PORT + 0x0C)
#define SB16_READ_STATUS     (SB16_BASE_PORT + 0x0E)
#define SB16_ACK_16BIT       (SB16_BASE_PORT + 0x0F)

#define SB16_DMA_BUFFER_ADDR 0x600000
#define SB16_SAMPLE_RATE     22050
#define SB16_HALF_SAMPLES    2048 /* 2048 stereo samples = 8192 bytes (~92.9 ms per half) */
#define SB16_TOTAL_SAMPLES   (SB16_HALF_SAMPLES * 2) /* 4096 stereo samples = 16384 bytes */
#define SB16_HALF_BYTES      (SB16_HALF_SAMPLES * 4) /* 8192 bytes */
#define SB16_TOTAL_BYTES     (SB16_TOTAL_SAMPLES * 4) /* 16384 bytes */
#define SB16_TOTAL_WORDS     (SB16_TOTAL_BYTES / 2)   /* 8192 16-bit words */
#define SB16_HALF_WORDS      (SB16_HALF_BYTES / 2)    /* 4096 16-bit words */

static int g_active_backend = AUDIO_BACKEND_NONE;
static audio_stream_t *g_stream_list = NULL;

static volatile bool     s_sb16_running = false;
static volatile int      s_last_filled_half = -1;
static volatile uint32_t s_silence_halves = 0;
static volatile int      s_audio_lock = 0;
static volatile uint32_t s_total_refills = 0;
static volatile uint32_t s_last_half_swap_ms = 0;
static volatile uint32_t s_last_sb16_poll_ms = 0;
static volatile uint16_t s_prev_dma_words_left = 0;
static volatile bool     s_dma_advancing = false;
static void             *s_audio_tramp_stack_top = NULL;

#define AUDIO_STREAM_INTERNAL_MAGIC 0x41554449u

typedef struct {
    audio_stream_t pub;
    uint32_t       magic;
    uint64_t       pos_16; /* 48.16 fixed-point sample position (integer/IRQ-safe) */
} audio_stream_internal_t;

static inline audio_stream_internal_t *audio_stream_priv(audio_stream_t *s) {
    if (!s) return NULL;
    audio_stream_internal_t *p = (audio_stream_internal_t *)s;
    return (p->magic == AUDIO_STREAM_INTERNAL_MAGIC) ? p : NULL;
}

static inline int audio_volume_to_scale256(float vol) {
    union { float f; uint32_t u; } v;
    v.f = vol;
    uint32_t bits = v.u;
    if ((bits & 0x80000000u) || (bits & 0x7FFFFFFFu) == 0u) return 0;
    uint32_t exp = (bits >> 23) & 0xFFu;
    if (exp >= 127u) return 256;
    uint32_t shift = 127u - exp + 15u;
    if (shift >= 25u) return 0;
    uint32_t mant = (1u << 23) | (bits & 0x7FFFFFu);
    int scaled = (int)(mant >> shift);
    return scaled > 256 ? 256 : scaled;
}

static inline void audio_lock(void) {
    while (__sync_lock_test_and_set(&s_audio_lock, 1)) {
        io_wait();
    }
}

static inline void audio_unlock(void) {
    __sync_lock_release(&s_audio_lock);
}

uint32_t audio_get_refill_count(void) {
    return s_total_refills;
}

bool audio_is_running(void) {
    return s_sb16_running;
}

static void sb16_dsp_write(uint8_t val) {
    for (int i = 0; i < 20000; i++) {
        if ((inb(SB16_WRITE_DATA_PORT) & 0x80) == 0) {
            outb(SB16_WRITE_DATA_PORT, val);
            return;
        }
        io_wait();
    }
}

static uint8_t sb16_dsp_read(void) {
    for (int i = 0; i < 20000; i++) {
        if (inb(SB16_READ_STATUS) & 0x80) {
            return inb(SB16_READ_DATA_PORT);
        }
        io_wait();
    }
    return 0;
}

static bool sb16_reset(void) {
    outb(SB16_RESET_PORT, 1);
    for (volatile int i = 0; i < 1000; i++) io_wait();
    outb(SB16_RESET_PORT, 0);

    for (int i = 0; i < 10000; i++) {
        if (inb(SB16_READ_STATUS) & 0x80) {
            if (inb(SB16_READ_DATA_PORT) == 0xAA) {
                return true;
            }
        }
        io_wait();
    }
    return false;
}

static inline uint16_t sb16_get_dma_words_left(void) {
    uint32_t eflags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(eflags) : : "memory");
    outb(0xD8, 0x00); /* Clear flip-flop */
    uint8_t lo = inb(0xC6);
    uint8_t hi = inb(0xC6);
    __asm__ volatile("pushl %0; popfl" : : "r"(eflags) : "memory", "cc");
    return (uint16_t)lo | ((uint16_t)hi << 8);
}

static inline uint8_t sb16_read_irq_status(void) {
    outb(SB16_MIXER_ADDR, 0x82);
    return inb(SB16_MIXER_DATA);
}

bool sb16_get_dsp_version(uint8_t *major, uint8_t *minor) {
    if (g_active_backend != AUDIO_BACKEND_SB16) return false;
    sb16_dsp_write(0xE1);
    uint8_t maj = sb16_dsp_read();
    uint8_t min = sb16_dsp_read();
    if (major) *major = maj;
    if (minor) *minor = min;
    return true;
}

void sb16_stop(void) {
    if (g_active_backend == AUDIO_BACKEND_SB16) {
        if (s_sb16_running) {
            sb16_dsp_write(0xD5); /* Pause 16-bit DMA */
            outb(0xD4, 0x05);      /* Mask DMA Ch 5 */
            inb(SB16_ACK_16BIT);   /* Acknowledge 16-bit IRQ */
            s_sb16_running = false;
        }
        s_last_filled_half = -1;
        s_silence_halves = 0;
        s_last_sb16_poll_ms = 0;
        s_prev_dma_words_left = 0;
        s_dma_advancing = false;
    }
}

void audio_stop_all(void) {
    audio_lock();
    for (audio_stream_t *s = g_stream_list; s; s = s->next) {
        s->is_playing = false;
        s->cursor = 0;
        s->fractional_pos = 0.0;
        audio_stream_internal_t *priv = audio_stream_priv(s);
        if (priv) priv->pos_16 = 0;
    }
    sb16_stop();
    sound_stop();
    audio_unlock();
}

bool audio_init(void) {
    if (g_active_backend != AUDIO_BACKEND_NONE) {
        return true;
    }

    if (!s_audio_tramp_stack_top) {
        s_audio_tramp_stack_top = address_space_get_audio_tramp_stack_top();
    }

    /* Try Sound Blaster 16 on standard I/O port 0x220 */
    if (sb16_reset()) {
        g_active_backend = AUDIO_BACKEND_SB16;
        /* Set mixer volumes to maximum */
        outb(SB16_MIXER_ADDR, 0x22); outb(SB16_MIXER_DATA, 0xFF);
        outb(SB16_MIXER_ADDR, 0x04); outb(SB16_MIXER_DATA, 0xFF);
        printf("[audio] Sound Blaster 16 initialized (Continuous Auto-Init DMA ready).\n");
        return true;
    }

    /* Fallback to PC Speaker PWM */
    sound_init();
    g_active_backend = AUDIO_BACKEND_PC_SPEAKER;
    return true;
}

int audio_get_active_backend(void) {
    if (g_active_backend == AUDIO_BACKEND_NONE) {
        audio_init();
    }
    return g_active_backend;
}

const char *audio_get_backend_name(void) {
    switch (audio_get_active_backend()) {
        case AUDIO_BACKEND_SB16:       return "Sound Blaster 16 (Continuous DMA)";
        case AUDIO_BACKEND_AC97:       return "Intel AC97 PCI Audio";
        case AUDIO_BACKEND_PC_SPEAKER: return "PC Speaker PWM (Timer 2)";
        default:                       return "None";
    }
}

static audio_stream_t *audio_stream_alloc(int format, int channels, uint32_t sample_rate,
                                          const void *data, size_t data_size, size_t ring_size) {
    if (!data || channels <= 0 || sample_rate == 0) return NULL;

    audio_stream_internal_t *priv = kmalloc(sizeof(audio_stream_internal_t));
    if (!priv) return NULL;
    memset(priv, 0, sizeof(audio_stream_internal_t));
    priv->magic = AUDIO_STREAM_INTERNAL_MAGIC;
    priv->pos_16 = 0;

    audio_stream_t *stream = &priv->pub;
    stream->format      = format;
    stream->channels    = channels;
    stream->sample_rate = sample_rate;
    stream->data        = (const uint8_t *)data;
    stream->data_size   = data_size;
    stream->volume      = 1.0f;
    stream->ring_size   = ring_size;
    stream->owner       = (void *)process_get_current();

    audio_lock();
    stream->next = g_stream_list;
    g_stream_list = stream;
    audio_unlock();

    return stream;
}

audio_stream_t *audio_stream_create(int format, int channels, uint32_t sample_rate, const void *data, size_t size) {
    if (size == 0) return NULL;
    return audio_stream_alloc(format, channels, sample_rate, data, size, 0);
}

audio_stream_t *audio_stream_create_ring(int format, int channels, uint32_t sample_rate,
                                         const void *ring_buf, size_t ring_size_samples,
                                         size_t total_expected_samples) {
    if (ring_size_samples == 0) return NULL;
    size_t data_size = total_expected_samples * 2u * (size_t)channels;
    return audio_stream_alloc(format, channels, sample_rate, ring_buf, data_size, ring_size_samples);
}

void audio_stream_update_buffered(audio_stream_t *stream, size_t buffered_samples, bool is_eof) {
    if (stream) {
        stream->buffered_samples = buffered_samples;
        stream->is_eof = is_eof;
    }
}

static void audio_stop_if_idle_locked(void) {
    for (audio_stream_t *s = g_stream_list; s; s = s->next) {
        if (s->is_playing) return;
    }
    /* Keep SB16 DMA running through its grace window; audio_poll() auto-stops after idle silence. */
    sound_stop();
}

void audio_stream_destroy(audio_stream_t *stream) {
    if (!stream) return;

    audio_lock();
    audio_stream_t **curr = &g_stream_list;
    while (*curr) {
        if (*curr == stream) {
            *curr = stream->next;
            break;
        }
        curr = &((*curr)->next);
    }
    if (g_stream_list == NULL) {
        sound_stop();
    }
    audio_unlock();

    kfree(stream);
}

void audio_cleanup_streams_for_owner(void *owner) {
    if (!owner) return;
    audio_lock();
    audio_stream_t **curr = &g_stream_list;
    while (*curr) {
        if ((*curr)->owner == owner) {
            audio_stream_t *to_free = *curr;
            *curr = to_free->next;
            kfree(to_free);
        } else {
            curr = &((*curr)->next);
        }
    }
    if (g_stream_list == NULL) {
        sb16_stop();
        sound_stop();
    }
    audio_unlock();
}

void audio_stream_play(audio_stream_t *stream) {
    if (stream) {
        stream->is_playing = true;
        audio_poll();
    }
}

void audio_stream_pause(audio_stream_t *stream) {
    if (stream) {
        audio_lock();
        stream->is_playing = false;
        audio_stop_if_idle_locked();
        audio_unlock();
    }
}

void audio_stream_stop(audio_stream_t *stream) {
    if (stream) {
        audio_lock();
        stream->is_playing = false;
        stream->cursor = 0;
        stream->fractional_pos = 0.0;
        audio_stream_internal_t *priv = audio_stream_priv(stream);
        if (priv) priv->pos_16 = 0;
        audio_stop_if_idle_locked();
        audio_unlock();
    }
}

void audio_stream_set_volume(audio_stream_t *stream, float volume) {
    if (stream) {
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 1.0f) volume = 1.0f;
        stream->volume = volume;
    }
}

static float read_sample_normalized(const uint8_t *buf, int format, size_t sample_index, size_t total_samples) {
    if (sample_index >= total_samples) return 0.0f;

    switch (format) {
        case AUDIO_FORMAT_U8: {
            uint8_t u8 = buf[sample_index];
            return ((float)u8 - 128.0f) / 128.0f;
        }
        case AUDIO_FORMAT_S16: {
            const int16_t *s16 = (const int16_t *)buf;
            return (float)s16[sample_index] / 32768.0f;
        }
        case AUDIO_FORMAT_S24: {
            size_t off = sample_index * 3;
            int32_t val = (int32_t)(buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16));
            if (val & 0x800000) val |= 0xFF000000;
            return (float)val / 8388608.0f;
        }
        case AUDIO_FORMAT_S32: {
            const int32_t *s32 = (const int32_t *)buf;
            return (float)s32[sample_index] / 2147483648.0f;
        }
        case AUDIO_FORMAT_F32: {
            const float *f32 = (const float *)buf;
            return f32[sample_index];
        }
        default:
            return 0.0f;
    }
}

static inline int32_t read_sample_s16(const uint8_t *buf, int format, size_t sample_index, size_t total_samples) {
    if (sample_index >= total_samples) return 0;

    switch (format) {
        case AUDIO_FORMAT_U8:
            return ((int32_t)buf[sample_index] - 128) << 8;
        case AUDIO_FORMAT_S16:
            return (int32_t)((const int16_t *)buf)[sample_index];
        case AUDIO_FORMAT_S24: {
            size_t off = sample_index * 3u;
            int32_t val = (int32_t)(buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16));
            if (val & 0x800000) val |= 0xFF000000;
            return val >> 8;
        }
        case AUDIO_FORMAT_S32:
            return ((const int32_t *)buf)[sample_index] >> 16;
        default:
            return 0;
    }
}

static size_t get_bytes_per_sample(int format) {
    switch (format) {
        case AUDIO_FORMAT_U8:  return 1;
        case AUDIO_FORMAT_S16: return 2;
        case AUDIO_FORMAT_S24: return 3;
        case AUDIO_FORMAT_S32:
        case AUDIO_FORMAT_F32: return 4;
        default:               return 2;
    }
}

void audio_stream_seek(audio_stream_t *stream, double progress_fraction) {
    if (!stream || stream->data_size == 0 || stream->channels <= 0) return;
    size_t b_per_s = get_bytes_per_sample(stream->format);
    size_t total_samples = stream->data_size / (b_per_s * (size_t)stream->channels);
    if (total_samples == 0) return;
    if (progress_fraction < 0.0) progress_fraction = 0.0;
    if (progress_fraction > 1.0) progress_fraction = 1.0;
    stream->fractional_pos = progress_fraction * (double)total_samples;
    stream->cursor = (size_t)(progress_fraction * (double)stream->data_size);
    audio_stream_internal_t *priv = audio_stream_priv(stream);
    if (priv) {
        priv->pos_16 = (uint64_t)(stream->fractional_pos * 65536.0);
    }
}

double audio_stream_get_progress(audio_stream_t *stream) {
    if (!stream || stream->data_size == 0 || stream->channels <= 0) return 0.0;
    size_t b_per_s = get_bytes_per_sample(stream->format);
    size_t total_samples = stream->data_size / (b_per_s * (size_t)stream->channels);
    if (total_samples == 0) return 0.0;
    audio_stream_internal_t *priv = audio_stream_priv(stream);
    double cur_pos = (g_active_backend == AUDIO_BACKEND_SB16 && priv)
        ? ((double)priv->pos_16 / 65536.0)
        : stream->fractional_pos;
    double p = cur_pos / (double)total_samples;
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;
    return p;
}

typedef struct {
    audio_stream_t *s;
    const uint8_t *stream_data;
    int16_t *buf;
    size_t total_samples;
    uint32_t block_samples;
    uint32_t rate;
    uint64_t step_16;
    int vol_scaled;
} sb16_mix_ctx_t;

static sb16_mix_ctx_t s_sb16_mix_ctx;

static const uint8_t *audio_resolve_stream_data(audio_stream_t *s, address_space_t **out_space) {
    if (out_space) *out_space = NULL;
    if (!s) return NULL;
    const uint8_t *stream_data = s->data;
    if (s->owner) {
        process_t *owner_proc = (process_t *)s->owner;
        if (!process_is_valid(owner_proc) || !owner_proc->is_running) {
            s->is_playing = false;
            return NULL;
        }
        uintptr_t d_addr = (uintptr_t)stream_data;
        if (owner_proc->binary_base && owner_proc->binary_size &&
            d_addr >= PROCESS_HEAP_START &&
            d_addr < PROCESS_HEAP_START + owner_proc->binary_size) {
            return (const uint8_t *)owner_proc->binary_base + (d_addr - PROCESS_HEAP_START);
        }
        if (out_space && owner_proc->address_space) {
            *out_space = owner_proc->address_space;
        }
    }
    return stream_data;
}

static inline bool sb16_fetch_sample_idx(audio_stream_t *s, uint64_t *pos_16,
                                         size_t total_samples, size_t *out_idx) {
    size_t s_idx = (size_t)(*pos_16 >> 16);
    if (s->ring_size > 0) {
        if (s_idx >= s->buffered_samples) {
            if (s->is_eof) {
                if (s->looping && total_samples > 0) {
                    *pos_16 = 0;
                    s_idx = 0;
                } else {
                    s->is_playing = false;
                    return false;
                }
            } else {
                return false;
            }
        }
        if (total_samples > 0 && s_idx >= total_samples && s->is_eof) {
            if (s->looping) {
                *pos_16 = 0;
                s_idx = 0;
            } else {
                s->is_playing = false;
                return false;
            }
        }
        *out_idx = s_idx % s->ring_size;
        return true;
    }

    if (s_idx >= total_samples) {
        if (s->looping && total_samples > 0) {
            *pos_16 = 0;
            s_idx = 0;
        } else {
            s->is_playing = false;
            return false;
        }
    }
    *out_idx = s_idx;
    return true;
}

static inline void sb16_mix_stereo(int16_t *dst_pair, int32_t l, int32_t r) {
    int32_t l_samp = (int32_t)dst_pair[0] + l;
    int32_t r_samp = (int32_t)dst_pair[1] + r;
    if (l_samp > 32767) l_samp = 32767; else if (l_samp < -32768) l_samp = -32768;
    if (r_samp > 32767) r_samp = 32767; else if (r_samp < -32768) r_samp = -32768;
    dst_pair[0] = (int16_t)l_samp;
    dst_pair[1] = (int16_t)r_samp;
}

static void sb16_mix_stream_tramp(void *raw_ctx) {
    sb16_mix_ctx_t *ctx = (sb16_mix_ctx_t *)raw_ctx;
    audio_stream_t *s = ctx->s;
    const uint8_t *stream_data = ctx->stream_data;
    int16_t *buf = ctx->buf;
    size_t total_samples = ctx->total_samples;
    uint32_t block_samples = ctx->block_samples;
    uint32_t rate = ctx->rate;
    uint64_t step_16 = ctx->step_16;
    int vol_scaled = ctx->vol_scaled;

    audio_stream_internal_t *priv = audio_stream_priv(s);
    uint64_t fallback_pos = 0;
    uint64_t *pos_16 = priv ? &priv->pos_16 : &fallback_pos;

    /* Fast path: S16 stereo/mono at native mixer sample rate (pure integer, 0 FPU) */
    if (s->format == AUDIO_FORMAT_S16 && (s->channels == 1 || s->channels == 2) && s->sample_rate == rate) {
        const int16_t *src = (const int16_t *)stream_data;
        bool stereo = (s->channels == 2);

        if (s->ring_size == 0) {
            uint32_t i = 0;
            while (i < block_samples) {
                size_t idx = (size_t)(*pos_16 >> 16);
                if (idx >= total_samples) {
                    if (s->looping && total_samples > 0) {
                        *pos_16 = 0;
                        idx = 0;
                    } else {
                        s->is_playing = false;
                        break;
                    }
                }
                size_t rem_src = total_samples - idx;
                uint32_t rem_out = block_samples - i;
                uint32_t chunk = (rem_src < (size_t)rem_out) ? (uint32_t)rem_src : rem_out;

                if (vol_scaled == 256) {
                    if (!stereo) {
                        for (uint32_t k = 0; k < chunk; k++) {
                            int32_t v = src[idx + k];
                            sb16_mix_stereo(&buf[(i + k) * 2u], v, v);
                        }
                    } else {
                        for (uint32_t k = 0; k < chunk; k++) {
                            size_t si = (idx + k) * 2u;
                            sb16_mix_stereo(&buf[(i + k) * 2u], src[si], src[si + 1u]);
                        }
                    }
                } else if (vol_scaled > 0) {
                    if (!stereo) {
                        for (uint32_t k = 0; k < chunk; k++) {
                            int32_t v = ((int32_t)src[idx + k] * vol_scaled) >> 8;
                            sb16_mix_stereo(&buf[(i + k) * 2u], v, v);
                        }
                    } else {
                        for (uint32_t k = 0; k < chunk; k++) {
                            size_t si = (idx + k) * 2u;
                            int32_t l = ((int32_t)src[si] * vol_scaled) >> 8;
                            int32_t r = ((int32_t)src[si + 1u] * vol_scaled) >> 8;
                            sb16_mix_stereo(&buf[(i + k) * 2u], l, r);
                        }
                    }
                }

                *pos_16 += (uint64_t)chunk << 16;
                i += chunk;
                if (idx + chunk >= total_samples) {
                    if (s->looping && total_samples > 0) {
                        *pos_16 = 0;
                    } else {
                        s->is_playing = false;
                        break;
                    }
                }
            }
            return;
        }

        for (uint32_t i = 0; i < block_samples; i++) {
            size_t idx = 0;
            if (!sb16_fetch_sample_idx(s, pos_16, total_samples, &idx)) break;

            int32_t l = stereo ? src[idx * 2u + 0] : src[idx];
            int32_t r = stereo ? src[idx * 2u + 1] : l;
            if (vol_scaled < 256) {
                l = (l * vol_scaled) >> 8;
                r = (r * vol_scaled) >> 8;
            }
            sb16_mix_stereo(&buf[i * 2u], l, r);
            *pos_16 += 65536ull;
        }
        return;
    }

    /* Generic path for arbitrary formats and fixed-point resampling */
    size_t limit_samples = (s->ring_size > 0) ? s->ring_size : total_samples;
    size_t ch = (size_t)s->channels;
    if (s->format != AUDIO_FORMAT_F32) {
        for (uint32_t i = 0; i < block_samples; i++) {
            size_t idx = 0;
            if (!sb16_fetch_sample_idx(s, pos_16, total_samples, &idx)) break;

            int32_t left = 0, right = 0;
            if (ch == 1) {
                left = read_sample_s16(stream_data, s->format, idx, limit_samples);
                right = left;
            } else {
                left = read_sample_s16(stream_data, s->format, idx * ch + 0, limit_samples * ch);
                right = read_sample_s16(stream_data, s->format, idx * ch + 1, limit_samples * ch);
            }
            if (vol_scaled < 256) {
                left = (left * vol_scaled) >> 8;
                right = (right * vol_scaled) >> 8;
            }
            sb16_mix_stereo(&buf[i * 2u], left, right);
            *pos_16 += step_16;
        }
        return;
    }

    float vol = s->volume;
    for (uint32_t i = 0; i < block_samples; i++) {
        size_t idx = 0;
        if (!sb16_fetch_sample_idx(s, pos_16, total_samples, &idx)) break;

        float left = 0.0f, right = 0.0f;
        if (ch == 1) {
            left = read_sample_normalized(stream_data, s->format, idx, limit_samples);
            right = left;
        } else {
            left = read_sample_normalized(stream_data, s->format, idx * ch + 0, limit_samples * ch);
            right = read_sample_normalized(stream_data, s->format, idx * ch + 1, limit_samples * ch);
        }

        sb16_mix_stereo(&buf[i * 2u],
                        (int32_t)(left * vol * 32767.0f),
                        (int32_t)(right * vol * 32767.0f));
        *pos_16 += step_16;
    }
}

/* Mix active streams into half-buffer index 0 or 1. Returns number of contributing streams. */
static int sb16_mix_half(int half_idx) {
    uint32_t phys_addr = SB16_DMA_BUFFER_ADDR + (half_idx * SB16_HALF_BYTES);
    int16_t *buf = (int16_t *)phys_addr;
    memset(buf, 0, SB16_HALF_BYTES);

    uint32_t block_samples = SB16_HALF_SAMPLES;
    uint32_t rate = SB16_SAMPLE_RATE;
    int active_streams = 0;

    for (audio_stream_t *s = g_stream_list; s; s = s->next) {
        if (!s->is_playing || s->data_size == 0) continue;

        size_t b_per_s = get_bytes_per_sample(s->format);
        size_t total_samples = s->data_size / (b_per_s * (size_t)s->channels);
        if (total_samples == 0) continue;

        address_space_t *target_space = NULL;
        const uint8_t *stream_data = audio_resolve_stream_data(s, &target_space);
        if (!stream_data) continue;

        active_streams++;
        s_sb16_mix_ctx.s = s;
        s_sb16_mix_ctx.stream_data = stream_data;
        s_sb16_mix_ctx.buf = buf;
        s_sb16_mix_ctx.total_samples = total_samples;
        s_sb16_mix_ctx.block_samples = block_samples;
        s_sb16_mix_ctx.rate = rate;
        s_sb16_mix_ctx.step_16 = (((uint64_t)s->sample_rate) << 16) / (uint64_t)rate;
        if (s_sb16_mix_ctx.step_16 == 0) s_sb16_mix_ctx.step_16 = 1;
        s_sb16_mix_ctx.vol_scaled = audio_volume_to_scale256(s->volume);

        if (target_space) {
            if (!s_audio_tramp_stack_top) {
                s_audio_tramp_stack_top = address_space_get_audio_tramp_stack_top();
            }
            address_space_call_on_stack(target_space, sb16_mix_stream_tramp,
                                        &s_sb16_mix_ctx,
                                        s_audio_tramp_stack_top);
        } else {
            sb16_mix_stream_tramp(&s_sb16_mix_ctx);
        }
    }

    return active_streams;
}

static void sb16_start(void) {
    if (g_active_backend != AUDIO_BACKEND_SB16) return;
    if (s_sb16_running) return;

    /* Pre-fill both halves of the circular DMA buffer ahead of time */
    sb16_mix_half(0);
    sb16_mix_half(1);

    /* Program DMA Channel 5 for Auto-Initialize Read */
    uint32_t phys_addr = SB16_DMA_BUFFER_ADDR;
    uint32_t word_addr = phys_addr >> 1;
    uint16_t word_count = (uint16_t)(SB16_TOTAL_WORDS - 1);

    outb(0xD4, 0x05); /* Mask Ch 5 */
    outb(0xD8, 0x00); /* Clear flip-flop */
    outb(0xD6, 0x59); /* Single-mode, Auto-init, Read transfer (0x40 | 0x10 | 0x08 | 0x01) */
    outb(0xC4, (uint8_t)(word_addr & 0xFF));
    outb(0xC4, (uint8_t)((word_addr >> 8) & 0xFF));
    outb(0x8B, (uint8_t)((phys_addr >> 16) & 0xFE));
    outb(0xC6, (uint8_t)(word_count & 0xFF));
    outb(0xC6, (uint8_t)((word_count >> 8) & 0xFF));
    outb(0xD4, 0x01); /* Unmask Ch 5 */

    /* Set DSP playback rate */
    sb16_dsp_write(0x41);
    sb16_dsp_write((uint8_t)(SB16_SAMPLE_RATE >> 8));
    sb16_dsp_write((uint8_t)(SB16_SAMPLE_RATE & 0xFF));

    /* Acknowledge any stale 16-bit interrupt before starting auto-init transfer */
    inb(SB16_ACK_16BIT);

    /* Start DSP 16-bit auto-initialize signed stereo playback. */
    sb16_dsp_write(0xB6); /* 16-bit auto-initialize D/A */
    sb16_dsp_write(0x30); /* 16-bit signed stereo */
    uint16_t word_len = (uint16_t)(SB16_HALF_WORDS - 1);
    sb16_dsp_write((uint8_t)(word_len & 0xFF));
    sb16_dsp_write((uint8_t)((word_len >> 8) & 0xFF));

    s_sb16_running = true;
    s_last_filled_half = 1; /* Both halves pre-filled; hardware starts at 0, next to fill is 0 */
    s_silence_halves = 0;
    s_last_half_swap_ms = timer_millis();
    s_last_sb16_poll_ms = s_last_half_swap_ms;
    s_prev_dma_words_left = sb16_get_dma_words_left();
    s_dma_advancing = false;
}

typedef struct {
    audio_stream_t *s;
    const uint8_t *stream_data;
    size_t total_samples;
    uint32_t test_pts;
    uint32_t pt_stride;
    double step;
    float prev_val;
    float peak_amp;
    int zero_crossings;
} pcspk_sample_ctx_t;

static pcspk_sample_ctx_t s_pcspk_ctx;

static void pcspk_sample_tramp(void *raw_ctx) {
    pcspk_sample_ctx_t *c = (pcspk_sample_ctx_t *)raw_ctx;
    size_t ch = (size_t)c->s->channels;
    size_t limit_samples = (c->s->ring_size > 0) ? c->s->ring_size : c->total_samples;
    for (uint32_t pt = 0; pt < c->test_pts; pt++) {
        size_t sample_offset = (size_t)(c->s->fractional_pos + (double)(pt * c->pt_stride) * c->step);
        if (c->s->ring_size > 0) {
            if (sample_offset >= c->s->buffered_samples) break;
            if (c->total_samples > 0 && sample_offset >= c->total_samples && c->s->is_eof) break;
            sample_offset %= c->s->ring_size;
        } else if (sample_offset >= c->total_samples) {
            break;
        }

        float val = read_sample_normalized(c->stream_data, c->s->format, sample_offset * ch, limit_samples * ch) * c->s->volume;
        float abs_val = (val >= 0.0f) ? val : -val;
        if (abs_val > c->peak_amp) c->peak_amp = abs_val;

        if (pt > 0) {
            if ((c->prev_val < 0.0f && val >= 0.0f) || (c->prev_val >= 0.0f && val < 0.0f)) {
                c->zero_crossings++;
            }
        }
        c->prev_val = val;
    }
}

void audio_poll(void) {
    if (g_active_backend == AUDIO_BACKEND_NONE) {
        audio_init();
    }

    /* Non-blocking atomic guard for ISR re-entrancy */
    if (__sync_lock_test_and_set(&s_audio_lock, 1)) {
        return;
    }

    if (g_active_backend == AUDIO_BACKEND_SB16) {
        if (!s_sb16_running) {
            bool any_playing = false;
            for (audio_stream_t *s = g_stream_list; s; s = s->next) {
                if (s->is_playing && s->data_size > 0) {
                    any_playing = true;
                    break;
                }
            }
            if (any_playing) {
                sb16_start();
            }
            __sync_lock_release(&s_audio_lock);
            return;
        }

        uint32_t now = timer_millis();
        if (s_last_sb16_poll_ms != 0 && (uint32_t)(now - s_last_sb16_poll_ms) < 6u) {
            __sync_lock_release(&s_audio_lock);
            return;
        }
        s_last_sb16_poll_ms = now;

        bool any_playing = false;
        for (audio_stream_t *s = g_stream_list; s; s = s->next) {
            if (s->is_playing && s->data_size > 0) {
                any_playing = true;
                break;
            }
        }

        /* SB16 is actively running in hardware Auto-Initialize DMA mode */
        uint16_t words_left = sb16_get_dma_words_left();
        if (words_left != s_prev_dma_words_left) {
            s_dma_advancing = true;
            s_prev_dma_words_left = words_left;
        }

        /* Mixer register 0x82 bit 1 reliably indicates 16-bit DMA IRQ pending */
        bool irq_16_pending = (sb16_read_irq_status() & 0x02) != 0;
        if (irq_16_pending) {
            inb(SB16_ACK_16BIT);
        }

        int current_half = (words_left >= SB16_HALF_WORDS) ? 0 : 1;
        int safe_half = 1 - current_half;

        uint32_t half_ms = (SB16_HALF_SAMPLES * 1000u) / SB16_SAMPLE_RATE;

        int half_to_refill = -1;
        if (s_dma_advancing) {
            /* Only refill the half hardware is NOT actively reading */
            if (s_last_filled_half != safe_half) {
                half_to_refill = safe_half;
            }
        } else {
            /* DMA word counter has not moved yet: allow legitimate 16-bit IRQ once per
             * half-period or fallback timer advance for headless dummy backends. */
            if (irq_16_pending && (now - s_last_half_swap_ms >= (half_ms / 2u))) {
                half_to_refill = (s_last_filled_half == 1) ? 0 : 1;
            } else if (s_last_half_swap_ms != 0 && (now - s_last_half_swap_ms >= half_ms)) {
                half_to_refill = (s_last_filled_half == 1) ? 0 : 1;
            }
        }

        if (half_to_refill >= 0) {
            int active = sb16_mix_half(half_to_refill);
            s_last_filled_half = half_to_refill;
            s_last_half_swap_ms = now;
            s_total_refills++;
            if (active == 0) s_silence_halves++;
            else s_silence_halves = 0;
        }

        /* Allow both halves plus one flush cycle (~279ms) of silence before stopping DMA */
        if (!any_playing && s_silence_halves >= 3) {
            sb16_stop();
        }

        __sync_lock_release(&s_audio_lock);
        return;
    }

    if (g_active_backend == AUDIO_BACKEND_PC_SPEAKER) {
        static uint32_t s_last_tick_ms = 0;
        uint32_t now = timer_millis();
        if (s_last_tick_ms == 0) {
            s_last_tick_ms = now;
            __sync_lock_release(&s_audio_lock);
            return;
        }
        uint32_t elapsed_ms = now - s_last_tick_ms;
        if (elapsed_ms == 0) {
            __sync_lock_release(&s_audio_lock);
            return;
        }
        if (elapsed_ms > 100) {
            elapsed_ms = 100;
        }
        s_last_tick_ms = now;

        uint32_t num_mixer_samples = (22050u * elapsed_ms) / 1000u;
        if (num_mixer_samples == 0) num_mixer_samples = 1;

        int active_count = 0;
        int zero_crossings = 0;
        float prev_val = 0.0f;
        float peak_amp = 0.0f;

        uint32_t test_pts = num_mixer_samples;
        if (test_pts > 64) test_pts = 64;
        uint32_t pt_stride = num_mixer_samples / test_pts;
        if (pt_stride == 0) pt_stride = 1;

        for (audio_stream_t *s = g_stream_list; s; s = s->next) {
            if (!s->is_playing || s->data_size == 0) continue;

            size_t b_per_s = get_bytes_per_sample(s->format);
            size_t total_samples = s->data_size / (b_per_s * (size_t)s->channels);
            if (total_samples == 0) continue;

            address_space_t *target_space = NULL;
            const uint8_t *stream_data = audio_resolve_stream_data(s, &target_space);
            if (!stream_data) continue;

            double step = (double)s->sample_rate / 22050.0;
            active_count++;

            s_pcspk_ctx.s = s;
            s_pcspk_ctx.stream_data = stream_data;
            s_pcspk_ctx.total_samples = total_samples;
            s_pcspk_ctx.test_pts = test_pts;
            s_pcspk_ctx.pt_stride = pt_stride;
            s_pcspk_ctx.step = step;
            s_pcspk_ctx.prev_val = prev_val;
            s_pcspk_ctx.peak_amp = peak_amp;
            s_pcspk_ctx.zero_crossings = zero_crossings;

            if (target_space) {
                if (!s_audio_tramp_stack_top) {
                    s_audio_tramp_stack_top = address_space_get_audio_tramp_stack_top();
                }
                address_space_call_on_stack(target_space, pcspk_sample_tramp,
                                            &s_pcspk_ctx,
                                            s_audio_tramp_stack_top);
            } else {
                pcspk_sample_tramp(&s_pcspk_ctx);
            }
            prev_val = s_pcspk_ctx.prev_val;
            peak_amp = s_pcspk_ctx.peak_amp;
            zero_crossings = s_pcspk_ctx.zero_crossings;

            s->fractional_pos += (double)num_mixer_samples * step;
            if ((size_t)s->fractional_pos >= total_samples) {
                if (s->looping) {
                    s->fractional_pos = 0.0;
                } else {
                    s->is_playing = false;
                }
            }
        }

        static uint32_t s_pcspk_last_freq = 0;
        static uint32_t s_pcspk_silence_ms = 0;

        if (active_count > 0 && peak_amp > 0.02f && zero_crossings > 0) {
            float effective_rate = 22050.0f / (float)pt_stride;
            float freq = ((float)zero_crossings * effective_rate) / (2.0f * (float)test_pts);
            if (freq < 50.0f) freq = 50.0f;
            if (freq > 4500.0f) freq = 4500.0f;
            uint32_t target_freq = (uint32_t)(freq + 0.5f);
            if (s_pcspk_last_freq == 0) {
                s_pcspk_last_freq = target_freq;
            } else {
                /* Smooth frequency changes to prevent jarring jumps */
                s_pcspk_last_freq = (s_pcspk_last_freq * 3 + target_freq) / 4;
            }
            sound_play(s_pcspk_last_freq);
            s_pcspk_silence_ms = 0;
        } else if (active_count > 0) {
            /* Active streams exist, but momentarily zero amplitude or no zero-crossings */
            s_pcspk_silence_ms += elapsed_ms;
            /* Only turn off speaker if silence persists for more than 100ms */
            if (s_pcspk_silence_ms >= 100) {
                sound_stop();
                s_pcspk_last_freq = 0;
            }
        } else {
            sound_stop();
            s_pcspk_last_freq = 0;
            s_pcspk_silence_ms = 0;
        }
    }

    __sync_lock_release(&s_audio_lock);
}

void audio_tick(void) {
    audio_poll();
}
