#include <kernel/sys/pit.h>
#include <kernel/sys/isr.h>
#include <kernel/sys/ioports.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#define PIT_REG_COUNTER_0   0x40
#define PIT_REG_COMMAND     0x43
#define TIMER_FREQUENCY     100  // Timer frequency in Hz

volatile unsigned int ticks = 0;
BOOL should_run = TRUE;

void timer_irq_handler(__attribute__ ((unused)) registers_t r) {
    ticks++;
}


void sleep_ticks(uint32_t delay_ticks) {
    size_t start_ticks = ticks;

    while (ticks - start_ticks < delay_ticks) {
        // busy
    }
}

void sleep(uint32_t milliseconds) {
    uint32_t needticks = milliseconds * TIMER_FREQUENCY;

    sleep_ticks(needticks / 1000);
}

void usleep(uint32_t microseconds) {
    uint32_t delay_ticks = (microseconds * TIMER_FREQUENCY) / 1000000;
    sleep_ticks(delay_ticks);
}

unsigned int timer_get_uptime() {
    return ticks;
}

void timer_set_phase(uint32_t hz) {
    int div = PIT_FREQ / hz;
    uint8_t ocw = PIT_WRITE_LSB_MSB | PIT_WRITE_COUNTER_0 |
                  PIT_BINARY_MODE   | PIT_SQUARE_WAVE_MODE;

    outb(PIT_REG_COMMAND, ocw);
    outb(PIT_REG_COUNTER_0, (div & 0xFF));
    outb(PIT_REG_COUNTER_0, (div >> 8) & 0xFF);
}

void timer_init() {
    timer_set_phase(TIMER_FREQUENCY);
    install_irq_handler(0, timer_irq_handler);
    printf("PIT initialization completed successfully!\n");
}
