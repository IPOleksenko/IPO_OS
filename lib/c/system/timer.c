#include <system/timer.h>
#include <system/pit.h>
#include <kernel/terminal.h>
#include <ioport.h>

#define TSC_PER_MS 2000000ULL

static volatile uint32_t timer_ms = 0;
static volatile uint32_t timer_ticks = 0;
static uint64_t last_tsc_value = 0ULL;

static uint64_t read_tsc(void) {
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

void timer_init(void) {
    timer_ms = 0;
    timer_ticks = 0;
    last_tsc_value = read_tsc();
    pit_init(1000u);
}

void timer_tick(void) {
    uint64_t now_tsc = read_tsc();
    if (last_tsc_value == 0ULL) {
        last_tsc_value = now_tsc;
        return;
    }

    uint64_t delta_tsc = now_tsc - last_tsc_value;
    uint32_t delta_ms = (uint32_t)(delta_tsc / TSC_PER_MS);
    if (delta_ms == 0u) {
        return;
    }

    timer_ms += delta_ms;
    timer_ticks += delta_ms;
    last_tsc_value += (uint64_t)delta_ms * TSC_PER_MS;
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
