#include <stdio.h>
#include <stdint.h>

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

            /* Check for 'l' length modifier */
            int is_long = 0;
            int is_long_long = 0;
            if (*format == 'l' && *(format + 1) == 'l') {
                is_long_long = 1;
                format += 2;
            } else if (*format == 'l') {
                is_long = 1;
                format++;
            }

            if (!*format) {
                break;
            }

            switch (*format) {
                case 'i':
                case 'd': {
                    /* Signed integer */
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
                        for (int p = 0; p < pad; p++) { putchar(' '); count++; }
                    }
                    if (is_neg) {
                        putchar('-');
                        count++;
                    }
                    if (!left_align && zero_pad) {
                        for (int p = 0; p < pad; p++) { putchar('0'); count++; }
                    }
                    for (int i = 0; i < len; i++) {
                        putchar(buf[i]);
                        count++;
                    }
                    if (left_align) {
                        for (int p = 0; p < pad; p++) { putchar(' '); count++; }
                    }
                    break;
                }

                case 'u': {
                    char buf[64];
                    int len = 0;
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
                        for (int p = 0; p < pad; p++) { putchar(pad_char); count++; }
                    }
                    for (int i = 0; i < len; i++) {
                        putchar(buf[i]);
                        count++;
                    }
                    if (left_align) {
                        for (int p = 0; p < pad; p++) { putchar(' '); count++; }
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

                case 'X':
                case 'x': {
                    /* Hexadecimal */
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
                        for (int p = 0; p < pad; p++) { putchar(pad_char); count++; }
                    }
                    for (int i = 0; i < len; i++) {
                        putchar(buf[i]);
                        count++;
                    }
                    if (left_align) {
                        for (int p = 0; p < pad; p++) { putchar(' '); count++; }
                    }
                    break;
                }

                case 'p': {
                    /* Pointer */
                    uint32_t val = (uint32_t)va_arg(args, void *);
                    char buf[32];
                    buf[0] = '0';
                    buf[1] = 'x';
                    int hex_len = itoa(val, buf + 2, 16);
                    int len = 2 + hex_len;
                    int pad = (width > len) ? (width - len) : 0;
                    if (!left_align) {
                        for (int p = 0; p < pad; p++) { putchar(' '); count++; }
                    }
                    for (int i = 0; i < len; i++) {
                        putchar(buf[i]);
                        count++;
                    }
                    if (left_align) {
                        for (int p = 0; p < pad; p++) { putchar(' '); count++; }
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
                    const char *str = va_arg(args, const char *);
                    if (!str) str = "(null)";
                    int len = 0;
                    while (str[len] && (precision < 0 || len < precision)) len++;

                    int pad = (width > len) ? (width - len) : 0;
                    if (!left_align) {
                        for (int p = 0; p < pad; p++) { putchar(' '); count++; }
                    }
                    for (int i = 0; i < len; i++) {
                        putchar(str[i]);
                        count++;
                    }
                    if (left_align) {
                        for (int p = 0; p < pad; p++) { putchar(' '); count++; }
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