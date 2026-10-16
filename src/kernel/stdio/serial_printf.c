#include <stdio.h>
#include <stdint.h>
#include <kernel/smp.h>

static spinlock_t serial_printf_lock = {0};

/**
 * Formatted print to serial port
 */
int serial_printf(const char *format, ...) {
    spin_lock(&serial_printf_lock);

    va_list args;
    va_start(args, format);

    int count = 0;


    while (*format) {
        if (*format == '%' && *(format + 1)) {
            format++;

            /* Flags */
            int left_align = 0;
            int zero_pad = 0;
            while (*format == '-' || *format == '0' || *format == ' ' || *format == '+') {
                if (*format == '-') left_align = 1;
                else if (*format == '0') zero_pad = 1;
                format++;
            }
            if (left_align) zero_pad = 0;

            /* Width */
            int width = 0;
            while (*format >= '0' && *format <= '9') {
                width = width * 10 + (*format - '0');
                format++;
            }

            /* Precision */
            int precision = -1;
            if (*format == '.') {
                format++;
                precision = 0;
                while (*format >= '0' && *format <= '9') {
                    precision = precision * 10 + (*format - '0');
                    format++;
                }
            }

            /* length modifier */
            int is_long = 0;
            int is_long_long = 0;
            if (*format == 'l' && *(format + 1) == 'l') {
                is_long_long = 1;
                format += 2;
            } else if (*format == 'l') {
                is_long = 1;
                format++;
            }

            if (!*format) break;

            switch (*format) {
                case 'i':
                case 'd': {
                    char buf[64];
                    int len = 0;
                    int is_neg = 0;

                    if (is_long_long) {
                        long long val = va_arg(args, long long);
                        if (val < 0) {
                            is_neg = 1;
                            val = -val;
                        }
                        len = itoa64((uint64_t)val, buf, 10);
                    } else if (is_long) {
                        long val = va_arg(args, long);
                        if (val < 0) {
                            is_neg = 1;
                            unsigned int abs_val = (unsigned int)(-(long)val);
                            len = itoa(abs_val, buf, 10);
                        } else {
                            len = itoa((unsigned int)val, buf, 10);
                        }
                    } else {
                        int val = va_arg(args, int);
                        if (val < 0) {
                            is_neg = 1;
                            unsigned int abs_val = (unsigned int)(-(long)val);
                            len = itoa(abs_val, buf, 10);
                        } else {
                            len = itoa((unsigned int)val, buf, 10);
                        }
                    }

                    int total_len = len + (is_neg ? 1 : 0);
                    int pad = (width > total_len) ? (width - total_len) : 0;

                    if (!left_align && !zero_pad) {
                        for (int p = 0; p < pad; p++) { serial_putc(' '); count++; }
                    }
                    if (is_neg) {
                        serial_putc('-');
                        count++;
                    }
                    if (!left_align && zero_pad) {
                        for (int p = 0; p < pad; p++) { serial_putc('0'); count++; }
                    }
                    for (int i = 0; i < len; i++) {
                        serial_putc(buf[i]);
                        count++;
                    }
                    if (left_align) {
                        for (int p = 0; p < pad; p++) { serial_putc(' '); count++; }
                    }
                    break;
                }

                case 'u': {
                    char buf[64];
                    int len;
                    if (is_long_long) {
                        uint64_t val = va_arg(args, uint64_t);
                        len = itoa64(val, buf, 10);
                    } else if (is_long) {
                        unsigned long val = va_arg(args, unsigned long);
                        len = itoa((unsigned int)val, buf, 10);
                    } else {
                        unsigned int val = va_arg(args, unsigned int);
                        len = itoa(val, buf, 10);
                    }

                    int pad = (width > len) ? (width - len) : 0;
                    char pad_char = zero_pad ? '0' : ' ';

                    if (!left_align) {
                        for (int p = 0; p < pad; p++) { serial_putc(pad_char); count++; }
                    }
                    for (int i = 0; i < len; i++) {
                        serial_putc(buf[i]);
                        count++;
                    }
                    if (left_align) {
                        for (int p = 0; p < pad; p++) { serial_putc(' '); count++; }
                    }
                    break;
                }

                case 'f': {
                    double val = va_arg(args, double);
                    char int_buf[32];
                    char frac_buf[16];
                    int frac_len = 0;

                    if (val < 0.0) {
                        serial_putc('-');
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

                    int whole_len = itoa64(whole, int_buf, 10);
                    for (int i = 0; i < whole_len; i++) {
                        serial_putc(int_buf[i]);
                        count++;
                    }

                    serial_putc('.');
                    count++;

                    uint64_t divisor = 100000ULL;
                    for (int i = 0; i < 6; i++) {
                        uint64_t digit = (scaled / divisor) % 10ULL;
                        frac_buf[frac_len++] = (char)('0' + digit);
                        divisor /= 10ULL;
                    }

                    for (int i = 0; i < frac_len; i++) {
                        serial_putc(frac_buf[i]);
                        count++;
                    }
                    break;
                }

                case 'X':
                case 'x': {
                    char buf[64];
                    int len = 0;
                    if (is_long_long) {
                        uint64_t val = va_arg(args, uint64_t);
                        len = itoa64(val, buf, 16);
                    } else if (is_long) {
                        unsigned long val = va_arg(args, unsigned long);
                        len = itoa((unsigned int)val, buf, 16);
                    } else {
                        unsigned int val = va_arg(args, unsigned int);
                        len = itoa(val, buf, 16);
                    }
                    if (*format == 'X') {
                        for (int j = 0; j < len; j++) {
                            if (buf[j] >= 'a' && buf[j] <= 'f') buf[j] -= 32;
                        }
                    }
                    int pad = (width > len) ? (width - len) : 0;
                    char pad_char = zero_pad ? '0' : ' ';

                    if (!left_align) {
                        for (int p = 0; p < pad; p++) { serial_putc(pad_char); count++; }
                    }
                    for (int i = 0; i < len; i++) {
                        serial_putc(buf[i]);
                        count++;
                    }
                    if (left_align) {
                        for (int p = 0; p < pad; p++) { serial_putc(' '); count++; }
                    }
                    break;
                }

                case 'p': {
                    uint32_t val = (uint32_t)va_arg(args, void *);
                    char buf[32];
                    buf[0] = '0';
                    buf[1] = 'x';
                    int hex_len = itoa(val, buf + 2, 16);
                    int len = 2 + hex_len;
                    int pad = (width > len) ? (width - len) : 0;
                    if (!left_align) {
                        for (int p = 0; p < pad; p++) { serial_putc(' '); count++; }
                    }
                    for (int i = 0; i < len; i++) {
                        serial_putc(buf[i]);
                        count++;
                    }
                    if (left_align) {
                        for (int p = 0; p < pad; p++) { serial_putc(' '); count++; }
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
                    if (!str) str = "(null)";
                    int len = 0;
                    while (str[len] && (precision < 0 || len < precision)) len++;

                    int pad = (width > len) ? (width - len) : 0;
                    if (!left_align) {
                        for (int p = 0; p < pad; p++) { serial_putc(' '); count++; }
                    }
                    for (int i = 0; i < len; i++) {
                        serial_putc(str[i]);
                        count++;
                    }
                    if (left_align) {
                        for (int p = 0; p < pad; p++) { serial_putc(' '); count++; }
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
    spin_unlock(&serial_printf_lock);
    return count;
}

