#include <stdint.h>
#include <stdio.h>
#include <syscall.h>

static volatile uint32_t tick_a = 0u;
static volatile uint32_t tick_b = 0u;
static volatile uint32_t total_runs = 0u;

static uint32_t parse_count(const char *text, uint32_t default_value) {
    uint32_t value = 0u;

    if (text == NULL || text[0] == '\0') {
        return default_value;
    }

    while (*text != '\0') {
        char c = *text++;
        if (c < '0' || c > '9') {
            return default_value;
        }
        value = value * 10u + (uint32_t)(c - '0');
    }

    return value == 0u ? default_value : value;
}

static void periodic_async_message_a(void) {
    tick_a++;
    printf("[async-demo A] tick=%u\n", tick_a);
    serial_printf("[async-demo A] tick=%u\n", tick_a);

    if (total_runs > 0u && tick_a >= total_runs) {
        ipo_syscall(IPO_SYSCALL_ASYNC_STOP,
                    (uint32_t)(uintptr_t)"async_demo_task_a",
                    0, 0, 0, 0);
    }
}

static void periodic_async_message_b(void) {
    tick_b++;
    printf("[async-demo B] tick=%u\n", tick_b);
    serial_printf("[async-demo B] tick=%u\n", tick_b);

    if (total_runs > 0u && tick_b >= total_runs) {
        ipo_syscall(IPO_SYSCALL_ASYNC_STOP,
                    (uint32_t)(uintptr_t)"async_demo_task_b",
                    0, 0, 0, 0);
    }
}

int main(int argc, char **argv) {
    uint32_t runs = parse_count((argc > 1) ? argv[1] : NULL, 3u);
    total_runs = runs;

    serial_printf("[async-demo] registering two background tasks, limit=%u\n", runs);

    int reg_a = ipo_syscall(IPO_SYSCALL_ASYNC_START,
                            (uint32_t)(uintptr_t)"async_demo_task_a",
                            20000u,
                            (uint32_t)(uintptr_t)periodic_async_message_a,
                            0,
                            0);

    int reg_b = ipo_syscall(IPO_SYSCALL_ASYNC_START,
                            (uint32_t)(uintptr_t)"async_demo_task_b",
                            30000u,
                            (uint32_t)(uintptr_t)periodic_async_message_b,
                            0,
                            0);

    if (reg_a < 0 || reg_b < 0) {
        printf("[async-demo] registration failed: A=%d B=%d\n", reg_a, reg_b);
        serial_printf("[async-demo] registration failed: A=%d B=%d\n", reg_a, reg_b);
        return 1;
    }

    serial_printf("[async-demo] tasks registered, returning to shell now\n");

    return 0;
}