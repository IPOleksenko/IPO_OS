#include <ioport.h>
#include <vga.h>

void kmain(void) {
    vga_clear(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    for (;;) asm volatile("hlt");
}
