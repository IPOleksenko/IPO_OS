#ifndef _TERMINAL_H
#define _TERMINAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TERMINAL_SCROLL_HISTORY_SIZE 40960

void print_header_row(volatile uint16_t *row, size_t width);
void print_header(void);

void terminal_initialize(void);
void builtin_clear(void);
void terminal_clear_buffer(void);
void terminal_clear_command_history(void);
void terminal_lock_input(void);
void terminal_unlock_input(void);
bool terminal_is_input_locked(void);

void handle_control_char(char c);

void terminal_console(void);

void terminal_print(const char *s);

void terminal_auto_scroll(void);
void terminal_scroll_up(void);
void terminal_scroll_down(void);
void terminal_return_to_present(void);
int terminal_get_top_buffer_count(void);
int terminal_get_bottom_buffer_count(void);
void terminal_on_external_output(void);
size_t terminal_visual_offset_range(const char *buf, size_t len, size_t index);
void terminal_render_text_line(int32_t *start_cursor_ptr,
                               const char *buf, size_t len,
                               uint8_t fg_color, size_t *prev_vis_len_ptr,
                               size_t cursor_pos, uint16_t *mirror_vram,
                               bool mirror_proc_cursor);

int try_execute_command(const char *cmd);

bool terminal_is_builtin(const char *name);

char* resolve_command_path(const char *cmd);
const char *terminal_get_cwd(void);
int terminal_set_cwd(const char *path);

void terminal_inject_input(const char *text, bool auto_execute);
void terminal_apply_pending_input(void);
void terminal_poll_serial_commands(void);

void terminal_render_language_bar(void);
void terminal_language_bar_tick(void);
bool terminal_get_show_return_value(void);

#endif
