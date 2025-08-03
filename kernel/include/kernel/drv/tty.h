#ifndef TTY_H
#define TTY_H

#include <string.h>
#include "vga.h"


#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define SCROLL_BUFFER_SIZE 1000

extern size_t terminal_row;
extern size_t terminal_column;
extern uint8_t terminal_color;
extern uint16_t* terminal_buffer;
extern uint16_t scroll_buffer[SCROLL_BUFFER_SIZE][VGA_HEIGHT * VGA_WIDTH];
extern size_t scroll_offset;
extern size_t scroll_buffer_pos;


void scroll_terminal(void);
void scroll_up(void);
void scroll_down(void);
void update_display(void);

void terminal_initialize(void);

void terminal_clear(void);

void terminal_setcolor(uint8_t color);

void terminal_set_cursor_position(uint16_t position);

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y);

void terminal_putchar(char c);

void terminal_write(const char* data, size_t size);

void terminal_writestring(const char* data);

void copyright_text();

#endif
