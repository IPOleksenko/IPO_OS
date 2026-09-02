#include <stdio.h>
#include <vga.h>
#include <ioport.h>
#include <kernel/terminal.h>

// Simple spinlock to prevent race conditions during VGA output
static volatile uint8_t output_lock = 0;

static void acquire_output_lock(void) {
    while (__sync_lock_test_and_set(&output_lock, 1)) {
        // Spin until lock is free
    }
}

static void release_output_lock(void) {
    __sync_lock_release(&output_lock);
}

/**
 * Output a single character to VGA memory at cursor position
 */
void putchar(char c) {
    putchar_color(c, VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

void putchar_color(char c, uint8_t fg, uint8_t bg) {
    terminal_on_external_output();
    acquire_output_lock();

    volatile uint16_t *vga = VGA_MEMORY;
    uint16_t cursor = vga_get_cursor_position();
    uint16_t top_row = VGA_START_CURSOR_POSITION / VGA_WIDTH;
    uint16_t terminal_rows = VGA_HEIGHT - top_row;
    uint16_t terminal_bottom = (top_row + terminal_rows) * VGA_WIDTH;
    uint16_t last_line_start = (top_row + terminal_rows - 1) * VGA_WIDTH;

    if (cursor < VGA_START_CURSOR_POSITION || cursor >= VGA_WIDTH * VGA_HEIGHT) {
        cursor = VGA_START_CURSOR_POSITION;
    }

    if (c == '\n') {
        uint16_t row = cursor / VGA_WIDTH;
        cursor = (row + 1) * VGA_WIDTH;
    } else if (c == '\r') {
        uint16_t row = cursor / VGA_WIDTH;
        cursor = row * VGA_WIDTH;
    } else if (c == '\b') {
        if (cursor > VGA_START_CURSOR_POSITION) {
            cursor--;
        }
    } else if (c == '\t') {
        uint16_t col = cursor % VGA_WIDTH;
        uint16_t spaces = 8 - (col % 8);
        if (spaces == 0) {
            spaces = 8;
        }
        if (cursor + spaces >= terminal_bottom) {
            terminal_auto_scroll();
            cursor = last_line_start;
        } else {
            cursor += spaces;
        }
    } else {
        if (cursor >= terminal_bottom) {
            terminal_auto_scroll();
            cursor = last_line_start;
        }
        vga[cursor] = vga_entry((unsigned char)c, fg, bg);
        cursor++;
    }

    if (cursor >= terminal_bottom) {
        terminal_auto_scroll();
        cursor = last_line_start;
    }

    if (cursor >= VGA_WIDTH * VGA_HEIGHT) {
        cursor = VGA_START_CURSOR_POSITION;
    }

    vga_set_cursor(cursor);
    release_output_lock();
}