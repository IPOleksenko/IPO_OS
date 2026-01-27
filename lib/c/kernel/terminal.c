#include <kernel/terminal.h>

#include <vga.h>
#include <stdbool.h>

void terminal_initialize(void) {
    volatile uint16_t* vga = VGA_MEMORY;

    vga_clear(VGA_COLOR_WHITE, VGA_COLOR_BLACK, true, VGA_WIDTH);
}