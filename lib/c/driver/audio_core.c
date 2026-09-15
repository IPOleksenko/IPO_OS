#include <driver/audio_core.h>
#include <driver/sound.h>
#include <system/pit.h>
#include <system/timer.h>
#include <memory/kmalloc.h>
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
#define SB16_BLOCK_SAMPLES   3308 /* ~150 ms at 22050 Hz stereo for smooth multitasking */
#define SB16_SAMPLE_RATE     22050

static int g_active_backend = AUDIO_BACKEND_NONE;
static audio_stream_t *g_stream_list = NULL;

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

typedef struct {
    uint32_t next_fill_ms;
    uint32_t buf_idx;
} sb16_hw_state_t;
#define SB16_HW_STATE ((volatile sb16_hw_state_t *)0x5FE000)

void sb16_stop(void) {
    if (g_active_backend == AUDIO_BACKEND_SB16) {
        sb16_dsp_write(0xD5); // Pause 16-bit DMA
        outb(0xD4, 0x05);      // Mask DMA Ch 5
        inb(SB16_ACK_16BIT);   // Acknowledge 16-bit IRQ
        SB16_HW_STATE->next_fill_ms = 0;
    }
}

void audio_stop_all(void) {
    for (audio_stream_t *s = g_stream_list; s; s = s->next) {
        s->is_playing = false;
        s->cursor = 0;
        s->fractional_pos = 0.0;
    }
    sb16_stop();
}

bool audio_init(void) {
    /* 1. Try Sound Blaster 16 on standard I/O port 0x220 */
    if (sb16_reset()) {
        g_active_backend = AUDIO_BACKEND_SB16;
        /* Set mixer volumes to maximum for high quality voice playback */
        outb(SB16_MIXER_ADDR, 0x22); outb(SB16_MIXER_DATA, 0xFF); // Master volume
        outb(SB16_MIXER_ADDR, 0x04); outb(SB16_MIXER_DATA, 0xFF); // Voice / DAC volume
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
        case AUDIO_BACKEND_SB16:       return "Sound Blaster 16 (DSP/DMA)";
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

    /* Add to active streams linked list */
    stream->next = g_stream_list;
    g_stream_list = stream;

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

    /* Add to active streams linked list */
    stream->next = g_stream_list;
    g_stream_list = stream;

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
    kfree(stream);
}

void audio_stream_play(audio_stream_t *stream) {
    if (stream) stream->is_playing = true;
}

void audio_stream_pause(audio_stream_t *stream) {
    if (stream) {
        stream->is_playing = false;
        bool any_playing = false;
        for (audio_stream_t *s = g_stream_list; s; s = s->next) {
            if (s->is_playing) { any_playing = true; break; }
        }
        if (!any_playing) {
            sb16_stop();
            sound_stop();
        }
    }
}

void audio_stream_stop(audio_stream_t *stream) {
    if (stream) {
        stream->is_playing = false;
        stream->cursor = 0;
        stream->fractional_pos = 0.0;
        bool any_playing = false;
        for (audio_stream_t *s = g_stream_list; s; s = s->next) {
            if (s != stream && s->is_playing) { any_playing = true; break; }
        }
        if (!any_playing) {
            sb16_stop();
            sound_stop();
        }
    }
}

void audio_stream_set_volume(audio_stream_t *stream, float volume) {
    if (stream) {
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 1.0f) volume = 1.0f;
        stream->volume = volume;
    }
}

/* Reads a sample normalized to [-1.0f, +1.0f] from arbitrary bit depths */
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
            if (val & 0x800000) val |= 0xFF000000; /* sign-extend */
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

/* Size of single sample in bytes */
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

