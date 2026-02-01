#include <stdio.h>
#include <stdint.h>

/**
 * Convert unsigned 64-bit integer to string
 */
int itoa64(uint64_t num, char *str, int base) {
    int len = 0;
    uint64_t digits[64];
    int i = 0;
    
    if (num == 0) {
        str[0] = '0';
        return 1;
    }
    
    while (num > 0) {
        digits[i++] = num % base;
        num /= base;
    }
    
    for (int j = i - 1; j >= 0; j--) {
        uint64_t digit = digits[j];
        if (digit < 10) {
            str[len++] = '0' + digit;
        } else {
            str[len++] = 'a' + (digit - 10);
        }
    }
    
    return len;
}

/**
 * Convert unsigned integer to string
 */
int itoa(unsigned int num, char *str, int base) {
    int len = 0;
    unsigned int digits[32];
    int i = 0;
    
    if (num == 0) {
        str[0] = '0';
        return 1;
    }
    
    while (num > 0) {
        digits[i++] = num % base;
        num /= base;
    }
    
    for (int j = i - 1; j >= 0; j--) {
        unsigned int digit = digits[j];
        if (digit < 10) {
            str[len++] = '0' + digit;
        } else {
            str[len++] = 'a' + (digit - 10);
        }
    }
    
    return len;
}