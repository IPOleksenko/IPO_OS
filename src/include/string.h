#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * Standard string and memory routines
 */
char* strcpy(char *dest, const char *src);
char* strcat(char *dest, const char *src);
void* memset(void *s, int c, size_t n);
void* memcpy(void *dest, const void *src, size_t n);
void* memmove(void *dest, const void *src, size_t n);
size_t strlen(const char *s);
char* strncpy(char *dest, const char *src, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
int atoi(const char *str);
int memcmp(const void *a, const void *b, size_t n);

/**
 * Common shared formatting and parsing utilities (deduplicated)
 */
void print_padded(const char *s, int width);
char *str_trim(char *str);
int parse_int(const char *str, int *out_val);
int parse_uint(const char *str, uint32_t *out_val);
int parse_hex(const char *str, uint32_t *out_val);
int format_size(uint64_t bytes, char *buf, size_t buf_size);
char *format_size_dynamic(uint64_t bytes);

/* Command argument tokenization */
int cmdline_split(const char *cmdline, int *out_argc, char ***out_argv);
void cmdline_free_args(int argc, char **argv);

#endif /* _STRING_H */
