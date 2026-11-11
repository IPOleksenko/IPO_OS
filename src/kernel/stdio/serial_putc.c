#include <stdio.h>
#include <ioport.h>

void serial_init(void) {
    outb(0x3F9, 0x00);    // Disable all interrupts
    outb(0x3FB, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(0x3F8, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
    outb(0x3F9, 0x00);    //                  (hi byte)
    outb(0x3FB, 0x03);    // 8 bits, no parity, one stop bit
    outb(0x3FA, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(0x3FC, 0x0B);    // IRQs enabled, RTS/DSR set
}

void serial_putc(char c) {
    uint32_t timeout = 10000;
    while (!(inb(0x3FD) & 0x20) && timeout > 0) {
        timeout--;
    }
    if (timeout > 0) {
        outb(0x3F8, (uint8_t)c);
    }
}

int serial_getc(void) {
    /* LSR (Line Status Register) bit 0 = Data Ready */
    if (inb(0x3FD) & 0x01) {
        return (int)(uint8_t)inb(0x3F8);
    }
    return -1;
}