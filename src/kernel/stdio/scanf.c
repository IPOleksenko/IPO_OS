#include <stdio.h>
#include <syscall.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <memory/kmalloc.h>

static const char *skip_spaces(const char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
        text++;
    }
    return text;
}

static bool has_more_conversions(const char *p) {
    while (*p != '\0') {
        if (*p == '%') {
            if (*(p + 1) != '%') return true;
            p++;
        }
        p++;
    }
    return false;
}

int scanf(const char *format, ...) {
    if (format == NULL) {
        return -1;
    }

    char *input = NULL;

    uint32_t syscall_args[] = {
        (uint32_t)(uintptr_t)&input,
        0u
    };

    serial_printf("[scanf] waiting for input, format=\"%s\"\n", format);

    int bytes_read = ipo_syscall(
        IPO_SYSCALL_READ,
        2u,
        syscall_args
    );

    if (bytes_read == -2) {
        /* Ctrl+C — read was interrupted, propagate without printing anything */
        if (input != NULL) {
            kfree(input);
        }
        return -1;
    }

    if (bytes_read < 0 || input == NULL) {
        serial_printf("[scanf] syscall read failed (error %d)\n", bytes_read);
        if (input != NULL) {
            kfree(input);
        }
        return -1;
    }

    size_t length = (size_t)bytes_read;
    input[length] = '\0';

    while (length > 0u &&
           (input[length - 1u] == '\n' ||
            input[length - 1u] == '\r')) {
        input[--length] = '\0';
    }

    serial_printf("[scanf] received line=\"%s\" (len=%u)\n", input, length);

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

            if (*pattern == '%') {
                if (*text != '%') {
                    break;
                }
                text++;
                pattern++;
                continue;
            }

            size_t max_width = 0;
            while (*pattern >= '0' && *pattern <= '9') {
                max_width = max_width * 10u + (size_t)(*pattern - '0');
                pattern++;
            }
            if (*pattern == '\0') {
                break;
            }

            if (*pattern != 'c' && *pattern != '[') {
                text = skip_spaces(text);
            }

            if (*pattern == 'd') {
                int sign = 1;
                int value = 0;
                int digits = 0;
                size_t rem = max_width ? max_width : (size_t)-1;
                int *output = va_arg(args, int *);

                if (rem > 0 && *text == '-') {
                    sign = -1;
                    text++;
                    rem--;
                } else if (rem > 0 && *text == '+') {
                    text++;
                    rem--;
                }

                while (rem > 0 && *text >= '0' && *text <= '9') {
                    value = value * 10 + (*text - '0');
                    text++;
                    digits++;
                    rem--;
                }

                if (digits == 0) {
                    break;
                }

                if (output != NULL) {
                    *output = value * sign;
                    assigned++;
                }
            } else if (*pattern == 'u') {
                uint32_t value = 0;
                int digits = 0;
                size_t rem = max_width ? max_width : (size_t)-1;
                unsigned int *output = va_arg(args, unsigned int *);

                while (rem > 0 && *text >= '0' && *text <= '9') {
                    value = value * 10u + (uint32_t)(*text - '0');
                    text++;
                    digits++;
                    rem--;
                }

                if (digits == 0) {
                    break;
                }

                if (output != NULL) {
                    *output = (unsigned int)value;
                    assigned++;
                }
            } else if (*pattern == 'f') {
                double sign = 1.0;
                double value = 0.0;
                int digits = 0;
                size_t rem = max_width ? max_width : (size_t)-1;
                float *output = va_arg(args, float *);

                if (rem > 0 && *text == '-') {
                    sign = -1.0;
                    text++;
                    rem--;
                } else if (rem > 0 && *text == '+') {
                    text++;
                    rem--;
                }

                while (rem > 0 && *text >= '0' && *text <= '9') {
                    value = value * 10.0 + (double)(*text - '0');
                    text++;
                    digits++;
                    rem--;
                }

                if (rem > 0 && *text == '.') {
                    text++;
                    rem--;
                    double frac = 0.1;
                    while (rem > 0 && *text >= '0' && *text <= '9') {
                        value += (double)(*text - '0') * frac;
                        frac *= 0.1;
                        text++;
                        digits++;
                        rem--;
                    }
                }

                if (digits == 0) {
                    break;
                }

                if (output != NULL) {
                    *output = (float)(value * sign);
                    assigned++;
                }
            } else if (*pattern == 's') {
                char *output = va_arg(args, char *);
                size_t copied = 0u;
                size_t limit = max_width ? max_width : (size_t)-1;
                bool more = has_more_conversions(pattern + 1);

                if (more) {
                    while (copied < limit && *text != '\0' &&
                           *text != ' ' && *text != '\t' &&
                           *text != '\n' && *text != '\r') {
                        if (output != NULL) {
                            output[copied] = *text;
                        }
                        copied++;
                        text++;
                    }
                } else {
                    while (copied < limit && *text != '\0' &&
                           *text != '\n' && *text != '\r') {
                        if (output != NULL) {
                            output[copied] = *text;
                        }
                        copied++;
                        text++;
                    }
                }

                if (copied == 0u) {
                    break;
                }

                if (output != NULL) {
                    output[copied] = '\0';
                    assigned++;
                }
            } else if (*pattern == 'c') {
                char *output = va_arg(args, char *);
                size_t count = max_width ? max_width : 1u;
                size_t copied = 0u;

                while (copied < count && *text != '\0') {
                    if (output != NULL) {
                        output[copied] = *text;
                    }
                    text++;
                    copied++;
                }
                if (copied < count) {
                    break;
                }
                if (output != NULL) {
                    assigned++;
                }
            } else if (*pattern == '[') {
                pattern++;
                bool invert = false;
                if (*pattern == '^') {
                    invert = true;
                    pattern++;
                }
                uint8_t set_chars[256];
                memset(set_chars, 0, sizeof(set_chars));
                if (*pattern == ']') {
                    set_chars[(unsigned char)']'] = 1;
                    pattern++;
                }
                while (*pattern != ']' && *pattern != '\0') {
                    set_chars[(unsigned char)*pattern] = 1;
                    pattern++;
                }
                if (*pattern != ']') {
                    break;
                }
                char *output = va_arg(args, char *);
                size_t copied = 0u;
                size_t limit = max_width ? max_width : (size_t)-1;
                while (copied < limit && *text != '\0' && *text != '\n' && *text != '\r') {
                    bool in_set = (set_chars[(unsigned char)*text] != 0);
                    if (invert ? in_set : !in_set) {
                        break;
                    }
                    if (output != NULL) {
                        output[copied] = *text;
                    }
                    copied++;
                    text++;
                }
                if (copied == 0u) {
                    break;
                }
                if (output != NULL) {
                    output[copied] = '\0';
                    assigned++;
                }
            } else {
                break;
            }
        } else if (*pattern == ' ' ||
                   *pattern == '\t' ||
                   *pattern == '\n') {
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

    serial_printf("[scanf] completed, assigned=%d\n", assigned);
    return assigned;
}