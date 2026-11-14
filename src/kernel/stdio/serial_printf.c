#include <stdio.h>
#include <kernel/smp.h>

static spinlock_t s_serial_printf_lock = {0};

static void serial_emit_ch(void *ctx, char ch) {
    (void)ctx;
    serial_putc_raw(ch);
}

int serial_printf(const char *format, ...) {
    uint32_t flags = spin_lock_irqsave(&s_serial_printf_lock);

    va_list args;
    va_start(args, format);
    int count = stdio_vformat(serial_emit_ch, NULL, format, args);
    va_end(args);

    spin_unlock_irqrestore(&s_serial_printf_lock, flags);
    return count;
}
