#include <driver/sound.h>
#include <system/pit.h>
#include <ioport.h>

/* Sound control port */
#define SOUND_CTRL_PORT 0x61
#define SOUND_CTRL_GATE 0x01
#define SOUND_CTRL_SPEAKER 0x02

void sound_init(void) {
    pit_init(0);  /* Just initialize PIT structure */
}

void sound_play(uint16_t frequency) {
    if (frequency == 0 || frequency > 20000)
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

void sound_beep(uint16_t frequency, uint16_t duration) {
    sound_play(frequency);
    
    /* Delay in milliseconds (busy-wait loop) */
    volatile uint32_t delay = duration * 1000;
    while (delay--) {
        io_wait();
    }
    
    sound_stop();
}
