#include <stdio.h>
#include <stdint.h>

/**
 * Formatted print to buffer
 */
__attribute__((weak))
int vsnprintf(char *buf, size_t size, const char *format, va_list args) {
    if (size == 0) return 0;
    if (!buf) return 0;

    size_t written = 0;
    size_t max_write = size - 1;  // Reserve space for null terminator

    while (*format && written < max_write) {
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

            /* Check for 'l' length modifier */
            int is_long = 0;
            if (*format == 'l' && *(format + 1) == 'l') {
                is_long = 1;
                format += 2;
            } else if (*format == 'l') {
                is_long = 1;
                format++;
            }

            if (!*format) break;

            char temp_buf[64];
            const char *str_val = NULL;
            int str_len = 0;

            switch (*format) {
                case 'd':
                case 'i': {
                    long long value = is_long ? va_arg(args, long long) : (long long)va_arg(args, int);
                    int pos = 0;
                    if (value < 0) {
                        temp_buf[pos++] = '-';
                        value = -value;
                    }
                    if (is_long) {
                        pos += itoa64((uint64_t)value, temp_buf + pos, 10);
                    } else {
                        pos += itoa((unsigned int)value, temp_buf + pos, 10);
                    }
                    temp_buf[pos] = '\0';
                    str_val = temp_buf;
                    str_len = pos;
                    break;
                }

                case 'u': {
                    if (is_long) {
                        uint64_t val = va_arg(args, uint64_t);
                        str_len = itoa64(val, temp_buf, 10);
                    } else {
                        unsigned int val = va_arg(args, unsigned int);
                        str_len = itoa(val, temp_buf, 10);
                    }
                    temp_buf[str_len] = '\0';
                    str_val = temp_buf;
                    break;
                }

                case 's': {
                    const char *s = va_arg(args, const char *);
                    if (!s) s = "(null)";
                    str_val = s;
                    str_len = 0;
                    while (s[str_len]) str_len++;
                    break;
                }

                case 'x':
                case 'X': {
                    unsigned long value = is_long ? va_arg(args, unsigned long) : (unsigned long)va_arg(args, unsigned int);
                    str_len = itoa(value, temp_buf, 16);
                    temp_buf[str_len] = '\0';
                    str_val = temp_buf;
                    break;
                }

                case 'c': {
                    int c = va_arg(args, int);
                    temp_buf[0] = (char)c;
                    temp_buf[1] = '\0';
                    str_val = temp_buf;
                    str_len = 1;
                    break;
                }

                case '%': {
                    temp_buf[0] = '%';
                    temp_buf[1] = '\0';
                    str_val = temp_buf;
                    str_len = 1;
                    break;
                }

                default:
                    temp_buf[0] = '%';
                    temp_buf[1] = *format;
                    temp_buf[2] = '\0';
                    str_val = temp_buf;
                    str_len = 2;
                    break;
            }

            if (str_val) {
                int pad_len = (width > str_len) ? (width - str_len) : 0;
                char pad_char = zero_pad ? '0' : ' ';

                /* Right align padding */
                if (!left_align) {
                    while (pad_len > 0 && written < max_write) {
                        buf[written++] = pad_char;
                        pad_len--;
                    }
                }

                /* Output string */
                for (int i = 0; i < str_len && written < max_write; i++) {
                    buf[written++] = str_val[i];
                }

                /* Left align padding */
                if (left_align) {
                    while (pad_len > 0 && written < max_write) {
                        buf[written++] = ' ';
                        pad_len--;
                    }
                }
            }
        } else {
            if (written < max_write) {
                buf[written++] = *format;
            }
        }

        format++;
    }

    buf[written] = '\0';
    return (int)written;
}

/**
 * Formatted print to buffer
 */
int snprintf(char *buf, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vsnprintf(buf, size, format, args);
    va_end(args);
    return ret;
}
