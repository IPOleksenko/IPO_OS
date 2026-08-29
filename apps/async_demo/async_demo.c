#include <stdint.h>
#include <stdio.h>
#include <syscall.h>

static volatile uint32_t tick_a = 0u;
static volatile uint32_t tick_b = 0u;

static void periodic_async_message_a(void) {
    tick_a++;
    printf("[async-demo A] tick=%u\n", tick_a);
}

static void periodic_async_message_b(void) {
    tick_b++;
    printf("[async-demo B] tick=%u\n", tick_b);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("[async-demo] registering two background tasks\n");

    int reg_a = ipo_syscall(IPO_SYSCALL_ASYNC_START,
                            (uint32_t)(uintptr_t)"async_demo_task_a",
                            20000000u,
                            (uint32_t)(uintptr_t)periodic_async_message_a,
                            0,
                            0);

    int reg_b = ipo_syscall(IPO_SYSCALL_ASYNC_START,
                            (uint32_t)(uintptr_t)"async_demo_task_b",
                            30000000u,
                            (uint32_t)(uintptr_t)periodic_async_message_b,
                            0,
                            0);

    if (reg_a < 0 || reg_b < 0) {
        printf("[async-demo] registration failed: A=%d B=%d\n", reg_a, reg_b);
        return 1;
    }

    printf("[async-demo] tasks registered, returning to shell now\n");

    return 0;
}