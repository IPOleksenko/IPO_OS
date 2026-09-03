#include <stdio.h>
#include <vga.h>
#include <ioport.h>
#include <kernel/terminal.h>
#include <kernel/driver.h>

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

    c = driver_dispatch_char_output(c);

    static char utf8_buf[4];
    static uint8_t utf8_len = 0;
    static uint8_t utf8_expected = 0;

    unsigned char uc = (unsigned char)c;

    if (utf8_len > 0) {
        /* Accumulate continuation byte */
        utf8_buf[utf8_len++] = c;
        if (utf8_len < utf8_expected) {
            release_output_lock();
            return;
        }

        /* Full UTF-8 sequence received */
        size_t b = 0;
        uint8_t glyph = utf8_to_vga_glyph(utf8_buf, utf8_len, &b);
        utf8_len = 0;
        utf8_expected = 0;
        c = (char)glyph;
    } else if ((uc & 0x80) != 0) {
        /* Start of UTF-8 multi-byte sequence */
        if ((uc & 0xE0) == 0xC0) {
            utf8_expected = 2;
        } else if ((uc & 0xF0) == 0xE0) {
            utf8_expected = 3;
        } else if ((uc & 0xF8) == 0xF0) {
            utf8_expected = 4;
        } else {
            utf8_expected = 1;
        }

        if (utf8_expected > 1) {
            utf8_buf[0] = c;
            utf8_len = 1;
            release_output_lock();
            return;
        }
    }

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