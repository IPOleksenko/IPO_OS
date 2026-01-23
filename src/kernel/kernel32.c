typedef unsigned short u16;

static inline void outb(unsigned short port, unsigned char val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void kmain(void) {
    const char *s = "K32";
    const char *p = s;
    while (*p) {
        outb(0x3f8, *p);
        p++;
    }

    /* Mark before VGA write */
    outb(0x3f8, 'V'); outb(0x3f8, 'G'); outb(0x3f8, 'A');

    /* Also write directly to VGA memory at 0xB8000 for visual check */
    volatile unsigned short *vga = (unsigned short*)0xB8000;
    vga[0] = (('3') | (0x07 << 8));

    /* Read back first two bytes from VGA memory and send to serial */
    unsigned char b0 = *((volatile unsigned char*)0xB8000);
    unsigned char b1 = *((volatile unsigned char*)0xB8001);
    outb(0x3f8, 'r'); outb(0x3f8, b0); outb(0x3f8, b1);

    for (;;) asm volatile("hlt");
}
