#include <driver/sound.h>
#include <ioport.h>

#define PIT_FREQUENCY 1193182  /* PIT oscillator frequency in Hz */
#define PIT_CONTROL_PORT 0x43  /* Control port for PIT */

void sound_init(void) {
    /* Initialize timer 2 for sound output */
    /* Select timer 2, 16-bit mode, square wave */
    outb(PIT_CONTROL_PORT, 0xB6);  /* Control word: timer 2, binary, mode 3, 16-bit load */
}

void sound_play(uint16_t frequency) {
    if (frequency == 0 || frequency > 20000)
        return;
    
    /* Calculate counter value for frequency */
    uint16_t counter = PIT_FREQUENCY / frequency;
    
    /* Send counter to timer 2 (port 0x42) */
    outb(0x42, (uint8_t)(counter & 0xFF));        /* Low byte */
    io_wait();
    outb(0x42, (uint8_t)((counter >> 8) & 0xFF)); /* High byte */
    io_wait();
    
    /* Enable speaker */
    uint8_t control = inb(SOUND_CTRL_PORT);
    outb(SOUND_CTRL_PORT, control | SOUND_CTRL_GATE | SOUND_CTRL_SPEAKER);
}

void sound_stop(void) {
    /* Disable speaker */
    uint8_t control = inb(SOUND_CTRL_PORT);
    outb(SOUND_CTRL_PORT, control & ~(SOUND_CTRL_GATE | SOUND_CTRL_SPEAKER));
}

void sound_beep(uint16_t frequency, uint16_t duration) {
    sound_play(frequency);
    
    /* Delay in milliseconds (busy-wait loop) */
    volatile uint32_t delay = duration * 1000;
    while (delay--) {
        io_wait();
    }
    
    sound_stop();
}
