#include <stdio.h>
#include <stdint.h>

#ifdef IPO_APP
#include <syscall.h>

static void app_putchar(char c) {
    char text[2] = { c, '\0' };

    uint32_t syscall_args[] = {
        (uint32_t)(uintptr_t)text
    };

    ipo_syscall(
        IPO_SYSCALL_WRITE,
        1u,
        syscall_args
    );
}

#define putchar app_putchar
#endif

/**
 * Formatted print function
 */
int printf(const char *format, ...) {
    va_list args;
    va_start(args, format);

    int count = 0;

    while (*format) {
        if (*format == '%' && *(format + 1)) {
            format++;

            /* Check for 'l' length modifier */
            int is_long = 0;

            if (*format == 'l' && *(format + 1) == 'l') {
                is_long = 1;
                format += 2;
            } else if (*format == 'l') {
                is_long = 1;
                format++;
            }

            if (!*format) {
                break;
            }

            switch (*format) {
                case 'd': {
                    /* Signed integer */
                    int val = va_arg(args, int);
                    char buf[32];
                    int len = 0;

                    if (val < 0) {
                        putchar('-');
                        count++;

                        /* Convert to absolute value safely */
                        /* For INT_MIN, we use (unsigned int)(-(long)val) to avoid overflow */
                        unsigned int abs_val =
                            (unsigned int)(-(long)val);

                        len = itoa(abs_val, buf, 10);
                    } else {
                        len = itoa((unsigned int)val, buf, 10);
                    }

                    for (int i = 0; i < len && i < 32; i++) {
                        putchar(buf[i]);
                        count++;
                    }

                    break;
                }

                case 'u': {
                    if (is_long) {
                        /* Unsigned long long */
                        uint64_t val = va_arg(args, uint64_t);
                        char buf[64];
                        int len = itoa64(val, buf, 10);

                        for (int i = 0; i < len; i++) {
                            putchar(buf[i]);
                            count++;
                        }
                    } else {
                        /* Unsigned integer */
                        unsigned int val =
                            va_arg(args, unsigned int);

                        char buf[32];
                        int len = itoa(val, buf, 10);

                        for (int i = 0; i < len; i++) {
                            putchar(buf[i]);
                            count++;
                        }
                    }

                    break;
                }

                case 'f': {
                    double val = va_arg(args, double);
                    char int_buf[32];
                    char frac_buf[16];
                    int whole_len = 0;
                    int frac_len = 0;

                    if (val < 0.0) {
                        putchar('-');
                        count++;
                        val = -val;
                    }

                    unsigned long long whole = (unsigned long long)val;
                    double fraction = val - (double)whole;
                    uint64_t scaled = (uint64_t)((fraction * 1000000.0) + 0.5);
                    if (scaled >= 1000000ULL) {
                        whole++;
                        scaled = 0ULL;
                    }

                    whole_len = itoa64(whole, int_buf, 10);
                    for (int i = 0; i < whole_len; i++) {
                        putchar(int_buf[i]);
                        count++;
                    }

                    putchar('.');
                    count++;

                    uint64_t divisor = 100000ULL;
                    for (int i = 0; i < 6; i++) {
                        uint64_t digit = (scaled / divisor) % 10ULL;
                        frac_buf[frac_len++] = (char)('0' + digit);
                        divisor /= 10ULL;
                    }

                    for (int i = 0; i < frac_len; i++) {
                        putchar(frac_buf[i]);
                        count++;
                    }

                    break;
                }

                case 'x': {
                    /* Hexadecimal */
                    unsigned int val =
                        va_arg(args, unsigned int);

                    char buf[32];
                    int len = itoa(val, buf, 16);

                    for (int i = 0; i < len; i++) {
                        putchar(buf[i]);
                        count++;
                    }

                    break;
                }

                case 'c': {
                    /* Character */
                    char val = (char)va_arg(args, int);

                    putchar(val);
                    count++;

                    break;
                }

                case 's': {
                    /* String */
                    const char *str =
                        va_arg(args, const char *);

                    if (str) {
                        while (*str) {
                            putchar(*str++);
                            count++;
                        }
                    }

                    break;
                }

                case '%': {
                    /* Literal % */
                    putchar('%');
                    count++;

                    break;
                }

                default:
                    putchar('%');
                    putchar(*format);
                    count += 2;

                    break;
            }
        } else {
            putchar(*format);
            count++;
        }

        format++;
    }

    va_end(args);

    return count;
}