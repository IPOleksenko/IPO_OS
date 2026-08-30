#include <stdio.h>
#include <vga.h>
#include <ioport.h>
#include <kernel/terminal.h>
#include <file_system/ipo_fs.h>

// Simple spinlock to prevent race conditions during VGA output
static volatile uint8_t output_lock = 0;
static char terminal_history_line[4096];
static int terminal_history_line_len = 0;
static uint8_t terminal_history_line_fg = VGA_COLOR_LIGHT_GREY;
static uint8_t terminal_history_line_bg = VGA_COLOR_BLACK;

void clear_terminal_history_file(void) {
    if (!fs_mounted) {
        return;
    }

    struct ipo_inode inode;
    if (ipo_fs_stat("/terminal_history", &inode)) {
        ipo_fs_delete("/terminal_history");
    }

    if (ipo_fs_create("/terminal_history", IPO_INODE_TYPE_FILE) < 0) {
        return;
    }

    terminal_history_line_len = 0;
    terminal_history_line[0] = '\0';
}

static void flush_terminal_history_line(void) {
    if (!fs_mounted || terminal_history_line_len <= 0) {
        return;
    }

    char record[4096];
    int len = snprintf(record, sizeof(record), "%s|%u|%u\n",
                       terminal_history_line,
                       terminal_history_line_fg,
                       terminal_history_line_bg);
    if (len <= 0) {
        return;
    }

    int fd = ipo_fs_open("/terminal_history");
    if (fd < 0) {
        return;
    }

    struct ipo_inode inode;
    if (!ipo_fs_stat("/terminal_history", &inode)) {
        ipo_fs_close(fd);
        terminal_history_line_len = 0;
        terminal_history_line[0] = '\0';
        return;
    }

    ipo_fs_write(fd, record, (uint32_t)len, inode.size);
    ipo_fs_close(fd);

    terminal_history_line_len = 0;
    terminal_history_line[0] = '\0';
}

static void append_terminal_log(char c, uint8_t fg, uint8_t bg) {
    if (c == 0 || !fs_mounted) {
        return;
    }

    if (terminal_history_line_len == 0) {
        terminal_history_line_fg = fg;
        terminal_history_line_bg = bg;
    }

    if (c == '\n' || c == '\r') {
        flush_terminal_history_line();
        return;
    }

    if (terminal_history_line_len < (int)sizeof(terminal_history_line) - 1) {
        terminal_history_line[terminal_history_line_len++] = c;
        terminal_history_line[terminal_history_line_len] = '\0';
    }
}

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
        append_terminal_log('\n', fg, bg);
        uint16_t row = cursor / VGA_WIDTH;
        cursor = (row + 1) * VGA_WIDTH;
    } else if (c == '\r') {
        append_terminal_log('\r', fg, bg);
        uint16_t row = cursor / VGA_WIDTH;
        cursor = row * VGA_WIDTH;
    } else if (c == '\t') {
        append_terminal_log('\t', fg, bg);
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
        append_terminal_log(c, fg, bg);
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