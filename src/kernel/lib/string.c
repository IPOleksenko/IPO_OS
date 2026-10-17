#include <string.h>
#include <stdio.h>
#include <memory/kmalloc.h>

char* strcpy(char *dest, const char *src) {
    char *d = dest;
    while (*src) {
        *d++ = *src++;
    }
    *d = '\0';
    return dest;
}

char* strncpy(char *dest, const char *src, size_t n) {
    size_t i = 0;
    while (i < n && src[i]) {
        dest[i] = src[i];
        i++;
    }
    while (i < n) dest[i++] = '\0';
    return dest;
}

int strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}

int strncmp(const char *a, const char *b, size_t n) {
    size_t i = 0;
    while (i < n && *a && *b && *a == *b) { a++; b++; i++; }
    if (i == n) return 0;
    return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}

char *strchr(const char *s, int c) {
    char cc = (char)c;
    while (*s) {
        if (*s == cc) return (char *)s;
        s++;
    }
    if (cc == '\0') return (char *)s;
    return NULL;
}

char *strrchr(const char *s, int c) {
    char cc = (char)c;
    const char *last = NULL;
    while (*s) {
        if (*s == cc) last = s;
        s++;
    }
    if (cc == '\0') return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) {
        return NULL;
    }

    size_t needle_len = strlen(needle);
    if (needle_len == 0) {
        return (char *)haystack;
    }

    for (const char *p = haystack; *p; p++) {
        if (strncmp(p, needle, needle_len) == 0) {
            return (char *)p;
        }
    }

    return NULL;
}

void* memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    for (size_t i = 0; i < n; i++) {
        p[i] = (unsigned char)c;
    }
    return s;
}

void* memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

void* memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

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

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

__attribute__((weak)) int atoi(const char *str) {
    if (!str) return 0;
    while (*str == ' ' || *str == '\t') str++;
    int sign = 1;
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    int res = 0;
    while (*str >= '0' && *str <= '9') {
        res = res * 10 + (*str - '0');
        str++;
    }
    return res * sign;
}

void print_padded(const char *s, int width) {
    int len = 0;
    if (s) {
        while (s[len]) {
            putchar(s[len]);
            len++;
        }
    }
    if (len >= width) {
        putchar(' ');
        return;
    }
    while (len < width) {
        putchar(' ');
        len++;
    }
}

char *str_trim(char *str) {
    if (!str) return NULL;
    char *start = str;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (*start == '\0') {
        str[0] = '\0';
        return str;
    }
    char *end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
    return str;
}

int parse_int(const char *str, int *out_val) {
    if (!str) return -1;
    while (*str == ' ' || *str == '\t') str++;
    if (!*str) return -1;

    int sign = 1;
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    if (!(*str >= '0' && *str <= '9')) return -1;

    int val = 0;
    while (*str >= '0' && *str <= '9') {
        val = val * 10 + (*str - '0');
        str++;
    }
    if (out_val) *out_val = val * sign;
    return 0;
}

int parse_uint(const char *str, uint32_t *out_val) {
    if (!str) return -1;
    while (*str == ' ' || *str == '\t') str++;
    if (!*str) return -1;

    if (*str == '0' && (*(str + 1) == 'x' || *(str + 1) == 'X')) {
        return parse_hex(str, out_val);
    }

    if (!(*str >= '0' && *str <= '9')) return -1;
    uint32_t val = 0;
    while (*str >= '0' && *str <= '9') {
        val = val * 10u + (uint32_t)(*str - '0');
        str++;
    }
    if (out_val) *out_val = val;
    return 0;
}

int parse_hex(const char *str, uint32_t *out_val) {
    if (!str) return -1;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '0' && (*(str + 1) == 'x' || *(str + 1) == 'X')) {
        str += 2;
    }
    if (!*str) return -1;

    uint32_t val = 0;
    int digits = 0;
    while (*str) {
        char c = *str;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else break;
        val = (val << 4) | d;
        digits++;
        str++;
    }
    if (digits == 0) return -1;
    if (out_val) *out_val = val;
    return 0;
}

int format_size(uint64_t bytes, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return 0;
    if (bytes >= (1024ull * 1024ull * 1024ull)) {
        uint32_t gb = (uint32_t)(bytes / (1024ull * 1024ull * 1024ull));
        uint32_t rem_mb = (uint32_t)(((bytes % (1024ull * 1024ull * 1024ull)) * 10ull) / (1024ull * 1024ull * 1024ull));
        return snprintf(buf, buf_size, "%u.%u GB", gb, rem_mb);
    } else if (bytes >= (1024ull * 1024ull)) {
        uint32_t mb = (uint32_t)(bytes / (1024ull * 1024ull));
        return snprintf(buf, buf_size, "%u MB", mb);
    } else if (bytes >= 1024ull) {
        uint32_t kb = (uint32_t)(bytes / 1024ull);
        return snprintf(buf, buf_size, "%u KB", kb);
    } else {
        return snprintf(buf, buf_size, "%u B", (uint32_t)bytes);
    }
}

