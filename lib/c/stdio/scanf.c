#include <stdio.h>
#include <syscall.h>
#include <string.h>
#include <stdint.h>

#define SCANF_BUFFER_SIZE 256

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

    char input[SCANF_BUFFER_SIZE];
    int length = ipo_syscall(IPO_SYSCALL_READ,
                             (uint32_t)(uintptr_t)input,
                             sizeof(input), 0, 0, 0);
    if (length < 0) {
        return -1;
    }
    input[length < SCANF_BUFFER_SIZE ? length : SCANF_BUFFER_SIZE - 1] = '\0';

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
                int copied = 0;

                while (*text != '\0' && *text != ' ' && *text != '\t' &&
                       *text != '\n' && *text != '\r') {
                    if (copied < SCANF_BUFFER_SIZE - 1) {
                        output[copied++] = *text;
                    }
                    text++;
                }
                if (copied == 0) {
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
    return assigned;
}
