#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return (int)p1[i] - (int)p2[i];
        }
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t *)s;
    uint8_t target = (uint8_t)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == target) {
            return (void *)(p + i);
        }
    }
    return NULL;
}

void bzero(void *s, size_t n) {
    memset(s, 0, n);
}

size_t strlen(const char *s) {
    if (!s) return 0;
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

char *strcpy(char *dest, const char *src) {
    if (!dest || !src) return dest;
    size_t i = 0;
    while (src[i]) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    if (!dest) return dest;
    size_t i = 0;
    if (src) {
        while (i < n && src[i]) {
            dest[i] = src[i];
            i++;
        }
    }
    while (i < n) {
        dest[i] = '\0';
        i++;
    }
    return dest;
}

char *strcat(char *dest, const char *src) {
    if (!dest || !src) return dest;
    size_t dlen = strlen(dest);
    size_t i = 0;
    while (src[i]) {
        dest[dlen + i] = src[i];
        i++;
    }
    dest[dlen + i] = '\0';
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    if (!dest || !src || n == 0) return dest;
    size_t dlen = strlen(dest);
    size_t i = 0;
    while (i < n && src[i]) {
        dest[dlen + i] = src[i];
        i++;
    }
    dest[dlen + i] = '\0';
    return dest;
}

int strcmp(const char *s1, const char *s2) {
    if (s1 == s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) return 0;
    if (s1 == s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) {
            return (int)(unsigned char)s1[i] - (int)(unsigned char)s2[i];
        }
        if (s1[i] == '\0') break;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    if (!s) return NULL;
    char ch = (char)c;
    while (*s) {
        if (*s == ch) return (char *)s;
        s++;
    }
    return (ch == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    if (!s) return NULL;
    char ch = (char)c;
    const char *last = NULL;
    while (*s) {
        if (*s == ch) last = s;
        s++;
    }
    return (ch == '\0') ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen(needle);
    while (*haystack) {
        if (strncmp(haystack, needle, nlen) == 0) {
            return (char *)haystack;
        }
        haystack++;
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept) {
    if (!s || !accept) return 0;
    size_t count = 0;
    while (*s && strchr(accept, *s)) {
        count++;
        s++;
    }
    return count;
}

size_t strcspn(const char *s, const char *reject) {
    if (!s || !reject) return 0;
    size_t count = 0;
    while (*s && !strchr(reject, *s)) {
        count++;
        s++;
    }
    return count;
}

char *strpbrk(const char *s, const char *accept) {
    if (!s || !accept) return NULL;
    while (*s) {
        if (strchr(accept, *s)) return (char *)s;
        s++;
    }
    return NULL;
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    if (!saveptr) return NULL;
    char *s = str ? str : *saveptr;
    if (!s) return NULL;

    while (*s && strchr(delim, *s)) s++;
    if (!*s) {
        *saveptr = NULL;
        return NULL;
    }

    char *start = s;
    while (*s && !strchr(delim, *s)) s++;
    if (*s) {
        *s = '\0';
        *saveptr = s + 1;
    } else {
        *saveptr = NULL;
    }
    return start;
}

char *strtok(char *str, const char *delim) {
    static char *last = NULL;
    return strtok_r(str, delim, &last);
}

char *strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *p = (char *)malloc(len + 1);
    if (p) {
        memcpy(p, s, len + 1);
    }
    return p;
}

char *strndup(const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *p = (char *)malloc(len + 1);
    if (p) {
        memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}

char *strerror(int errnum) {
    switch (errnum) {
        case 0: return "Success";
        case 1: return "Operation not permitted";
        case 2: return "No such file or directory";
        case 5: return "Input/output error";
        case 12: return "Cannot allocate memory";
        case 13: return "Permission denied";
        case 17: return "File exists";
        case 22: return "Invalid argument";
        case 28: return "No space left on device";
        case 38: return "Function not implemented";
        default: return "Unknown error";
    }
}

int strcoll(const char *s1, const char *s2) {
    return strcmp(s1, s2);
}

size_t strxfrm(char *dest, const char *src, size_t n) {
    if (dest && n > 0) {
        strncpy(dest, src, n);
    }
    return strlen(src);
}

#include <locale.h>

static struct lconv default_lconv = {
    .decimal_point = ".",
    .thousands_sep = "",
    .grouping = "",
    .int_curr_symbol = "",
    .currency_symbol = "",
    .mon_decimal_point = "",
    .mon_thousands_sep = "",
    .mon_grouping = "",
    .positive_sign = "",
    .negative_sign = "",
    .int_frac_digits = 127,
    .frac_digits = 127,
    .p_cs_precedes = 127,
    .p_sep_by_space = 127,
    .n_cs_precedes = 127,
    .n_sep_by_space = 127,
    .p_sign_posn = 127,
    .n_sign_posn = 127
};

char *setlocale(int category, const char *locale) {
    (void)category;
    (void)locale;
    return "C";
}

struct lconv *localeconv(void) {
    return &default_lconv;
}

int strcasecmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return s1 ? 1 : (s2 ? -1 : 0);
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) return 0;
    if (!s1 || !s2) return s1 ? 1 : (s2 ? -1 : 0);
    while (n-- > 0) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2 || *s1 == '\0') return c1 - c2;
        s1++;
        s2++;
    }
    return 0;
}

