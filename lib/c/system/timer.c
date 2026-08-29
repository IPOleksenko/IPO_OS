#include <system/timer.h>
#include <system/pit.h>
#include <kernel/terminal.h>
#include <ioport.h>

static volatile uint32_t timer_ms = 0;
static volatile uint32_t timer_ticks = 0;

void timer_init(void) {
    timer_ms = 0;
    timer_ticks = 0;
    pit_init(1000u);
}

void timer_tick(void) {
    timer_ms += 1u;
    timer_ticks++;
}

uint32_t timer_millis(void) {
    return timer_ms;
}

uint32_t timer_seconds(void) {
    return timer_ms / 1000u;
}

uint32_t timer_elapsed_ms(uint32_t start_ms) {
    return timer_ms - start_ms;
}

int timer_after(uint32_t start_ms, uint32_t delay_ms) {
    return (int)(timer_ms - start_ms >= delay_ms);
}
