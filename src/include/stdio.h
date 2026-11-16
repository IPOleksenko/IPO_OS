#ifndef _STDIO_H
#define _STDIO_H

#include <stdint.h>
#include <stdarg.h>

#include <stddef.h>
#include <stdbool.h>
#include <types.h>

/* Write a character to VGA text memory at the current cursor with fg/bg colors */
void putchar_color(char c, uint8_t fg, uint8_t bg);

bool putchar_is_locked(void);
void putchar_unlock(void);

void serial_init(void);
void serial_putc_raw(char c);
void serial_putc(char c);
int serial_getc(void);
void putchar(char c);
void putchar_write_color(const char *str, ipo_size_t len, uint8_t fg, uint8_t bg);
void putchar_write(const char *str, ipo_size_t len);
void putchar_vga_replay(const char *str, ipo_size_t len);

int stdio_vformat(void (*emit)(void *ctx, char ch), void *ctx, const char *format, va_list args);
int printf(const char *format, ...);
int scanf(const char *format, ...);
int snprintf(char *buf, size_t size, const char *format, ...);
int vsnprintf(char *buf, size_t size, const char *format, va_list args);
int serial_printf(const char *format, ...);

#endif
