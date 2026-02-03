#include <stdio.h>
#include <stdint.h>

/**
 * Formatted print to buffer
 */
int snprintf(char *buf, size_t size, const char *format, ...) {
    if (size == 0) return 0;
    if (!buf) return 0;
    
    va_list args;
    va_start(args, format);
    
    size_t written = 0;
    size_t max_write = size - 1;  // Reserve space for null terminator
    
    while (*format && written < max_write) {
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
                case 'd':
                case 'i': {
                    long long value = is_long ? va_arg(args, long long) : (long long)va_arg(args, int);
                    char temp_buf[32];
                    int n = itoa(value, temp_buf, 10);
                    if (n > 0) {
                        int to_copy = (n > (int)(max_write - written)) ? (max_write - written) : n;
                        for (int i = 0; i < to_copy; i++) {
                            buf[written++] = temp_buf[i];
                        }
                    }
                    break;
                }
                
                case 's': {
                    const char *s = va_arg(args, const char *);
                    if (!s) s = "(null)";
                    while (*s && written < max_write) {
                        buf[written++] = *s++;
                    }
                    break;
                }
                
                case 'x': {
                    unsigned long value = is_long ? va_arg(args, unsigned long) : (unsigned long)va_arg(args, unsigned int);
                    char temp_buf[32];
                    int n = itoa(value, temp_buf, 16);
                    if (n > 0) {
                        int to_copy = (n > (int)(max_write - written)) ? (max_write - written) : n;
                        for (int i = 0; i < to_copy; i++) {
                            buf[written++] = temp_buf[i];
                        }
                    }
                    break;
                }
                
                case 'c': {
                    int c = va_arg(args, int);
                    if (written < max_write) {
                        buf[written++] = (char)c;
                    }
                    break;
                }
                
                case '%': {
                    if (written < max_write) {
                        buf[written++] = '%';
                    }
                    break;
                }
                
                default:
                    if (written < max_write) buf[written++] = '%';
                    if (written < max_write) buf[written++] = *format;
                    break;
            }
        } else {
            if (written < max_write) {
                buf[written++] = *format;
            }
        }
        
        format++;
    }
    
    buf[written] = '\0';
    
    va_end(args);
    return (int)written;
}
