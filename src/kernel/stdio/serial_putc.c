#include <stdio.h>
#include <ioport.h>
#include <kernel/smp.h>

static spinlock_t s_serial_lock = {0};

void serial_init(void) {
    outb(0x3F9, 0x00);    // IER: disable interrupts
    outb(0x3FB, 0x80);    // LCR: enable DLAB for divisor latch
    outb(0x3F8, 0x01);    // DLL: 115200 baud (divisor = 1)
    outb(0x3F9, 0x00);    // DLM: divisor high byte
    outb(0x3FB, 0x03);    // LCR: 8N1, clear DLAB
    outb(0x3FA, 0xC7);    // FCR: enable/clear FIFOs, 14-byte threshold
    outb(0x3FC, 0x0B);    // MCR: DTR, RTS, OUT2
}

void serial_putc_raw(char c) {
    uint32_t timeout = 10000;
    while (!(inb(0x3FD) & 0x20) && timeout > 0) {
        timeout--;
    }
    if (timeout > 0) {
        outb(0x3F8, (uint8_t)c);
    }
}

void serial_putc(char c) {
    ipo_reg_t flags = spin_lock_irqsave(&s_serial_lock);
    serial_putc_raw(c);
    spin_unlock_irqrestore(&s_serial_lock, flags);
}

int serial_getc(void) {
    /* LSR (Line Status Register) bit 0 = Data Ready */
    if (inb(0x3FD) & 0x01) {
        return (int)(uint8_t)inb(0x3F8);
    }
    return -1;
}