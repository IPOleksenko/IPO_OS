#include <stdio.h>
#include <vga.h>
#include <ioport.h>

/**
 * Output a single character to VGA memory at cursor position
 */
void putchar(char c) {
    volatile uint16_t *vga = VGA_MEMORY;
    uint16_t cursor = vga_get_cursor_position();
    
    if (c == '\n') {
        /* Move to next line */
        uint16_t row = cursor / VGA_WIDTH;
        cursor = (row + 1) * VGA_WIDTH;
    } else if (c == '\r') {
        /* Move to start of line */
        uint16_t row = cursor / VGA_WIDTH;
        cursor = row * VGA_WIDTH;
    } else if (c == '\t') {
        /* Move to next tab stop (8 chars) */
        uint16_t col = cursor % VGA_WIDTH;
        cursor += (8 - (col % 8));
    } else {
        /* Print character at current position */
        vga[cursor] = vga_entry((unsigned char)c, VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        cursor++;
    }
    
    /* Wrap to next line if needed */
    if (cursor >= VGA_WIDTH * VGA_HEIGHT) {
        cursor = VGA_WIDTH * VGA_HEIGHT - 1;  /* Stay at last line */
    }
    
    vga_set_cursor(cursor);
}