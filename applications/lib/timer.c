#include <system/timer.h>
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
    return (uint32_t)ipo_syscall(IPO_SYSCALL_TIME, 0u, NULL);
}

uint32_t timer_elapsed_ms(uint32_t start_ms) {
    uint32_t args[1] = { start_ms };
    return (uint32_t)ipo_syscall(IPO_SYSCALL_ELAPSED_MS, 1u, args);
}

int timer_after(uint32_t start_ms, uint32_t delay_ms) {
    uint32_t args[2] = { start_ms, delay_ms };
    return (int)ipo_syscall(IPO_SYSCALL_TIMER_AFTER, 2u, args);
}

