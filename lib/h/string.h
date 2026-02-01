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

/**
 * Copy up to n characters
 */
char* strncpy(char *dest, const char *src, size_t n);

/**
 * Compare strings
 */
int strcmp(const char *a, const char *b);

/**
 * Compare strings with length
 */
int strncmp(const char *a, const char *b, size_t n);

/**
 * Find character in string
 */
char *strchr(const char *s, int c);

/* Compare memory areas */
int memcmp(const void *a, const void *b, size_t n);

#endif
