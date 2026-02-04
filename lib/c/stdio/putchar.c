#include <stdio.h>
#include <vga.h>
#include <ioport.h>
#include <kernel/terminal.h>

/**
 * Output a single character to VGA memory at cursor position
 */
void putchar(char c) {
    putchar_color(c, VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

void putchar_color(char c, uint8_t fg, uint8_t bg) {
    volatile uint16_t *vga = VGA_MEMORY;
    uint16_t cursor = vga_get_cursor_position();
    uint16_t top_row = VGA_START_CURSOR_POSITION / VGA_WIDTH;
    uint16_t terminal_rows = VGA_HEIGHT - top_row;
    uint16_t terminal_bottom = (top_row + terminal_rows) * VGA_WIDTH;
    
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
        vga[cursor] = vga_entry((unsigned char)c, fg, bg);
        cursor++;
    }
    
    /* Handle overflow by scrolling */
    if (cursor >= terminal_bottom) {
        /* Call auto-scroll to save history and shift lines up */
        terminal_auto_scroll();
        /* Cursor is now at the start of the last line */
        cursor = terminal_bottom - VGA_WIDTH;
    }
    
    vga_set_cursor(cursor);
}