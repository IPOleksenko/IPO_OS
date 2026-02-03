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
            
            switch (*format) {
                case 'd': {
                    /* Signed integer */
                    int val = va_arg(args, int);
                    if (val < 0) {
                        putchar('-');
                        count++;
                        val = -val;
                    }
                    char buf[32];
                    int len = itoa((unsigned int)val, buf, 10);
                    for (int i = 0; i < len; i++) {
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
                        unsigned int val = va_arg(args, unsigned int);
                        char buf[32];
                        int len = itoa(val, buf, 10);
                        for (int i = 0; i < len; i++) {
                            putchar(buf[i]);
                            count++;
                        }
                    }
                    break;
                }
                
                case 'x': {
                    /* Hexadecimal */
                    unsigned int val = va_arg(args, unsigned int);
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
                    const char *str = va_arg(args, const char*);
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