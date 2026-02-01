#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>

/**
 * Copy string from src to dest
 */
char* strcpy(char *dest, const char *src);

/**
 * Fill memory with a constant byte
 */
void* memset(void *s, int c, size_t n);

/**
 * Copy memory area
 */
void* memcpy(void *dest, const void *src, size_t n);

/**
 * Get string length
 */
size_t strlen(const char *s);

#endif
