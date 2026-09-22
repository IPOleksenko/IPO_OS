#include <stdio.h>
#include <stdint.h>

/**
 * Convert unsigned 64-bit integer to string in-place without static buffers
 */
int itoa64(uint64_t num, char *str, int base) {
    if (!str || base < 2 || base > 36) return 0;
    
    if (num == 0) {
        str[0] = '0';
        str[1] = '\0';
        return 1;
    }
    
    // First pass: compute exact number of characters needed
    uint64_t temp = num;
    int len = 0;
    while (temp > 0) {
        temp /= (uint64_t)base;
        len++;
    }
    
    str[len] = '\0';
    for (int j = len - 1; j >= 0; j--) {
        uint64_t digit = num % (uint64_t)base;
        num /= (uint64_t)base;
        str[j] = (digit < 10) ? ('0' + (char)digit) : ('a' + (char)(digit - 10));
    }
    return len;
}

/**
 * Convert unsigned integer to string in-place without static buffers
 */
int itoa(unsigned int num, char *str, int base) {
    if (!str || base < 2 || base > 36) return 0;
    
    if (num == 0) {
        str[0] = '0';
        str[1] = '\0';
        return 1;
    }
    
    // First pass: compute exact number of characters needed
    unsigned int temp = num;
    int len = 0;
    while (temp > 0) {
        temp /= (unsigned int)base;
        len++;
    }
    
    str[len] = '\0';
    for (int j = len - 1; j >= 0; j--) {
        unsigned int digit = num % (unsigned int)base;
        num /= (unsigned int)base;
        str[j] = (digit < 10) ? ('0' + (char)digit) : ('a' + (char)(digit - 10));
    }
    return len;
}