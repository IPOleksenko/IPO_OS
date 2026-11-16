#include <stdio.h>
#include <arch/cpu_regs.h>
#include <vga.h>
#include <vga_gfx.h>
#include <wm.h>
#include <ioport.h>
#include <kernel/terminal.h>
#include <kernel/driver.h>
#include <kernel/process.h>
#include <kernel/smp.h>
#include <kernel/utf.h>
#include <system/timer.h>
#include <system/state.h>
#include <memory/kmalloc.h>
#include <string.h>

static volatile uint8_t output_lock = 0;

bool putchar_is_locked(void) {
    return __atomic_load_n(&output_lock, __ATOMIC_RELAXED) != 0;
}

void putchar_unlock(void) {
    __atomic_clear(&output_lock, __ATOMIC_RELEASE);
}

static ipo_reg_t acquire_output_lock(void) {
    for (;;) {
        ipo_reg_t flags = irq_save();
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

static void release_output_lock(ipo_reg_t flags) {
    __atomic_clear(&output_lock, __ATOMIC_RELEASE);
    irq_restore(flags);
}

static uint16_t putchar_scroll_line(process_t *curr, bool has_virtual, bool is_fg,
                                    bool using_saved_gfx_text, uint16_t last_line_start) {
    if (has_virtual && (!is_fg || vga_is_graphics_mode())) {
        process_workspace_auto_scroll(curr);
    } else if (using_saved_gfx_text) {
        vga_gfx_scroll_saved_text();
    } else {
        terminal_auto_scroll();
    }
    return last_line_start;
}

static uint16_t putchar_render_glyph(process_t *curr, bool has_virtual, bool is_fg,
                                     bool using_saved_gfx_text,
                                     volatile uint16_t *vga, uint16_t cursor,
                                     uint16_t terminal_bottom, uint16_t last_line_start,
                                     char c, uint8_t fg, uint8_t bg) {
    if (cursor < VGA_START_CURSOR_POSITION || cursor >= VGA_WIDTH * VGA_HEIGHT) {
        cursor = VGA_START_CURSOR_POSITION;
    }

    if (using_saved_gfx_text && (vga[cursor] & 0xFFu) == VGA_CURSOR_GLYPH_SLOT) {
        vga[cursor] = (uint16_t)((vga[cursor] & 0xFF00u) | ' ');
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
            cursor = putchar_scroll_line(curr, has_virtual, is_fg, using_saved_gfx_text, last_line_start);
        } else {
            cursor += spaces;
        }
    } else {
        if (cursor >= terminal_bottom) {
            cursor = putchar_scroll_line(curr, has_virtual, is_fg, using_saved_gfx_text, last_line_start);
        }
        vga[cursor] = vga_entry((unsigned char)c, fg, bg);
        if (has_virtual && is_fg && !vga_is_graphics_mode()) {
            VGA_MEMORY[cursor] = vga_entry((unsigned char)c, fg, bg);
        }
        cursor++;
    }

    if (cursor >= terminal_bottom) {
        cursor = putchar_scroll_line(curr, has_virtual, is_fg, using_saved_gfx_text, last_line_start);
    }

    if (cursor >= VGA_WIDTH * VGA_HEIGHT) {
        cursor = VGA_START_CURSOR_POSITION;
    }
    return cursor;
}

static void putchar_write_color_ex(const char *str, ipo_size_t len, uint8_t fg, uint8_t bg,
                                   bool to_serial, bool capture_buf) {
    if (!str || len == 0u) {
        return;
    }

    if (to_serial) {
        for (ipo_size_t i = 0u; i < len; i++) {
            serial_putc(str[i]);
        }
    }

    process_t *curr = process_get_current();
    bool has_virtual = (process_get_separate_windows() && curr != NULL && curr->text_vram_backup != NULL);
    process_t *active_disp = process_get_separate_windows() ? process_get_displayed_foreground() : process_get_foreground();
    bool is_fg = (!has_virtual || curr == active_disp ||
                  (curr != NULL && active_disp != NULL && curr->window_id == active_disp->window_id));

    ipo_reg_t irq_flags = acquire_output_lock();

    if (capture_buf && process_is_batch_active() && curr && !curr->wants_graphics && !curr->is_wm_app) {
        process_append_text_output(curr, str, len);
    }

    uint16_t *saved_vram = NULL;
    size_t saved_cells = 0;
    uint16_t *saved_cur = NULL;
    bool using_saved_gfx_text = false;
    if (!has_virtual && vga_is_graphics_mode()) {
        size_t need_cells = (size_t)VGA_WIDTH * (size_t)VGA_HEIGHT;
        if (vga_gfx_get_saved_text_buffer(&saved_vram, &saved_cells, &saved_cur) &&
            saved_cells >= need_cells) {
            using_saved_gfx_text = true;
        } else {
            release_output_lock(irq_flags);
            for (ipo_size_t i = 0u; i < len; i++) {
                driver_dispatch_char_output(str[i]);
            }
            return;
        }
    }

    if (curr && !curr->wants_graphics && !curr->is_wm_app &&
        process_is_batch_active() && !process_get_separate_windows() &&
        (!vga_is_graphics_mode() || using_saved_gfx_text)) {
        curr->text_vga_written_len = curr->text_output_len;
    }

    if (is_fg && !vga_is_graphics_mode()) {
        if (terminal_get_bottom_buffer_count() > 0) {
            terminal_return_to_present();
        }
        terminal_on_external_output();
        vga_cursor_erase();
    }

    volatile uint16_t *vga = has_virtual ? curr->text_vram_backup
                                         : (using_saved_gfx_text ? saved_vram : VGA_MEMORY);
    uint16_t cursor = has_virtual ? curr->text_cursor_pos
                                  : (using_saved_gfx_text ? *saved_cur : vga_get_cursor_position());
    uint16_t top_row = VGA_START_CURSOR_POSITION / VGA_WIDTH;
    uint16_t terminal_rows = VGA_HEIGHT - top_row;
    uint16_t terminal_bottom = (top_row + terminal_rows) * VGA_WIDTH;
    uint16_t last_line_start = (top_row + terminal_rows - 1) * VGA_WIDTH;

    static utf_stream_t k_utf_stream = {0};
    utf_stream_t *ustream = curr ? &curr->utf_stream : &k_utf_stream;

    for (ipo_size_t i = 0u; i < len; i++) {
        char c = driver_dispatch_char_output(str[i]);
        uint64_t codepoint = 0;
        if (!utf_stream_push_byte(ustream, (uint8_t)c, &codepoint)) {
            continue;
        }

        uint8_t out_glyph = utf_codepoint_to_vga_glyph(codepoint);
        cursor = putchar_render_glyph(curr, has_virtual, is_fg, using_saved_gfx_text,
                                      vga, cursor, terminal_bottom, last_line_start,
                                      (char)out_glyph, fg, bg);

        while (utf_stream_flush_next(ustream, &codepoint)) {
            out_glyph = utf_codepoint_to_vga_glyph(codepoint);
            cursor = putchar_render_glyph(curr, has_virtual, is_fg, using_saved_gfx_text,
                                          vga, cursor, terminal_bottom, last_line_start,
                                          (char)out_glyph, fg, bg);
        }
    }

    if (has_virtual) {
        curr->text_cursor_pos = cursor;
        process_sync_workspace_text_state(curr);
    }
    if (using_saved_gfx_text && saved_cur) {
        *saved_cur = cursor;
    }
    if (is_fg && !vga_is_graphics_mode()) {
        vga_set_cursor(cursor);
    }
    release_output_lock(irq_flags);
}

void putchar_color(char c, uint8_t fg, uint8_t bg) {
    putchar_write_color_ex(&c, 1u, fg, bg, true, true);
}

void putchar(char c) {
    putchar_color(c, VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

void putchar_write_color(const char *str, ipo_size_t len, uint8_t fg, uint8_t bg) {
    putchar_write_color_ex(str, len, fg, bg, true, true);
}

void putchar_write(const char *str, ipo_size_t len) {
    putchar_write_color_ex(str, len, VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK, true, true);
}

void putchar_vga_replay(const char *str, ipo_size_t len) {
    putchar_write_color_ex(str, len, VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK, false, false);
}
