#ifndef _TERMINAL_H
#define _TERMINAL_H

#include <stdint.h>

void print_header(void);

void terminal_initialize(void);

void handle_control_char(char c);

void terminal_console(void);

void terminal_print(const char *s);

void terminal_auto_scroll(void);

void async_scheduler_init(void);
void async_scheduler_tick(void);
int async_start_task(const char *name, uint32_t interval_ms, void (*fn)(void));
int async_stop_task(const char *name);

int try_execute_command(const char *cmd);

char* resolve_command_path(const char *cmd);

#endif
