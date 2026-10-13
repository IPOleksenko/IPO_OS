#include <system/timer.h>
#ifdef IPO_APP
#include <syscall.h>

uint64_t read_tsc(void) {
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

void timer_init(void) {}
void timer_tick(void) {}
void timer_update(void) {}

uint32_t timer_millis(void) {
    return (uint32_t)ipo_syscall(IPO_SYSCALL_MILLIS, 0u, NULL);
}

uint32_t timer_seconds(void) {
    return ipo_time(NULL);
}

uint32_t timer_elapsed_ms(uint32_t start_ms) {
    uint32_t args[1] = { start_ms };
    return (uint32_t)ipo_syscall(IPO_SYSCALL_ELAPSED_MS, 1u, args);
}

int timer_after(uint32_t start_ms, uint32_t delay_ms) {
    uint32_t args[2] = { start_ms, delay_ms };
    return (int)ipo_syscall(IPO_SYSCALL_TIMER_AFTER, 2u, args);
}

#else
#include <system/pit.h>
#include <kernel/terminal.h>
#include <kernel/process.h>
#include <stdio.h>
#include <ioport.h>

#define TSC_PER_MS 2000000ULL

static volatile uint32_t timer_ms = 0;
static volatile uint32_t timer_ticks = 0;
static uint64_t last_tsc_value = 0ULL;
static uint32_t last_slice_yield_ms = 0;

uint64_t read_tsc(void) {
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

void timer_init(void) {
    timer_ms = 0;
    timer_ticks = 0;
    last_tsc_value = read_tsc();
    last_slice_yield_ms = 0;
    pit_init(1000u);
}

void timer_update(void) {
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

void timer_tick(void) {
    timer_update();
    if (process_is_batch_active() && process_in_process_context() && !putchar_is_locked()) {
        if (timer_ms - last_slice_yield_ms >= 10u) {
            last_slice_yield_ms = timer_ms;
            process_yield_kernel();
        }
    }
}

uint32_t timer_millis(void) {
    timer_update();
    return timer_ms;
}

uint32_t timer_seconds(void) {
    return timer_millis() / 1000u;
}

uint32_t timer_elapsed_ms(uint32_t start_ms) {
    return timer_millis() - start_ms;
}

int timer_after(uint32_t start_ms, uint32_t delay_ms) {
    return (int)(timer_millis() - start_ms >= delay_ms);
}
#endif
