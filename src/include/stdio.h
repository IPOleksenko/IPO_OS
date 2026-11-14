#ifndef _STDIO_H
#define _STDIO_H

#include <stdint.h>
#include <stdarg.h>

#include <stddef.h>
#include <stdbool.h>

/**
 * Output a single character to VGA memory at cursor position with specified colors
 * @param c Character to output
 * @param fg Foreground color
 * @param bg Background color
 */
void putchar_color(char c, uint8_t fg, uint8_t bg);

bool putchar_is_locked(void);
void putchar_unlock(void);

void serial_init(void);
void serial_putc_raw(char c);
void serial_putc(char c);
int serial_getc(void);
void putchar(char c);

int stdio_vformat(void (*emit)(void *ctx, char ch), void *ctx, const char *format, va_list args);
int printf(const char *format, ...);
int scanf(const char *format, ...);
int snprintf(char *buf, size_t size, const char *format, ...);
int vsnprintf(char *buf, size_t size, const char *format, va_list args);
int serial_printf(const char *format, ...);

#endif
