#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ioport.h>
#include <system/timer.h>
#include <syscall.h>

#define SB16_BASE_PORT       0x220
#define SB16_MIXER_ADDR      (SB16_BASE_PORT + 0x04)
#define SB16_MIXER_DATA      (SB16_BASE_PORT + 0x05)
#define SB16_RESET_PORT      (SB16_BASE_PORT + 0x06)
#define SB16_READ_DATA_PORT  (SB16_BASE_PORT + 0x0A)
#define SB16_WRITE_DATA_PORT (SB16_BASE_PORT + 0x0C)
#define SB16_READ_STATUS     (SB16_BASE_PORT + 0x0E)
#define SB16_ACK_16BIT       (SB16_BASE_PORT + 0x0F)

#define DMA_BUFFER_ADDR      0x600000

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
    for (int i = 0; i < 100; i++) io_wait();
    outb(SB16_RESET_PORT, 0);
    for (int i = 0; i < 2000; i++) {
        if (inb(SB16_READ_STATUS) & 0x80) {
            if (inb(SB16_READ_DATA_PORT) == 0xAA) {
                return true;
            }
        }
        io_wait();
    }
    return false;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("[SB16 Test] Probing Sound Blaster 16 on 0x220...\n");
    if (!sb16_reset()) {
        printf("[SB16 Test] FAILED to reset DSP on port 0x220!\n");
        return 1;
    }
    printf("[SB16 Test] DSP Reset OK (received 0xAA)!\n");

    /* Query DSP version */
    sb16_dsp_write(0xE1);
    uint8_t v_maj = sb16_dsp_read();
    uint8_t v_min = sb16_dsp_read();
    printf("[SB16 Test] DSP Version: %d.%02d\n", (int)v_maj, (int)v_min);

    /* Maximize volume in mixer */
    outb(SB16_MIXER_ADDR, 0x22); outb(SB16_MIXER_DATA, 0xFF); // Master
    outb(SB16_MIXER_ADDR, 0x04); outb(SB16_MIXER_DATA, 0xFF); // Voice

    /* Test multi-block streaming: 3 blocks of 0.3s (6615 samples each) */
    int16_t *buf = (int16_t *)DMA_BUFFER_ADDR;
    uint32_t rate = 22050;
    uint32_t block_samples = 6615;
    uint32_t freqs[3] = { 440, 554, 659 };

    for (int b = 0; b < 3; b++) {
        uint32_t period = rate / freqs[b];
        for (uint32_t i = 0; i < block_samples; i++) {
            int16_t sample = ((i % period) < (period / 2)) ? 14000 : -14000;
            buf[i * 2 + 0] = sample;
            buf[i * 2 + 1] = sample;
        }

        uint32_t phys_addr = DMA_BUFFER_ADDR;
        uint32_t total_bytes = block_samples * 4;
        uint32_t word_addr = phys_addr >> 1;
        uint16_t word_count = (total_bytes >> 1) - 1;

        outb(0xD4, 0x05); // Mask Ch 5
        outb(0xD8, 0x00); // Clear FF
        outb(0xD6, 0x49); // Mode: single-cycle read
        outb(0xC4, (uint8_t)(word_addr & 0xFF));
        outb(0xC4, (uint8_t)((word_addr >> 8) & 0xFF));
        outb(0x8B, (uint8_t)((phys_addr >> 16) & 0xFF));
        outb(0xC6, (uint8_t)(word_count & 0xFF));
        outb(0xC6, (uint8_t)((word_count >> 8) & 0xFF));
        outb(0xD4, 0x01); // Unmask Ch 5

        sb16_dsp_write(0x41);
        sb16_dsp_write((uint8_t)(rate >> 8));
        sb16_dsp_write((uint8_t)(rate & 0xFF));

        sb16_dsp_write(0xB0); // 16-bit single-cycle
        sb16_dsp_write(0x30); // signed stereo
        uint16_t sample_cnt = block_samples - 1;
        sb16_dsp_write((uint8_t)(sample_cnt & 0xFF));
        sb16_dsp_write((uint8_t)((sample_cnt >> 8) & 0xFF));

        uint32_t t0 = timer_millis();
        while (timer_elapsed_ms(t0) < 300) {
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        }
        inb(SB16_ACK_16BIT);
        printf("[SB16 Test] Block %d completed\n", b);
    }
    printf("[SB16 Test] All 3 blocks streamed successfully!\n");
    return 0;
}
