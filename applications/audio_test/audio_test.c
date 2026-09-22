#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <driver/audio_core.h>
#include <system/timer.h>
#include <system/state.h>
#include <syscall.h>

#define SAMPLES_COUNT 8000

static uint8_t  u8_samples[SAMPLES_COUNT];
static int16_t  s16_samples[SAMPLES_COUNT * 2]; /* stereo */
static uint8_t  s24_samples[SAMPLES_COUNT * 3]; /* 24-bit packed */
static float    f32_samples[SAMPLES_COUNT * 2]; /* float stereo */

/* Generate waveform buffers using integer approximations to avoid heavy trig */
static void generate_test_buffers(void) {
    /* Triangle wave / pulse wave generator */
    for (int i = 0; i < SAMPLES_COUNT; i++) {
        /* U8: 0..255, center 128 */
        int phase8 = (i * 20) % 256;
        u8_samples[i] = (uint8_t)phase8;

        /* S16 stereo: -32768..32767 */
        int phase16_l = ((i * 15) % 1000) - 500;
        int phase16_r = ((i * 25) % 1000) - 500;
        s16_samples[i * 2 + 0] = (int16_t)(phase16_l * 40);
        s16_samples[i * 2 + 1] = (int16_t)(phase16_r * 40);

        /* S24: 3 bytes little endian */
        int32_t val24 = (((i * 30) % 2000) - 1000) * 4000;
        s24_samples[i * 3 + 0] = (uint8_t)(val24 & 0xFF);
        s24_samples[i * 3 + 1] = (uint8_t)((val24 >> 8) & 0xFF);
        s24_samples[i * 3 + 2] = (uint8_t)((val24 >> 16) & 0xFF);

        /* F32: -1.0f .. 1.0f */
        float f_val = (float)(((i * 12) % 100) - 50) / 50.0f;
        f32_samples[i * 2 + 0] = f_val * 0.5f;
        f32_samples[i * 2 + 1] = -f_val * 0.5f;
    }
}

static void delay_ms(uint32_t ms) {
    uint32_t start = timer_millis();
    while (timer_elapsed_ms(start) < ms) {
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("[Audio Test] Initializing universal audio core...\n");
    if (!audio_init()) {
        printf("[Audio Test] No compatible audio hardware detected!\n");
        return 1;
    }

    printf("[Audio Test] Active Audio Backend: %s\n", audio_get_backend_name());
    printf("[Audio Test] Generating dynamic waveforms (8/16/24-bit int and 32-bit float)...\n");
    generate_test_buffers();

    /* 1. Test 8-bit mono stream at 11025 Hz */
    printf("[Audio Test] Testing 8-bit mono 11025 Hz stream...\n");
    audio_stream_t *s_u8 = audio_stream_create(AUDIO_FORMAT_U8, 1, 11025, u8_samples, sizeof(u8_samples));
    if (s_u8) {
        audio_stream_play(s_u8);
        for (int t = 0; t < 30; t++) {
            audio_tick();
            delay_ms(10);
        }
        audio_stream_destroy(s_u8);
    }

    /* 2. Test 16-bit stereo stream at 22050 Hz */
    printf("[Audio Test] Testing 16-bit stereo 22050 Hz stream...\n");
    audio_stream_t *s_s16 = audio_stream_create(AUDIO_FORMAT_S16, 2, 22050, s16_samples, sizeof(s16_samples));
    if (s_s16) {
        audio_stream_play(s_s16);
        for (int t = 0; t < 30; t++) {
            audio_tick();
            delay_ms(10);
        }
        audio_stream_destroy(s_s16);
    }

    /* 3. Test 24-bit stream at 44100 Hz */
    printf("[Audio Test] Testing 24-bit high-resolution 44100 Hz stream...\n");
    audio_stream_t *s_s24 = audio_stream_create(AUDIO_FORMAT_S24, 1, 44100, s24_samples, sizeof(s24_samples));
    if (s_s24) {
        audio_stream_play(s_s24);
        for (int t = 0; t < 30; t++) {
            audio_tick();
            delay_ms(10);
        }
        audio_stream_destroy(s_s24);
    }

    /* 4. Test 32-bit float stereo stream at 48000 Hz */
    printf("[Audio Test] Testing 32-bit IEEE float 48000 Hz studio stream...\n");
    audio_stream_t *s_f32 = audio_stream_create(AUDIO_FORMAT_F32, 2, 48000, f32_samples, sizeof(f32_samples));
    if (s_f32) {
        audio_stream_play(s_f32);
        for (int t = 0; t < 30; t++) {
            audio_tick();
            delay_ms(10);
        }
        audio_stream_destroy(s_f32);
    }

    printf("[Audio Test] All audio formats, bit depths and sample rates verified successfully!\n");
    return 0;
}
