#ifndef _STDIO_H
#define _STDIO_H

#include <stdint.h>
#include <stdarg.h>

#include <stddef.h>

/**
 * Low-level serial output (COM1)
 * @param c Character to output
 */
void serial_putc(char c);

/**
 * Output a single character to VGA memory at cursor position
 * @param c Character to output
 */
void putchar(char c);

/**
 * Formatted print function
 * Supports: %d (int), %u (unsigned), %x (hex), %c (char), %s (string), %%
 * @param format Format string
 * @param ... Variable arguments
 * @return Number of characters printed
 */
int printf(const char *format, ...);

/**
 * Formatted print to buffer
 * @param buf Output buffer
 * @param size Maximum size to write (including null terminator)
 * @param format Format string
 * @param ... Variable arguments
 * @return Number of characters written (excluding null terminator)
 */
int snprintf(char *buf, size_t size, const char *format, ...);

/**
 * Convert unsigned integer to string (internal use)
 */
int itoa(unsigned int num, char *str, int base);

/**
 * Convert unsigned 64-bit integer to string (internal use)
 */
int itoa64(uint64_t num, char *str, int base);

#endif