char *format_size_dynamic(uint64_t bytes) {
    size_t sz = 64;
    char *buf = (char *)kmalloc(sz);
    if (!buf) return NULL;
    format_size(bytes, buf, sz);
    return buf;
}

int cmdline_split(const char *cmdline, int *out_argc, char ***out_argv) {
    if (out_argc) *out_argc = 0;
    if (out_argv) *out_argv = NULL;
    if (!cmdline) return -1;

    const char *p = cmdline;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return 0;

    int argc = 0;
    int argv_cap = 8;
    char **argv = (char **)kmalloc((size_t)argv_cap * sizeof(char *));
    if (!argv) return -1;

    size_t arg_cap = 128;
    char *arg_buf = (char *)kmalloc(arg_cap);
    if (!arg_buf) {
        kfree(argv);
        return -1;
    }
    size_t arg_pos = 0;
    bool in_arg = false;
    char in_quote = '\0';

    while (*p) {
        char c = *p;

        if (in_quote) {
            if (c == in_quote) {
                in_quote = '\0';
            } else if (c == '\\' && (*(p + 1) == in_quote || *(p + 1) == '\\')) {
                p++;
                c = *p;
                if (arg_pos + 1 >= arg_cap) {
                    size_t new_cap = arg_cap * 2;
                    char *ext = (char *)kmalloc(new_cap);
                    if (ext) { memcpy(ext, arg_buf, arg_pos); kfree(arg_buf); arg_buf = ext; arg_cap = new_cap; }
                }
                arg_buf[arg_pos++] = c;
            } else {
                if (arg_pos + 1 >= arg_cap) {
                    size_t new_cap = arg_cap * 2;
                    char *ext = (char *)kmalloc(new_cap);
                    if (ext) { memcpy(ext, arg_buf, arg_pos); kfree(arg_buf); arg_buf = ext; arg_cap = new_cap; }
                }
                arg_buf[arg_pos++] = c;
            }
            p++;
            in_arg = true;
            continue;
        }

        if (c == '"' || c == '\'') {
            in_quote = c;
            in_arg = true;
            p++;
            continue;
        }

        if (c == '\\' && *(p + 1) != '\0') {
            char next = *(p + 1);
            if (next == ' ' || next == '\t' || next == '"' || next == '\'' || next == '\\' || next == '&' || next == ';' || next == '|' || next == '>' || next == '<') {
                p++;
                c = *p;
            }
            if (arg_pos + 1 >= arg_cap) {
                size_t new_cap = arg_cap * 2;
                char *ext = (char *)kmalloc(new_cap);
                if (ext) { memcpy(ext, arg_buf, arg_pos); kfree(arg_buf); arg_buf = ext; arg_cap = new_cap; }
            }
            arg_buf[arg_pos++] = c;
            p++;
            in_arg = true;
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (in_arg) {
                arg_buf[arg_pos] = '\0';
                char *copy = (char *)kmalloc(arg_pos + 1);
                if (copy) {
                    memcpy(copy, arg_buf, arg_pos + 1);
                    if (argc + 1 >= argv_cap) {
                        int new_cap = argv_cap * 2;
                        char **new_argv = (char **)kmalloc((size_t)new_cap * sizeof(char *));
                        if (new_argv) {
                            memcpy(new_argv, argv, (size_t)argc * sizeof(char *));
                            kfree(argv);
                            argv = new_argv;
                            argv_cap = new_cap;
                        }
                    }
                    argv[argc++] = copy;
                }
                arg_pos = 0;
                in_arg = false;
            }
            p++;
            continue;
        }

        if (arg_pos + 1 >= arg_cap) {
            size_t new_cap = arg_cap * 2;
            char *ext = (char *)kmalloc(new_cap);
            if (ext) { memcpy(ext, arg_buf, arg_pos); kfree(arg_buf); arg_buf = ext; arg_cap = new_cap; }
        }
        arg_buf[arg_pos++] = c;
        p++;
        in_arg = true;
    }

    if (in_arg) {
        arg_buf[arg_pos] = '\0';
        char *copy = (char *)kmalloc(arg_pos + 1);
        if (copy) {
            memcpy(copy, arg_buf, arg_pos + 1);
            if (argc + 1 >= argv_cap) {
                int new_cap = argv_cap * 2;
                char **new_argv = (char **)kmalloc((size_t)new_cap * sizeof(char *));
                if (new_argv) {
                    memcpy(new_argv, argv, (size_t)argc * sizeof(char *));
                    kfree(argv);
                    argv = new_argv;
                    argv_cap = new_cap;
                }
            }
            argv[argc++] = copy;
        }
    }

    kfree(arg_buf);
    argv[argc] = NULL;

    if (out_argc) *out_argc = argc;
    if (out_argv) *out_argv = argv;
    return argc;
}

void cmdline_free_args(int argc, char **argv) {
    if (!argv) return;
    for (int i = 0; i < argc; i++) {
        if (argv[i]) kfree(argv[i]);
    }
    kfree(argv);
}

char* strcat(char *dest, const char *src) {
    if (!dest || !src) return dest;
    char *rdest = dest;
    while (*dest) dest++;
    while ((*dest++ = *src++));
    return rdest;
}