char *strsep(char **stringp, const char *delim) {
    if (!stringp || !*stringp) return NULL;
    char *start = *stringp;
    char *end = strpbrk(start, delim);
    if (end) {
        *end = '\0';
        *stringp = end + 1;
    } else {
        *stringp = NULL;
    }
    return start;
}

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t srclen = strlen(src);
    if (size > 0) {
        size_t copylen = (srclen >= size) ? size - 1 : srclen;
        memcpy(dst, src, copylen);
        dst[copylen] = '\0';
    }
    return srclen;
}

size_t strlcat(char *dst, const char *src, size_t size) {
    size_t dstlen = strlen(dst);
    size_t srclen = strlen(src);
    if (dstlen >= size) return size + srclen;
    size_t copylen = (dstlen + srclen >= size) ? size - dstlen - 1 : srclen;
    memcpy(dst + dstlen, src, copylen);
    dst[dstlen + copylen] = '\0';
    return dstlen + srclen;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && s[len]) len++;
    return len;
}

static int32_t toupper_table[384];
static int32_t tolower_table[384];
static unsigned short ctype_b_table[384];
static int ctype_tables_initialized = 0;

static void init_ctype_tables(void) {
    if (ctype_tables_initialized) return;
    for (int i = 0; i < 384; i++) {
        int c = i - 128;
        toupper_table[i] = (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
        tolower_table[i] = (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;

        unsigned short mask = 0;
        if (c >= 'A' && c <= 'Z') mask |= 0x0100;
        if (c >= 'a' && c <= 'z') mask |= 0x0200;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) mask |= 0x0400;
        if (c >= '0' && c <= '9') mask |= 0x0800;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) mask |= 0x1000;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') mask |= 0x2000;
        if (c >= 0x20 && c <= 0x7E) mask |= 0x4000;
        if (c > 0x20 && c <= 0x7E) mask |= 0x8000;
        if (c == ' ' || c == '\t') mask |= 0x0001;
        if ((c >= 0 && c <= 0x1F) || c == 0x7F) mask |= 0x0002;
        if ((mask & 0x0400) || (mask & 0x0800)) mask |= 0x0008;
        ctype_b_table[i] = mask;
    }
    ctype_tables_initialized = 1;
}

const int32_t **__ctype_toupper_loc(void) {
    init_ctype_tables();
    static const int32_t *p = &toupper_table[128];
    return &p;
}

const int32_t **__ctype_tolower_loc(void) {
    init_ctype_tables();
    static const int32_t *p = &tolower_table[128];
    return &p;
}

const unsigned short **__ctype_b_loc(void) {
    init_ctype_tables();
    static const unsigned short *p = &ctype_b_table[128];
    return &p;
}
