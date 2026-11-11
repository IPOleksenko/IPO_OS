#include <driver/audio_core.h>
#include <driver/sound.h>
#include <system/pit.h>
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

static inline uint32_t read_cr3_safe(void) {
    uint32_t val;
    __asm__ volatile("movl %%cr3, %0" : "=r"(val));
    return val;
}

static inline void write_cr3_safe(uint32_t val) {
    __asm__ volatile("movl %0, %%cr3" : : "r"(val) : "memory");
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
    outb(0xD8, 0x00); /* Clear flip-flop */
    uint8_t lo = inb(0xC6);
    uint8_t hi = inb(0xC6);
    return (uint16_t)lo | ((uint16_t)hi << 8);
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
    }
}

void audio_stop_all(void) {
    audio_lock();
    for (audio_stream_t *s = g_stream_list; s; s = s->next) {
        s->is_playing = false;
        s->cursor = 0;
        s->fractional_pos = 0.0;
    }
    sb16_stop();
    sound_stop();
    audio_unlock();
}

bool audio_init(void) {
    if (g_active_backend != AUDIO_BACKEND_NONE) {
        return true;
    }

    /* 1. Try Sound Blaster 16 on standard I/O port 0x220 */
    if (sb16_reset()) {
        g_active_backend = AUDIO_BACKEND_SB16;
        /* Set mixer volumes to maximum for high quality audio output */
        outb(SB16_MIXER_ADDR, 0x22); outb(SB16_MIXER_DATA, 0xFF); /* Master volume */
        outb(SB16_MIXER_ADDR, 0x04); outb(SB16_MIXER_DATA, 0xFF); /* Voice / DAC volume */
        printf("[audio] Sound Blaster 16 initialized (Continuous Auto-Init DMA ready).\n");
        return true;
    }

    /* 2. Fallback to PC Speaker PWM */
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

audio_stream_t *audio_stream_create(int format, int channels, uint32_t sample_rate, const void *data, size_t size) {
    if (!data || size == 0 || channels <= 0 || sample_rate == 0) return NULL;

    audio_stream_t *stream = kmalloc(sizeof(audio_stream_t));
    if (!stream) return NULL;
    memset(stream, 0, sizeof(audio_stream_t));

    stream->format         = format;
    stream->channels       = channels;
    stream->sample_rate    = sample_rate;
    stream->data           = (const uint8_t *)data;
    stream->data_size      = size;
    stream->cursor         = 0;
    stream->is_playing     = false;
    stream->looping        = false;
    stream->volume         = 1.0f;
    stream->fractional_pos = 0.0;
    stream->ring_size      = 0;
    stream->buffered_samples = 0;
    stream->is_eof         = false;
    stream->owner          = (void *)process_get_current();

    audio_lock();
    stream->next = g_stream_list;
    g_stream_list = stream;
    audio_unlock();

    return stream;
}

audio_stream_t *audio_stream_create_ring(int format, int channels, uint32_t sample_rate,
                                         const void *ring_buf, size_t ring_size_samples,
                                         size_t total_expected_samples) {
    if (!ring_buf || ring_size_samples == 0 || channels <= 0 || sample_rate == 0) return NULL;

    audio_stream_t *stream = kmalloc(sizeof(audio_stream_t));
    if (!stream) return NULL;
    memset(stream, 0, sizeof(audio_stream_t));

    stream->format           = format;
    stream->channels         = channels;
    stream->sample_rate      = sample_rate;
    stream->data             = (const uint8_t *)ring_buf;
    stream->data_size        = total_expected_samples * 2 * (size_t)channels;
    stream->cursor           = 0;
    stream->is_playing       = false;
    stream->looping          = false;
    stream->volume           = 1.0f;
    stream->fractional_pos   = 0.0;
    stream->ring_size        = ring_size_samples;
    stream->buffered_samples = 0;
    stream->is_eof           = false;
    stream->owner            = (void *)process_get_current();

    audio_lock();
    stream->next = g_stream_list;
    g_stream_list = stream;
    audio_unlock();

    return stream;
}

void audio_stream_update_buffered(audio_stream_t *stream, size_t buffered_samples, bool is_eof) {
    if (stream) {
        stream->buffered_samples = buffered_samples;
        stream->is_eof = is_eof;
    }
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
        sb16_stop();
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
        stream->is_playing = false;
        audio_lock();
        bool any_playing = false;
        for (audio_stream_t *s = g_stream_list; s; s = s->next) {
            if (s->is_playing) { any_playing = true; break; }
        }
        if (!any_playing) {
            sb16_stop();
            sound_stop();
        }
        audio_unlock();
    }
}

