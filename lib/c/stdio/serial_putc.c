#include <stdio.h>
#include <ioport.h>

void serial_putc(char c) {
    uint32_t timeout = 10000;
    while (!(inb(0x3FD) & 0x20) && timeout > 0) {
        timeout--;
    }
    if (timeout > 0) {
        outb(0x3F8, (uint8_t)c);
    }
}