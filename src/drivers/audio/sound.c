#include <driver/sound.h>
#include <driver/audio_core.h>
#include <system/pit.h>
#include <system/timer.h>
#include <memory/kmalloc.h>
#include <GL/gl_math.h>
#include <ioport.h>
#include <stdio.h>
#include <stddef.h>

/* Sound control port */
#define SOUND_CTRL_PORT 0x61
#define SOUND_CTRL_GATE 0x01
#define SOUND_CTRL_SPEAKER 0x02

static bool s_sound_initialized = false;

void sound_init(void) {
    if (s_sound_initialized) {
        return;
    }
    s_sound_initialized = true;
    pit_init(0);
    printf("[sound] PC speaker audio driver initialized.\n");
}

void sound_play(uint32_t frequency) {
    if (frequency == 0)
        return;
    
    /* Set PIT counter 2 to desired frequency */
    pit_set_frequency(2, frequency);
    
    /* Enable speaker */
    uint8_t control = inb(SOUND_CTRL_PORT);
    outb(SOUND_CTRL_PORT, control | SOUND_CTRL_GATE | SOUND_CTRL_SPEAKER);
}

void sound_stop(void) {
    /* Disable speaker */
    uint8_t control = inb(SOUND_CTRL_PORT);
    outb(SOUND_CTRL_PORT, control & ~(SOUND_CTRL_GATE | SOUND_CTRL_SPEAKER));
}

void sound_beep(uint32_t frequency, uint32_t duration) {
    sound_play(frequency);
    
    /* Delay in milliseconds (busy-wait loop) */
    volatile uint32_t delay = duration * 1000;
    while (delay--) {
        io_wait();
    }
    
    sound_stop();
}

void play_startup_sound(void) {
    typedef struct {
        uint16_t note;
        uint16_t duration;
    } note_duration_t;

    static const note_duration_t startup_sound[] = {
        {NOTE_C6, 150},
        {NOTE_E6, 150},
        {NOTE_G6, 150},
        {NOTE_C7, 350}
    };
    const size_t notes_count = sizeof(startup_sound) / sizeof(startup_sound[0]);

    uint32_t total_ms = 0;
    for (size_t i = 0; i < notes_count; i++) {
        total_ms += startup_sound[i].duration;
    }

    audio_stream_t *sb16_stream = NULL;
    int16_t *pcm_buf = NULL;

    if (audio_get_active_backend() == AUDIO_BACKEND_SB16) {
        uint32_t rate = 22050;
        uint32_t tail_ms = 200;
        size_t melody_samples = (rate * total_ms) / 1000;
        size_t silence_samples = (rate * tail_ms) / 1000;
        size_t total_samples = melody_samples + silence_samples;
        size_t total_bytes = total_samples * 4;

        pcm_buf = (int16_t *)kmalloc(total_bytes);
        if (pcm_buf) {
            size_t sample_pos = 0;
            float current_phase = 0.0f;

            for (size_t n = 0; n < notes_count; n++) {
                uint16_t freq = startup_sound[n].note;
                uint32_t note_ms = startup_sound[n].duration;
                size_t note_samples = (rate * note_ms) / 1000;
                float phase_inc = (6.2831853f * (float)freq) / (float)rate;

                for (size_t s = 0; s < note_samples && sample_pos < melody_samples; s++, sample_pos++) {
                    if (freq == NOTE_REST) {
                        pcm_buf[sample_pos * 2 + 0] = 0;
                        pcm_buf[sample_pos * 2 + 1] = 0;
                        continue;
                    }

                    float amp = 14000.0f;
                    if (sample_pos < 220) {
                        amp *= (float)sample_pos / 220.0f;
                    }
                    if (melody_samples > 440 && sample_pos > melody_samples - 440) {
                        amp *= (float)(melody_samples - sample_pos) / 440.0f;
                    }

                    float s1 = gl_sinf(current_phase);
                    float s2 = gl_sinf(current_phase * 2.0f) * 0.25f;
                    int32_t val = (int32_t)((s1 + s2) * amp);
                    if (val > 32767) val = 32767;
                    else if (val < -32768) val = -32768;

                    pcm_buf[sample_pos * 2 + 0] = (int16_t)val;
                    pcm_buf[sample_pos * 2 + 1] = (int16_t)val;

                    current_phase += phase_inc;
                    if (current_phase >= 6.2831853f) current_phase -= 6.2831853f;
                }
            }

            while (sample_pos < total_samples) {
                pcm_buf[sample_pos * 2 + 0] = 0;
                pcm_buf[sample_pos * 2 + 1] = 0;
                sample_pos++;
            }

            sb16_stream = audio_stream_create(AUDIO_FORMAT_S16, 2, rate, pcm_buf, total_bytes);
            if (sb16_stream) {
                audio_stream_play(sb16_stream);
            }
        }
    }

    if (sb16_stream) {
        uint32_t wait_start = timer_millis();
        uint32_t max_wait_ms = total_ms + 1200u;
        while ((sb16_stream->is_playing || audio_is_running()) &&
               (timer_millis() - wait_start < max_wait_ms)) {
            timer_tick();
            io_wait();
        }
        audio_stream_destroy(sb16_stream);
    } else {
        bool speaker_on = false;
        for (size_t i = 0; i < notes_count; i++) {
            uint16_t freq = startup_sound[i].note;
            uint16_t duration_ms = startup_sound[i].duration;

            if (freq == NOTE_REST) {
                if (speaker_on) {
                    sound_stop();
                    speaker_on = false;
                }
            } else {
                sound_play(freq);
                speaker_on = true;
            }

            uint32_t start = timer_millis();
            while (timer_millis() - start < duration_ms) {
                timer_tick();
                io_wait();
            }
        }
        sound_stop();
    }

    if (pcm_buf) {
        kfree(pcm_buf);
    }
}
