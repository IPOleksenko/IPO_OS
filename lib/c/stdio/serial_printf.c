#include <stdio.h>
#include <stdint.h>

/**
 * Formatted print to serial port
 */
int serial_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);

    int count = 0;

    while (*format) {
        if (*format == '%' && *(format + 1)) {
            format++;

            /* length modifier */
            int is_long = 0;
            if (*format == 'l' && *(format + 1) == 'l') {
                is_long = 1;
                format += 2;
            } else if (*format == 'l') {
                is_long = 1;
                format++;
            }

            if (!*format) break;

            switch (*format) {
                case 'd': {
                    int val = va_arg(args, int);
                    char buf[32];
                    int len;

                    if (val < 0) {
                        serial_putc('-');
                        count++;
                        unsigned int abs_val = (unsigned int)(-(long)val);
                        len = itoa(abs_val, buf, 10);
                    } else {
                        len = itoa((unsigned int)val, buf, 10);
                    }

                    for (int i = 0; i < len && i < 32; i++) {
                        serial_putc(buf[i]);
                        count++;
                    }
                    break;
                }

                case 'u': {
                    if (is_long) {
                        uint64_t val = va_arg(args, uint64_t);
                        char buf[64];
                        int len = itoa64(val, buf, 10);
                        for (int i = 0; i < len; i++) {
                            serial_putc(buf[i]);
                            count++;
                        }
                    } else {
                        unsigned int val = va_arg(args, unsigned int);
                        char buf[32];
                        int len = itoa(val, buf, 10);
                        for (int i = 0; i < len; i++) {
                            serial_putc(buf[i]);
                            count++;
                        }
                    }
                    break;
                }

                case 'x': {
                    unsigned int val = va_arg(args, unsigned int);
                    char buf[32];
                    int len = itoa(val, buf, 16);
                    for (int i = 0; i < len; i++) {
                        serial_putc(buf[i]);
                        count++;
                    }
                    break;
                }

                case 'c': {
                    char val = (char)va_arg(args, int);
                    serial_putc(val);
                    count++;
                    break;
                }

                case 's': {
                    const char *str = va_arg(args, const char *);
                    if (str) {
                        while (*str) {
                            serial_putc(*str++);
                            count++;
                        }
                    }
                    break;
                }

                case '%': {
                    serial_putc('%');
                    count++;
                    break;
                }

                default:
                    serial_putc('%');
                    serial_putc(*format);
                    count += 2;
                    break;
            }
        } else {
            serial_putc(*format);
            count++;
        }

        format++;
    }

    va_end(args);
    return count;
}
