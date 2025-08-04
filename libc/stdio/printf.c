#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>

// Declare conversion functions to support different types
void putint(const size_t i) {
    char res[32];
    itoa(i, res, 0);
    puts(res);
}

int printf(const char* format, ...) {
    va_list args;
    va_start(args, format);

    size_t count = 0;
    char buffer[96] = {0};  // Increased buffer size for larger numbers and strings

    while (*format) {
        if (*format == '%') {
            format++;
            if (*format == '%') {
                putchar('%');
                count++;
            } else {
                int width = 0;          // Output width (e.g., 08 for %08llx)
                int precision = -1;     // Precision for floating-point numbers (e.g., .6 for %.6f)
                char pad_char = ' ';    // Padding character (default is space)

                // Process format options
                if (*format == '0') {
                    pad_char = '0'; // If %0 is specified, use '0' for padding
                    format++;
                }
                
                // Read field width
                while (*format >= '0' && *format <= '9') {
                    width = width * 10 + (*format - '0');
                    format++;
                }

                // Read precision
                if (*format == '.') {
                    format++;
                    precision = 0;
                    while (*format >= '0' && *format <= '9') {
                        precision = precision * 10 + (*format - '0');
                        format++;
                    }
                }

                switch (*format) {
                    case 'd': // Print integer
                    {
                        int value = va_arg(args, int);
                        itoa(value, buffer, 10);
                        size_t len = strlen(buffer);
                        for (size_t i = 0; i < len; i++) {
                            putchar(buffer[i]);
                            count++;
                        }
                    }
                    break;
                    case 'u': // Print unsigned int
                    {
                        unsigned int value = va_arg(args, unsigned int);
                        utoa(value, buffer, 10);
                        size_t len = strlen(buffer);
                        for (size_t i = 0; i < len; i++) {
                            putchar(buffer[i]);
                            count++;
                        }
                    }
                    break;
                    case 's': // Print string
                    {
                        uint8_t* str = va_arg(args, uint8_t*);
                        
                        while(*str) {
                            uint16_t ch = (uint16_t)*str;

                            if(ch == 0xd0 || ch == 0xd1) {
                                ch |= (*++str) << 8;
                                count++;
                            }

                            putchar(ch);
                            str++;
                            count++;
                        }
                    }
                    break;
                    case 'c': // Print character
                    {
                        char c = va_arg(args, int);
                        putchar(c);
                        count++;
                    }
                    break;
                    case 'p': // Print pointer
                    {
                        void* ptr = va_arg(args, void*);
                        itoa((size_t)ptr, buffer, 16); // Convert pointer to string in hexadecimal format
                        size_t len = strlen(buffer);
                        for (size_t i = 0; i < len; i++) {
                            putchar(buffer[i]);
                            count++;
                        }
                    }                    
                    break;
                    case 'x': // Print integer in hexadecimal format (lowercase)
                    {
                        int value = va_arg(args, int);
                        itoa(value, buffer, 16);
                        size_t len = strlen(buffer);
                        for (size_t i = 0; i < len; i++) {
                            putchar(buffer[i]);
                            count++;
                        }
                    }
                    break;
                    case 'X': // Print integer in hexadecimal format (uppercase)
                    {
                        int value = va_arg(args, int);
                        itoa(value, buffer, 16);
                        for (size_t i = 0; i < strlen(buffer); i++) {
                            putchar(toupper(buffer[i])); // Convert to uppercase
                            count++;
                        }
                    }
                    break;
                    case 'o': // Print integer in octal format
                    {
                        unsigned int value = va_arg(args, unsigned int);
                        utoa(value, buffer, 8); // Convert number to string in octal format
                        size_t len = strlen(buffer);
                        for (size_t i = 0; i < len; i++) {
                            putchar(buffer[i]);
                            count++;
                        }
                    }
                    break;
                    case 'b': // Print integer in binary format
                    {
                        unsigned int value = va_arg(args, unsigned int);
                        utoa(value, buffer, 2); // Convert number to string in binary format
                        size_t len = strlen(buffer);
                        for (size_t i = 0; i < len; i++) {
                            putchar(buffer[i]);
                            count++;
                        }
                    }
                    break;
                    case 'n': // Store the number of printed characters
                    {
                        int* pcount = va_arg(args, int*);
                        *pcount = count;
                    }
                    break;
                    case 'z': // Print size_t
                    {
                        format++;
                        if (*format == 'u') {
                            size_t value = va_arg(args, size_t);
                            itoa(value, buffer, 10);
                            size_t len = strlen(buffer);
                            for (size_t i = 0; i < len; i++) {
                                putchar(buffer[i]);
                                count++;
                            }
                        } else {
                            putchar('%');
                            putchar('z');
                            putchar(*format);
                            count += 3;
                        }
                    }
                    break;
                    case 'f': // Print floating-point number
                    {
                        double value = va_arg(args, double);
                        int prec = (precision == -1) ? 6 : precision; // Use default precision of 6 if not specified
                        ftoa(value, buffer, prec);  // Use specified precision
                        size_t len = strlen(buffer);
                        for (size_t i = 0; i < len; i++) {
                            putchar(buffer[i]);
                            count++;
                        }
                    }
                    break;
                    case 'e': // Print number in exponential format
                    {
                        double value = va_arg(args, double);
                        int prec = (precision == -1) ? 6 : precision; // Use default precision of 6 if not specified
                        etoa(value, buffer, prec);  // Use specified precision
                        size_t len = strlen(buffer);
                        for (size_t i = 0; i < len; i++) {
                            putchar(buffer[i]);
                            count++;
                        }
                    }
                    break;
                    case 'g': // Print number in shortest format
                    {
                        double value = va_arg(args, double);
                        int prec = (precision == -1) ? 6 : precision; // Use default precision of 6 if not specified
                        gtoa(value, buffer, prec);  // Use specified precision
                        size_t len = strlen(buffer);
                        for (size_t i = 0; i < len; i++) {
                            putchar(buffer[i]);
                            count++;
                        }
                    }
                    break;
                    case 'l': // Print long and long long
                    {
                        format++;
                        if (*format == 'd') { // long
                            long value = va_arg(args, long);
                            ltoa(value, buffer, 10);
                            size_t len = strlen(buffer);
                            for (size_t i = 0; i < len; i++) {
                                putchar(buffer[i]);
                                count++;
                            }
                        } else if (*format == 'l') { // long long
                            format++;
                            if (*format == 'd') {
                                long long value = va_arg(args, long long);
                                lltoa(value, buffer, 10);
                                size_t len = strlen(buffer);
                                for (size_t i = 0; i < len; i++) {
                                    putchar(buffer[i]);
                                    count++;
                                }
                            } else if(*format == 'u') { // %llu - unsigned long long
                                unsigned long long value = va_arg(args, unsigned long long);
                                llutoa(value, buffer, 10);
                                size_t len = strlen(buffer);
                                for (size_t i = 0; i < len; i++) {
                                    putchar(buffer[i]);
                                    count++;
                                }
                            } else if (*format == 'x') { // %llx - long long in hexadecimal format
                                unsigned long long value = va_arg(args, unsigned long long);
                                llutoa(value, buffer, 16);
                                size_t len = strlen(buffer);
                                
                                // Add leading zeros for %08llx format
								if ((size_t)width > len) { // Cast width to size_t
									for (size_t i = 0; i < (size_t)width - len; i++) {
										putchar(pad_char);
										count++;
									}
								}


                                for (size_t i = 0; i < len; i++) {
                                    putchar(buffer[i]);
                                    count++;
                                }
                            } else {
                                putchar('%');
                                putchar('l');
                                putchar('l');
                                putchar(*format);
                                count += 4;
                            }
                        } else if (*format == 'f') { // long double
                            long double value = va_arg(args, long double);
                            int prec = (precision == -1) ? 6 : precision; // Use default precision of 6 if not specified
                            lftoa(value, buffer, prec);  // Use specified precision
                            size_t len = strlen(buffer);
                            for (size_t i = 0; i < len; i++) {
                                putchar(buffer[i]);
                                count++;
                            }
                        } else if(*format == 'u') { // %lu - unsigned long
                            unsigned long value = va_arg(args, unsigned long);
                            lutoa(value, buffer, 10);
                            size_t len = strlen(buffer);
                            for (size_t i = 0; i < len; i++) {
                                putchar(buffer[i]);
                                count++;
                            }
                        } else if (*format == 'x') { // %lx - unsigned long in hexadecimal format
                            unsigned long value = va_arg(args, unsigned long);
                            lutoa(value, buffer, 16);
                            size_t len = strlen(buffer);
                            for (size_t i = 0; i < len; i++) {
                                putchar(buffer[i]);
                                count++;
                            }
                        } else {
                            putchar('%');
                            putchar('l');
                            putchar(*format);
                            count += 3;
                        }
                    }
                    break;
                    case 'L': // Print long double
                    {
                        format++;
                        if (*format == 'f') {
                            long double value = va_arg(args, long double);
                            int prec = (precision == -1) ? 6 : precision; // Use default precision of 6 if not specified
                            lftoa(value, buffer, prec);  // Use specified precision
                            size_t len = strlen(buffer);
                            for (size_t i = 0; i < len; i++) {
                                putchar(buffer[i]);
                                count++;
                            }
                        } else {
                            putchar('%');
                            putchar('L');
                            putchar(*format);
                            count += 3;
                        }
                    }
                    break;
                    default:
                        putchar('%');
                        putchar(*format);
                        count += 2;
                        break;
                }
            }
        } else {
            uint16_t ch = (uint16_t)(*(uint8_t*)format);

            if(ch == 0xd0 || ch == 0xd1) {
                ch |= (*(uint8_t*)(++format)) << 8;
                count++;
            }

            putchar(ch);
            count++;
        }
        format++;
    }

    va_end(args);
    return count;
}
