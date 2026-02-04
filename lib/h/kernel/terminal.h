#ifndef _TERMINAL_H
#define _TERMINAL_H

#include <stdint.h>

void print_header(void);

void terminal_initialize(void);

void handle_control_char(char c);

void terminal_console(void);

void terminal_print(const char *s);

void terminal_auto_scroll(void);

int try_execute_command(const char *cmd);

char* resolve_command_path(const char *cmd);

#endif