void audio_stream_stop(audio_stream_t *stream) {
    if (stream) {
        stream->is_playing = false;
        stream->cursor = 0;
        stream->fractional_pos = 0.0;
        audio_lock();
        bool any_playing = false;
        for (audio_stream_t *s = g_stream_list; s; s = s->next) {
            if (s != stream && s->is_playing) { any_playing = true; break; }
        }
        if (!any_playing) {
            sb16_stop();
            sound_stop();
        }
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
}

double audio_stream_get_progress(audio_stream_t *stream) {
    if (!stream || stream->data_size == 0 || stream->channels <= 0) return 0.0;
    size_t b_per_s = get_bytes_per_sample(stream->format);
    size_t total_samples = stream->data_size / (b_per_s * (size_t)stream->channels);
    if (total_samples == 0) return 0.0;
    double p = stream->fractional_pos / (double)total_samples;
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;
    return p;
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

        active_streams++;
        double step = (double)s->sample_rate / (double)rate;
        float vol = s->volume;
        int vol_scaled = (int)(vol * 256.0f);

        uint32_t orig_cr3 = 0;
        bool switched_as = false;
        if (s->owner && process_is_valid((process_t *)s->owner)) {
            process_t *owner_proc = (process_t *)s->owner;
            if (owner_proc->address_space) {
                orig_cr3 = read_cr3_safe();
                if (address_space_activate(owner_proc->address_space)) {
                    switched_as = true;
                }
            }
        }

        /* Fast-path 1: S16 stereo 22050 Hz (standard WAV/MP3) */
        if (s->format == AUDIO_FORMAT_S16 && s->channels == 2 && s->sample_rate == rate) {
            const int16_t *src = (const int16_t *)s->data;
            for (uint32_t i = 0; i < block_samples; i++) {
                size_t s_idx = (size_t)s->fractional_pos;
                if (s->ring_size > 0) {
                    if (s_idx >= s->buffered_samples) {
                        if (s->is_eof) {
                            if (s->looping && total_samples > 0) {
                                s->fractional_pos = 0.0;
                                s_idx = 0;
                            } else {
                                s->is_playing = false;
                                break;
                            }
                        } else {
                            break;
                        }
                    }
                    if (total_samples > 0 && s_idx >= total_samples && s->is_eof) {
                        if (s->looping) {
                            s->fractional_pos = 0.0;
                            s_idx = 0;
                        } else {
                            s->is_playing = false;
                            break;
                        }
                    }
                    size_t r_idx = s_idx % s->ring_size;
                    int32_t l = src[r_idx * 2 + 0];
                    int32_t r = src[r_idx * 2 + 1];
                    if (vol_scaled < 256) {
                        l = (l * vol_scaled) >> 8;
                        r = (r * vol_scaled) >> 8;
                    }
                    int32_t l_samp = (int32_t)buf[i * 2 + 0] + l;
                    int32_t r_samp = (int32_t)buf[i * 2 + 1] + r;
                    if (l_samp > 32767) l_samp = 32767; else if (l_samp < -32768) l_samp = -32768;
                    if (r_samp > 32767) r_samp = 32767; else if (r_samp < -32768) r_samp = -32768;
                    buf[i * 2 + 0] = (int16_t)l_samp;
                    buf[i * 2 + 1] = (int16_t)r_samp;
                    s->fractional_pos += 1.0;
                    continue;
                }

                if (s_idx >= total_samples) {
                    if (s->looping) {
                        s->fractional_pos = 0.0;
                        s_idx = 0;
                    } else {
                        s->is_playing = false;
                        break;
                    }
                }
                int32_t l = src[s_idx * 2 + 0];
                int32_t r = src[s_idx * 2 + 1];
                if (vol_scaled < 256) {
                    l = (l * vol_scaled) >> 8;
                    r = (r * vol_scaled) >> 8;
                }
                int32_t l_samp = (int32_t)buf[i * 2 + 0] + l;
                int32_t r_samp = (int32_t)buf[i * 2 + 1] + r;
                if (l_samp > 32767) l_samp = 32767; else if (l_samp < -32768) l_samp = -32768;
                if (r_samp > 32767) r_samp = 32767; else if (r_samp < -32768) r_samp = -32768;
                buf[i * 2 + 0] = (int16_t)l_samp;
                buf[i * 2 + 1] = (int16_t)r_samp;
                s->fractional_pos += 1.0;
            }
            if (switched_as) write_cr3_safe(orig_cr3);
            continue;
        }

        /* Fast-path 2: S16 mono 22050 Hz */
        if (s->format == AUDIO_FORMAT_S16 && s->channels == 1 && s->sample_rate == rate) {
            const int16_t *src = (const int16_t *)s->data;
            for (uint32_t i = 0; i < block_samples; i++) {
                size_t s_idx = (size_t)s->fractional_pos;
                if (s->ring_size > 0) {
                    if (s_idx >= s->buffered_samples) {
                        if (s->is_eof) {
                            if (s->looping && total_samples > 0) {
                                s->fractional_pos = 0.0;
                                s_idx = 0;
                            } else {
                                s->is_playing = false;
                                break;
                            }
                        } else {
                            break;
                        }
                    }
                    if (total_samples > 0 && s_idx >= total_samples && s->is_eof) {
                        if (s->looping) {
                            s->fractional_pos = 0.0;
                            s_idx = 0;
                        } else {
                            s->is_playing = false;
                            break;
                        }
                    }
                    size_t r_idx = s_idx % s->ring_size;
                    int32_t val = src[r_idx];
                    if (vol_scaled < 256) val = (val * vol_scaled) >> 8;
                    int32_t l_samp = (int32_t)buf[i * 2 + 0] + val;
                    int32_t r_samp = (int32_t)buf[i * 2 + 1] + val;
                    if (l_samp > 32767) l_samp = 32767; else if (l_samp < -32768) l_samp = -32768;
                    if (r_samp > 32767) r_samp = 32767; else if (r_samp < -32768) r_samp = -32768;
                    buf[i * 2 + 0] = (int16_t)l_samp;
                    buf[i * 2 + 1] = (int16_t)r_samp;
                    s->fractional_pos += 1.0;
                    continue;
                }

                if (s_idx >= total_samples) {
                    if (s->looping) {
                        s->fractional_pos = 0.0;
                        s_idx = 0;
                    } else {
                        s->is_playing = false;
                        break;
                    }
                }
                int32_t val = src[s_idx];
                if (vol_scaled < 256) val = (val * vol_scaled) >> 8;
                int32_t l_samp = (int32_t)buf[i * 2 + 0] + val;
                int32_t r_samp = (int32_t)buf[i * 2 + 1] + val;
                if (l_samp > 32767) l_samp = 32767; else if (l_samp < -32768) l_samp = -32768;
                if (r_samp > 32767) r_samp = 32767; else if (r_samp < -32768) r_samp = -32768;
                buf[i * 2 + 0] = (int16_t)l_samp;
                buf[i * 2 + 1] = (int16_t)r_samp;
                s->fractional_pos += 1.0;
            }
            if (switched_as) write_cr3_safe(orig_cr3);
            continue;
        }

        /* Generic path for arbitrary formats and resampling */
        for (uint32_t i = 0; i < block_samples; i++) {
            size_t s_idx = (size_t)s->fractional_pos;
            if (s->ring_size > 0) {
                if (s_idx >= s->buffered_samples) {
                    if (s->is_eof) {
                        if (s->looping && total_samples > 0) {
                            s->fractional_pos = 0.0;
                            s_idx = 0;
                        } else {
                            s->is_playing = false;
                            break;
                        }
                    } else {
                        break;
                    }
                }
                s_idx = s_idx % s->ring_size;
            } else if (s_idx >= total_samples) {
                if (s->looping) {
                    s->fractional_pos = 0.0;
                    s_idx = 0;
                } else {
                    s->is_playing = false;
                    break;
                }
            }

            float left = 0.0f, right = 0.0f;
            if (s->channels == 1) {
                left = read_sample_normalized(s->data, s->format, s_idx, total_samples);
                right = left;
            } else {
                left = read_sample_normalized(s->data, s->format, s_idx * 2 + 0, total_samples * 2);
                right = read_sample_normalized(s->data, s->format, s_idx * 2 + 1, total_samples * 2);
            }

            int32_t l_samp = (int32_t)buf[i * 2 + 0] + (int32_t)(left * vol * 32767.0f);
            int32_t r_samp = (int32_t)buf[i * 2 + 1] + (int32_t)(right * vol * 32767.0f);

            if (l_samp > 32767) l_samp = 32767; else if (l_samp < -32768) l_samp = -32768;
            if (r_samp > 32767) r_samp = 32767; else if (r_samp < -32768) r_samp = -32768;

            buf[i * 2 + 0] = (int16_t)l_samp;
            buf[i * 2 + 1] = (int16_t)r_samp;

            s->fractional_pos += step;
        }
        if (switched_as) write_cr3_safe(orig_cr3);
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

    /* Start DSP 16-bit auto-initialize signed stereo playback */
    sb16_dsp_write(0xB6); /* 16-bit auto-initialize D/A */
    sb16_dsp_write(0x30); /* 16-bit signed stereo */
    uint16_t sample_cnt = (uint16_t)(SB16_HALF_SAMPLES - 1);
    sb16_dsp_write((uint8_t)(sample_cnt & 0xFF));
    sb16_dsp_write((uint8_t)((sample_cnt >> 8) & 0xFF));

    s_sb16_running = true;
    s_last_filled_half = 1; /* Both halves pre-filled; hardware starts at 0, next to fill is 0 */
    s_silence_halves = 0;
    s_last_half_swap_ms = timer_millis();
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
        bool any_playing = false;
        for (audio_stream_t *s = g_stream_list; s; s = s->next) {
            if (s->is_playing && s->data_size > 0) {
                any_playing = true;
                break;
            }
        }

        if (!s_sb16_running) {
            if (any_playing) {
                sb16_start();
            }
            __sync_lock_release(&s_audio_lock);
            return;
        }

        /* SB16 is actively running in hardware Auto-Initialize DMA mode */
        uint16_t words_left = sb16_get_dma_words_left();
        bool irq_pending = (inb(SB16_READ_STATUS) & 0x02) != 0;
        int current_half = (words_left >= SB16_HALF_WORDS) ? 0 : 1;

        uint32_t now = timer_millis();
        uint32_t half_ms = (SB16_HALF_SAMPLES * 1000u) / SB16_SAMPLE_RATE;

        bool need_refill_0 = false;
        bool need_refill_1 = false;

        if (irq_pending) {
            inb(SB16_ACK_16BIT);
            if (s_last_filled_half == 1) need_refill_0 = true;
            else need_refill_1 = true;
        } else if (current_half == 1 && s_last_filled_half != 0) {
            need_refill_0 = true;
        } else if (current_half == 0 && s_last_filled_half != 1) {
            need_refill_1 = true;
        } else if (s_last_half_swap_ms != 0 && (now - s_last_half_swap_ms >= half_ms)) {
            /* Fallback timer for headless QEMU where DMA counter doesn't decrement in dummy audiodev */
            if (s_last_filled_half == 1) need_refill_0 = true;
            else need_refill_1 = true;
        }

        if (need_refill_0) {
            inb(SB16_ACK_16BIT);
            int active = sb16_mix_half(0);
            s_last_filled_half = 0;
            s_last_half_swap_ms = now;
            s_total_refills++;
            if (active == 0) s_silence_halves++;
            else s_silence_halves = 0;
        } else if (need_refill_1) {
            inb(SB16_ACK_16BIT);
            int active = sb16_mix_half(1);
            s_last_filled_half = 1;
            s_last_half_swap_ms = now;
            s_total_refills++;
            if (active == 0) s_silence_halves++;
            else s_silence_halves = 0;
        }

        /* If all streams finished and silence has filled both halves, stop hardware cleanly */
        if (!any_playing && s_silence_halves >= 2) {
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

            double step = (double)s->sample_rate / 22050.0;
            active_count++;

            for (uint32_t pt = 0; pt < test_pts; pt++) {
                size_t sample_offset = (size_t)(s->fractional_pos + (double)(pt * pt_stride) * step);
                if (sample_offset >= total_samples) break;

                float val = read_sample_normalized(s->data, s->format, sample_offset * s->channels, total_samples * s->channels) * s->volume;
                float abs_val = (val >= 0.0f) ? val : -val;
                if (abs_val > peak_amp) peak_amp = abs_val;

                if (pt > 0) {
                    if ((prev_val < 0.0f && val >= 0.0f) || (prev_val >= 0.0f && val < 0.0f)) {
                        zero_crossings++;
                    }
                }
                prev_val = val;
            }

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
