#include <stdio.h>
#include <syscall.h>
#include <string.h>
#include <stdint.h>
#include <memory/kmalloc.h>

static void *xrealloc(void *ptr, size_t old_size, size_t new_size) {
    if (new_size == 0u) {
        if (ptr != NULL) {
            kfree(ptr);
        }
        return NULL;
    }

    void *new_ptr = kmalloc(new_size);
    if (new_ptr == NULL) {
        return NULL;
    }

    if (ptr != NULL) {
        size_t copy_size = old_size < new_size ? old_size : new_size;
        memcpy(new_ptr, ptr, copy_size);
        kfree(ptr);
    }

    return new_ptr;
}

static const char *skip_spaces(const char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
        text++;
    }
    return text;
}

int scanf(const char *format, ...) {
    if (format == NULL) {
        return -1;
    }

    size_t capacity = 256u;
    char *input = kmalloc(capacity);
    if (input == NULL) {
        return -1;
    }

    size_t length = 0u;
    while (1) {
        if (length + 128u >= capacity) {
            size_t new_capacity = capacity ? capacity * 2u : 256u;
            char *larger = xrealloc(input, capacity, new_capacity);
            if (larger == NULL) {
                kfree(input);
                return -1;
            }
            input = larger;
            capacity = new_capacity;
        }

        int chunk = ipo_syscall(IPO_SYSCALL_READ,
                                (uint32_t)(uintptr_t)(input + length),
                                128u,
                                0, 0, 0);
        if (chunk <= 0) {
            break;
        }

        length += (size_t)chunk;
        input[length] = '\0';

        if (length > 0u && (input[length - 1u] == '\n' || input[length - 1u] == '\r')) {
            break;
        }
    }

    if (length == 0u) {
        kfree(input);
        return -1;
    }

    while (length > 0u && (input[length - 1u] == '\n' || input[length - 1u] == '\r')) {
        input[--length] = '\0';
    }

    va_list args;
    va_start(args, format);

    int assigned = 0;
    const char *pattern = format;
    const char *text = input;

    while (*pattern != '\0') {
        if (*pattern == '%') {
            pattern++;
            if (*pattern == '\0') {
                break;
            }

            text = skip_spaces(text);

            if (*pattern == 'd') {
                int sign = 1;
                int value = 0;
                int digits = 0;
                int *output = va_arg(args, int *);

                if (*text == '-') {
                    sign = -1;
                    text++;
                } else if (*text == '+') {
                    text++;
                }

                while (*text >= '0' && *text <= '9') {
                    value = value * 10 + (*text - '0');
                    text++;
                    digits++;
                }
                if (digits == 0) {
                    break;
                }
                *output = value * sign;
                assigned++;
            } else if (*pattern == 'u') {
                uint32_t value = 0;
                int digits = 0;
                unsigned int *output = va_arg(args, unsigned int *);

                while (*text >= '0' && *text <= '9') {
                    value = value * 10u + (uint32_t)(*text - '0');
                    text++;
                    digits++;
                }
                if (digits == 0) {
                    break;
                }
                *output = (unsigned int)value;
                assigned++;
            } else if (*pattern == 's') {
                char *output = va_arg(args, char *);
                size_t copied = 0u;

                while (*text != '\0' && *text != ' ' && *text != '\t' &&
                       *text != '\n' && *text != '\r') {
                    output[copied++] = *text++;
                }
                if (copied == 0u) {
                    break;
                }
                output[copied] = '\0';
                assigned++;
            } else if (*pattern == 'c') {
                char *output = va_arg(args, char *);
                if (*text == '\0') {
                    break;
                }
                *output = *text++;
                assigned++;
            } else {
                break;
            }
        } else if (*pattern == ' ' || *pattern == '\t' || *pattern == '\n') {
            text = skip_spaces(text);
        } else {
            if (*text != *pattern) {
                break;
            }
            text++;
        }
        pattern++;
    }

    va_end(args);
    kfree(input);
    return assigned;
}
