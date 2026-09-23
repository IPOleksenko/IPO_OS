#ifndef DOOM_COMPAT_H
#define DOOM_COMPAT_H

#ifndef __BYTEBOOL__
#define __BYTEBOOL__
typedef int boolean;
#ifndef false
#define false 0
#endif
#ifndef true
#define true 1
#endif
typedef unsigned char byte;
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#ifndef alloca
#define alloca(size) __builtin_alloca(size)
#endif

#ifdef __cplusplus
extern "C" {
#endif

char *strcat(char *dest, const char *src);
int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* DOOM_COMPAT_H */
