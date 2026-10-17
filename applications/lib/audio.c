#include <driver/audio_core.h>
#include <driver/sound.h>
#include <syscall.h>

bool audio_init(void) {
    return ipo_syscall(IPO_SYSCALL_AUDIO_INIT, 0u, NULL) != 0;
}

int audio_get_active_backend(void) {
    return AUDIO_BACKEND_SB16;
}

const char *audio_get_backend_name(void) {
    return "Sound Blaster 16";
}

audio_stream_t *audio_stream_create(int format, int channels, uint32_t sample_rate, const void *data, size_t size) {
    uint32_t args[5];
    args[0] = (uint32_t)format;
    args[1] = (uint32_t)channels;
    args[2] = sample_rate;
    args[3] = (uint32_t)(uintptr_t)data;
    args[4] = (uint32_t)size;
    return (audio_stream_t *)(uintptr_t)ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_CREATE, 5u, args);
}

audio_stream_t *audio_stream_create_ring(int format, int channels, uint32_t sample_rate,
                                         const void *ring_buf, size_t ring_size_samples,
                                         size_t total_expected_samples) {
    uint32_t args[6];
    args[0] = (uint32_t)format;
    args[1] = (uint32_t)channels;
    args[2] = sample_rate;
    args[3] = (uint32_t)(uintptr_t)ring_buf;
    args[4] = (uint32_t)ring_size_samples;
    args[5] = (uint32_t)total_expected_samples;
    return (audio_stream_t *)(uintptr_t)ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_CREATE_RING, 6u, args);
}

void audio_stream_update_buffered(audio_stream_t *stream, size_t buffered_samples, bool is_eof) {
    uint32_t args[3];
    args[0] = (uint32_t)(uintptr_t)stream;
    args[1] = (uint32_t)buffered_samples;
    args[2] = is_eof ? 1u : 0u;
    ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_UPDATE, 3u, args);
}

void audio_stream_destroy(audio_stream_t *stream) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)stream;
    ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_DESTROY, 1u, args);
}

void audio_stream_play(audio_stream_t *stream) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)stream;
    ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_PLAY, 1u, args);
}

void audio_stream_pause(audio_stream_t *stream) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)stream;
    ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_PAUSE, 1u, args);
}

void audio_stream_stop(audio_stream_t *stream) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)stream;
    ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_STOP, 1u, args);
}

void audio_stream_set_volume(audio_stream_t *stream, float volume) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)stream;
    args[1] = (uint32_t)(uintptr_t)&volume;
    ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_SET_VOL, 2u, args);
}

void audio_stream_seek(audio_stream_t *stream, double progress_fraction) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)stream;
    args[1] = (uint32_t)(uintptr_t)&progress_fraction;
    ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_SEEK, 2u, args);
}

double audio_stream_get_progress(audio_stream_t *stream) {
    double prog = 0.0;
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)stream;
    args[1] = (uint32_t)(uintptr_t)&prog;
    ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_PROGRESS, 2u, args);
    return prog;
}

void audio_tick(void) {
    ipo_syscall(IPO_SYSCALL_AUDIO_TICK, 0u, NULL);
}

void sb16_stop(void) {
    ipo_syscall(IPO_SYSCALL_AUDIO_STOP_ALL, 0u, NULL);
}

bool sb16_get_dsp_version(uint8_t *major, uint8_t *minor) {
    if (major) *major = 4;
    if (minor) *minor = 5;
    return true;
}

void audio_stop_all(void) {
    ipo_syscall(IPO_SYSCALL_AUDIO_STOP_ALL, 0u, NULL);
}

void sound_init(void) {}
void sound_play(uint32_t frequency) { (void)frequency; }
void sound_stop(void) {}
void sound_beep(uint32_t frequency, uint32_t duration) { (void)frequency; (void)duration; }

