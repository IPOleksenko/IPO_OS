#include <stdio.h>
#include <vga.h>
#include <vga_gfx.h>
#include <wm.h>
#include <ioport.h>
#include <kernel/terminal.h>
#include <kernel/driver.h>
#include <kernel/process.h>
#include <kernel/smp.h>
#include <kernel/utf.h>
#include <memory/kmalloc.h>
#include <string.h>

static volatile uint8_t output_lock = 0;

bool putchar_is_locked(void) {
    return __atomic_load_n(&output_lock, __ATOMIC_RELAXED) != 0;
}

void putchar_unlock(void) {
    __atomic_clear(&output_lock, __ATOMIC_RELEASE);
}

static uint32_t acquire_output_lock(void) {
    for (;;) {
        uint32_t flags = irq_save();
        if (!__atomic_test_and_set(&output_lock, __ATOMIC_ACQUIRE)) {
            return flags;
        }
        irq_restore(flags);
        if ((flags & 0x200u) && process_is_batch_active() && process_in_process_context()) {
            process_yield();
        } else {
            __builtin_ia32_pause();
        }
    }
}

static void release_output_lock(uint32_t flags) {
    __atomic_clear(&output_lock, __ATOMIC_RELEASE);
    irq_restore(flags);
}

static uint16_t putchar_scroll_line(process_t *curr, bool has_virtual, bool is_fg,
                                    uint16_t last_line_start) {
    if (has_virtual && (!is_fg || vga_is_graphics_mode())) {
        process_workspace_auto_scroll(curr);
    } else {
        terminal_auto_scroll();
    }
    return last_line_start;
}

static uint16_t putchar_render_glyph(process_t *curr, bool has_virtual, bool is_fg,
                                     volatile uint16_t *vga, uint16_t cursor,
                                     uint16_t terminal_bottom, uint16_t last_line_start,
                                     char c, uint8_t fg, uint8_t bg) {
    if (cursor < VGA_START_CURSOR_POSITION || cursor >= VGA_WIDTH * VGA_HEIGHT) {
        cursor = VGA_START_CURSOR_POSITION;
    }

    if (is_fg && !vga_is_graphics_mode()) {
        vga_cursor_erase();
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
            cursor = putchar_scroll_line(curr, has_virtual, is_fg, last_line_start);
        } else {
            cursor += spaces;
        }
    } else {
        if (cursor >= terminal_bottom) {
            cursor = putchar_scroll_line(curr, has_virtual, is_fg, last_line_start);
        }
        vga[cursor] = vga_entry((unsigned char)c, fg, bg);
        if (has_virtual && is_fg && !vga_is_graphics_mode()) {
            VGA_MEMORY[cursor] = vga_entry((unsigned char)c, fg, bg);
        }
        cursor++;
    }

    if (cursor >= terminal_bottom) {
        cursor = putchar_scroll_line(curr, has_virtual, is_fg, last_line_start);
    }

    if (cursor >= VGA_WIDTH * VGA_HEIGHT) {
        cursor = VGA_START_CURSOR_POSITION;
    }
    return cursor;
}

void putchar(char c) {
    putchar_color(c, VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

void putchar_color(char c, uint8_t fg, uint8_t bg) {
    serial_putc(c);

    process_t *curr = process_get_current();
    bool has_virtual = (process_get_separate_windows() && curr != NULL && curr->text_vram_backup != NULL);
    process_t *active_disp = process_get_separate_windows() ? process_get_displayed_foreground() : process_get_foreground();
    bool is_fg = (!has_virtual || curr == active_disp ||
                  (curr != NULL && active_disp != NULL && curr->window_id == active_disp->window_id));

    uint32_t irq_flags = acquire_output_lock();

    if (process_is_batch_active() && !process_get_separate_windows() && curr && !curr->wants_graphics && !curr->is_wm_app) {
        if (curr->text_output_len + 2 > curr->text_output_cap) {
            uint32_t ncap = curr->text_output_cap ? curr->text_output_cap * 2 : 256;
            char *nb = (char *)krealloc(curr->text_output_buf, ncap);
            if (nb) {
                curr->text_output_buf = nb;
                curr->text_output_cap = ncap;
            }
        }
        if (curr->text_output_buf && curr->text_output_len + 1 < curr->text_output_cap) {
            curr->text_output_buf[curr->text_output_len++] = c;
            curr->text_output_buf[curr->text_output_len] = '\0';
        }
        if (!vga_is_graphics_mode()) {
            curr->text_vga_written_len = curr->text_output_len;
        }
    }

    if (vga_is_graphics_mode() && !has_virtual) {
        release_output_lock(irq_flags);
        driver_dispatch_char_output(c);
        return;
    }

    if (is_fg && !vga_is_graphics_mode()) {
        if (terminal_get_bottom_buffer_count() > 0) {
            terminal_return_to_present();
        }
        terminal_on_external_output();
    }

    volatile uint16_t *vga = has_virtual ? curr->text_vram_backup : VGA_MEMORY;
    uint16_t cursor = has_virtual ? curr->text_cursor_pos : vga_get_cursor_position();
    uint16_t top_row = VGA_START_CURSOR_POSITION / VGA_WIDTH;
    uint16_t terminal_rows = VGA_HEIGHT - top_row;
    uint16_t terminal_bottom = (top_row + terminal_rows) * VGA_WIDTH;
    uint16_t last_line_start = (top_row + terminal_rows - 1) * VGA_WIDTH;

    c = driver_dispatch_char_output(c);

    static utf_stream_t k_utf_stream = {0};
    utf_stream_t *ustream = curr ? &curr->utf_stream : &k_utf_stream;

    uint64_t codepoint = 0;
    if (!utf_stream_push_byte(ustream, (uint8_t)c, &codepoint)) {
        release_output_lock(irq_flags);
        return;
    }

    uint8_t out_glyph = utf_codepoint_to_vga_glyph(codepoint);
    cursor = putchar_render_glyph(curr, has_virtual, is_fg, vga, cursor,
                                  terminal_bottom, last_line_start,
                                  (char)out_glyph, fg, bg);

    while (utf_stream_flush_next(ustream, &codepoint)) {
        out_glyph = utf_codepoint_to_vga_glyph(codepoint);
        cursor = putchar_render_glyph(curr, has_virtual, is_fg, vga, cursor,
                                      terminal_bottom, last_line_start,
                                      (char)out_glyph, fg, bg);
    }

    if (has_virtual) {
        curr->text_cursor_pos = cursor;
        process_sync_workspace_text_state(curr);
    }
    if (is_fg && !vga_is_graphics_mode()) {
        vga_set_cursor(cursor);
    }
    release_output_lock(irq_flags);
}