void audio_tick(void) {
    if (g_active_backend == AUDIO_BACKEND_NONE) {
        audio_init();
    }

    if (g_active_backend == AUDIO_BACKEND_SB16) {
        uint32_t now = timer_millis();

        int playing_count = 0;
        for (audio_stream_t *s = g_stream_list; s; s = s->next) {
            if (s->is_playing && s->data_size > 0) playing_count++;
        }

        if (playing_count == 0) {
            if (SB16_HW_STATE->next_fill_ms != 0 && now >= SB16_HW_STATE->next_fill_ms) {
                sb16_stop();
                SB16_HW_STATE->next_fill_ms = 0;
            }
            return;
        }

        /* Wait until previous DMA block has completed playing */
        if (SB16_HW_STATE->next_fill_ms != 0 && now < SB16_HW_STATE->next_fill_ms) {
            return;
        }

        /* Acknowledge 16-bit IRQ */
        inb(SB16_ACK_16BIT);

        /* Alternate between two physical memory buffers below 16MB */
        uint32_t phys_addr = (SB16_HW_STATE->buf_idx == 0) ? SB16_DMA_BUFFER_ADDR : (SB16_DMA_BUFFER_ADDR + 0x10000);
        SB16_HW_STATE->buf_idx ^= 1;
        int16_t *buf = (int16_t *)phys_addr;

        uint32_t block_samples = SB16_BLOCK_SAMPLES;
        uint32_t rate = SB16_SAMPLE_RATE;

        memset(buf, 0, block_samples * 4);

        /* Mix all active streams into buffer */
        for (audio_stream_t *s = g_stream_list; s; s = s->next) {
            if (!s->is_playing || s->data_size == 0) continue;

            size_t b_per_s = get_bytes_per_sample(s->format);
            size_t total_samples = s->data_size / (b_per_s * (size_t)s->channels);
            if (total_samples == 0) continue;

            double step = (double)s->sample_rate / (double)rate;
            float vol = s->volume;

            /* Fast-path 1: S16 stereo 22050 Hz (standard MP3 and CD quality) */
            if (s->format == AUDIO_FORMAT_S16 && s->channels == 2 && s->sample_rate == rate) {
                const int16_t *src = (const int16_t *)s->data;
                int vol_scaled = (int)(vol * 256.0f);
                for (uint32_t i = 0; i < block_samples; i++) {
                    size_t s_idx = (size_t)s->fractional_pos;
                    if (s->ring_size > 0) {
                        /* Circular streaming ring buffer */
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
                                /* Temporary underrun: let silence play, don't kill stream */
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

                    /* Linear buffer path */
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
                continue;
            }

            /* Fast-path 2: S16 mono 22050 Hz */
            if (s->format == AUDIO_FORMAT_S16 && s->channels == 1 && s->sample_rate == rate) {
                const int16_t *src = (const int16_t *)s->data;
                int vol_scaled = (int)(vol * 256.0f);
                for (uint32_t i = 0; i < block_samples; i++) {
                    size_t s_idx = (size_t)s->fractional_pos;
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
                    if (vol_scaled < 256) {
                        val = (val * vol_scaled) >> 8;
                    }
                    int32_t l_samp = (int32_t)buf[i * 2 + 0] + val;
                    int32_t r_samp = (int32_t)buf[i * 2 + 1] + val;
                    if (l_samp > 32767) l_samp = 32767; else if (l_samp < -32768) l_samp = -32768;
                    if (r_samp > 32767) r_samp = 32767; else if (r_samp < -32768) r_samp = -32768;
                    buf[i * 2 + 0] = (int16_t)l_samp;
                    buf[i * 2 + 1] = (int16_t)r_samp;
                    s->fractional_pos += 1.0;
                }
                continue;
            }

            /* Generic path for arbitrary formats and resampling */
            for (uint32_t i = 0; i < block_samples; i++) {
                size_t s_idx = (size_t)s->fractional_pos;
                if (s_idx >= total_samples) {
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
        }

        /* Program DMA Channel 5 */
        uint32_t total_bytes = block_samples * 4;
        uint32_t word_addr = phys_addr >> 1;
        uint16_t word_count = (total_bytes >> 1) - 1;

        outb(0xD4, 0x05); // Mask Ch 5
        outb(0xD8, 0x00); // Clear FF
        outb(0xD6, 0x49); // Single-cycle read
        outb(0xC4, (uint8_t)(word_addr & 0xFF));
        outb(0xC4, (uint8_t)((word_addr >> 8) & 0xFF));
        outb(0x8B, (uint8_t)((phys_addr >> 16) & 0xFE));
        outb(0xC6, (uint8_t)(word_count & 0xFF));
        outb(0xC6, (uint8_t)((word_count >> 8) & 0xFF));
        outb(0xD4, 0x01); // Unmask Ch 5

        /* Set DSP playback rate */
        sb16_dsp_write(0x41);
        sb16_dsp_write((uint8_t)(rate >> 8));
        sb16_dsp_write((uint8_t)(rate & 0xFF));

        /* Start DSP 16-bit signed stereo playback */
        sb16_dsp_write(0xB0); // 16-bit single-cycle
        sb16_dsp_write(0x30); // signed stereo
        uint16_t sample_cnt = block_samples - 1;
        sb16_dsp_write((uint8_t)(sample_cnt & 0xFF));
        sb16_dsp_write((uint8_t)((sample_cnt >> 8) & 0xFF));

        SB16_HW_STATE->next_fill_ms = now + (block_samples * 1000) / rate;
        return;
    }

    static uint32_t s_last_tick_ms = 0;
    uint32_t now = timer_millis();
    if (s_last_tick_ms == 0) {
        s_last_tick_ms = now;
        return;
    }
    uint32_t elapsed_ms = now - s_last_tick_ms;
    if (elapsed_ms == 0) {
        return;
    }
    if (elapsed_ms > 100) {
        elapsed_ms = 100; /* Cap to prevent huge leap on freeze/load */
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

        /* Analyze frequency & zero-crossings over the elapsed interval */
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

        /* Advance position by elapsed real-time samples */
        s->fractional_pos += (double)num_mixer_samples * step;
        if ((size_t)s->fractional_pos >= total_samples) {
            if (s->looping) {
                s->fractional_pos = 0.0;
            } else {
                s->is_playing = false;
            }
        }
    }

    if (active_count > 0 && peak_amp > 0.03f && zero_crossings > 0) {
        if (g_active_backend == AUDIO_BACKEND_PC_SPEAKER) {
            float effective_rate = 22050.0f / (float)pt_stride;
            float freq = ((float)zero_crossings * effective_rate) / (2.0f * (float)test_pts);
            if (freq < 50.0f) freq = 50.0f;
            if (freq > 4500.0f) freq = 4500.0f;
            sound_play((uint32_t)(freq + 0.5f));
        }
    } else {
        if (g_active_backend == AUDIO_BACKEND_PC_SPEAKER) {
            sound_stop();
        }
    }
}

