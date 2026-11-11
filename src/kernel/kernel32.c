#include <kernel/terminal.h>
#include <kernel/autorun.h>
#include <system/state.h>
#include <vga.h>
#include <vga_gfx.h>
#include <ioport.h>
#include <driver/sound.h>
#include <driver/audio_core.h>
#include <driver/ata/ata.h>
#include <file_system/ipo_fs.h>
#include <kernel/process.h>
#include <syscall.h>
#include <system/timer.h>
#include <driver/input/mouse.h>
#include <net/net.h>
#include <memory/memory_map.h>
#include <kernel/smp.h>
#include <kernel/device.h>
#include <driver/storage/block_dev.h>
#include <storage/storage_pool.h>
#include <kernel/kmod.h>
#include <memory/kmalloc.h>
#include <GL/gl_math.h>
#include <stdio.h>

#define FS_START_LBA (uint32_t)2048
#define STARTUP_SOUND {NOTE_C6, 150}, {NOTE_E6, 150}, {NOTE_G6, 150}, {NOTE_C7, 350}

static void ensure_fs_mounted(void) {
    storage_pool_state_t pool_state = storage_pool_get_state();
    if (pool_state == STORAGE_POOL_STATE_CORRUPT) {
        printf("[ipo_fs] Storage pool metadata is CORRUPTED. Automatic formatting ABORTED to prevent data loss.\n");
        return;
    }

    if (ipo_fs_mount(FS_START_LBA) || ipo_fs_mount(0)) {
        if (pool_state == STORAGE_POOL_STATE_DEGRADED) {
            printf("[ipo_fs] Mounted IPO_FS in DEGRADED storage pool mode.\n");
        } else {
            printf("[ipo_fs] Mounted IPO_FS\n");
        }
        return;
    }

    if (pool_state == STORAGE_POOL_STATE_DEGRADED) {
        printf("[ipo_fs] Storage pool is DEGRADED. Automatic formatting ABORTED to protect surviving data.\n");
        return;
    }

    printf("[ipo_fs] No IPO_FS found, attempting dynamic format...\n");
    if (!ipo_fs_install()) {
        printf("[ipo_fs] Initial filesystem installation failed\n");
    }
}

void play_startup_sound(void) {
    typedef struct {
        uint16_t note;
        uint16_t duration;
    } NoteDuration;

    NoteDuration startup_sound[] = { STARTUP_SOUND };
    size_t notes_count = sizeof(startup_sound) / sizeof(startup_sound[0]);
    if (notes_count == 0) return;

    /* 1. If Sound Blaster 16 continuous DMA is active, synthesize high-fidelity PCM */
    audio_stream_t *sb16_stream = NULL;
    int16_t *pcm_buf = NULL;

    if (audio_get_active_backend() == AUDIO_BACKEND_SB16) {
        uint32_t total_ms = 0;
        for (size_t i = 0; i < notes_count; i++) {
            total_ms += startup_sound[i].duration;
        }

        uint32_t rate = 22050;
        size_t total_samples = (rate * total_ms) / 1000;
        size_t total_bytes = total_samples * 4; /* 16-bit signed stereo = 4 bytes/sample */

        pcm_buf = (int16_t *)kmalloc(total_bytes);
        if (pcm_buf) {
            size_t sample_pos = 0;
            float current_phase = 0.0f;

            for (size_t n = 0; n < notes_count; n++) {
                uint16_t freq = startup_sound[n].note;
                uint32_t note_ms = startup_sound[n].duration;
                size_t note_samples = (rate * note_ms) / 1000;
                float phase_inc = (6.2831853f * (float)freq) / (float)rate;

                for (size_t s = 0; s < note_samples && sample_pos < total_samples; s++, sample_pos++) {
                    if (freq == NOTE_REST) {
                        pcm_buf[sample_pos * 2 + 0] = 0;
                        pcm_buf[sample_pos * 2 + 1] = 0;
                        continue;
                    }

                    float amp = 14000.0f;
                    /* Smooth attack at very start of song (first 10 ms) */
                    if (sample_pos < 220) {
                        amp *= (float)sample_pos / 220.0f;
                    }
                    /* Smooth release at very end of song (last 20 ms) */
                    if (sample_pos > total_samples - 440) {
                        amp *= (float)(total_samples - sample_pos) / 440.0f;
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

            sb16_stream = audio_stream_create(AUDIO_FORMAT_S16, 2, rate, pcm_buf, total_bytes);
            if (sb16_stream) {
                audio_stream_play(sb16_stream);
            }
        }
    }

    /* 2. Play continuous notes */
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
            /* Directly change frequency to next note without turning off speaker gate */
            sound_play(freq);
            speaker_on = true;
        }

        uint32_t start = timer_millis();
        while (timer_millis() - start < duration_ms) {
            timer_tick();
            io_wait();
        }
    }

    /* Stop PC speaker when the song finishes */
    sound_stop();

    /* Allow SB16 stream to finish playing gracefully if active */
    if (sb16_stream) {
        uint32_t wait_start = timer_millis();
        while (sb16_stream->is_playing && (timer_millis() - wait_start < 400)) {
            timer_tick();
            io_wait();
        }
        audio_stream_destroy(sb16_stream);
    }
    if (pcm_buf) {
        kfree(pcm_buf);
    }
}

void kmain(void) {
    serial_init();
    system_set_state(SYSTEM_STATE_BOOT);

    terminal_initialize();

    memory_map_init();

    process_init();

    syscall_init();

    timer_init();

    smp_init();

    async_scheduler_init();

    sound_init();
    audio_init();
    
    block_dev_init();

    ata_init();

    storage_pool_init();

    kmod_init();
    
    mouse_init();

    net_init();

    device_init();

    paging_init();

    ipo_fs_init();

    ensure_fs_mounted();

    play_startup_sound();
    terminal_clear_buffer();
    terminal_clear_command_history();
    printf("Type \"help\" on the keyboard to view commands, shortcuts, and OS features.\n\n");

    system_set_state(SYSTEM_STATE_TERMINAL_IDLE);

    autorun_init();
    
    for (;;) {
        timer_tick();
        async_scheduler_tick();
        net_poll();
        terminal_console();
    }
}
