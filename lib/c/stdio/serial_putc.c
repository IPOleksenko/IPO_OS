#include <stdio.h>
#include <ioport.h>

void serial_putc(char c) {
    while (!(inb(0x3FD) & 0x20)) {}
    outb(0x3F8, (uint8_t)c);
}