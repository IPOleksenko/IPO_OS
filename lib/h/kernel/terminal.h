#ifndef _TERMINAL_H
#define _TERMINAL_H

#include <stdint.h>

void print_header(void);

void terminal_initialize(void);

void handle_control_char(char c);

void terminal_console(void);

/* Print a line to the terminal history */
void terminal_print(const char *s);

#endif
