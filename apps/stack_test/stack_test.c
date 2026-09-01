#include <stdint.h>
#include <stdio.h>
#include <syscall.h>

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

static void stress_stack(uint32_t depth, uint32_t seed) {
    volatile uint8_t chunk[4096];

    for (uint32_t i = 0; i < sizeof(chunk); ++i) {
        chunk[i] = (uint8_t)((seed + i) & 0xFFu);
    }

    if (depth > 0u) {
        stress_stack(depth - 1u, seed + 17u);
    }

    for (uint32_t i = 0; i < sizeof(chunk); ++i) {
        if (chunk[i] != (uint8_t)((seed + i) & 0xFFu)) {
            printf("stack corruption detected at depth=%u\n", depth);
            serial_printf("stack corruption detected at depth=%u\n", depth);
            return;
        }
    }
}

int main(int argc, char **argv) {
    uint32_t extra = parse_count((argc > 1) ? argv[1] : NULL, 128u * 1024u);

    printf("[stack-test] starting: requested extra=%u bytes\n", extra);
    serial_printf("[stack-test] starting: requested extra=%u bytes\n", extra);

    uint32_t grow_args[] = { extra };
    int grown = ipo_syscall(IPO_SYSCALL_STACK_GROW, 1u, grow_args);

    printf("[stack-test] grow result=%d\n", grown);
    serial_printf("[stack-test] grow result=%d\n", grown);

    stress_stack(8u, 0x11u);

    uint32_t shrink_args[] = { extra };
    int shrunk = ipo_syscall(IPO_SYSCALL_STACK_SHRINK, 1u, shrink_args);

    printf("[stack-test] shrink result=%d\n", shrunk);
    serial_printf("[stack-test] shrink result=%d\n", shrunk);

    printf("[stack-test] done\n");
    serial_printf("[stack-test] done\n");

    return 0;
}
