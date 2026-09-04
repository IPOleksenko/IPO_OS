#include <stdint.h>

uint64_t __udivmoddi4(uint64_t num, uint64_t den, uint64_t *rem_p) {
    if (den == 0) {
        if (rem_p) *rem_p = 0;
        return 0;
    }
    uint64_t quot = 0, rem = 0;
    for (int i = 63; i >= 0; i--) {
        rem = (rem << 1) | ((num >> i) & 1);
        if (rem >= den) {
            rem -= den;
            quot |= ((uint64_t)1 << i);
        }
    }
    if (rem_p) *rem_p = rem;
    return quot;
}

uint64_t __udivdi3(uint64_t a, uint64_t b) {
    return __udivmoddi4(a, b, (void *)0);
}

uint64_t __umoddi3(uint64_t a, uint64_t b) {
    uint64_t rem = 0;
    __udivmoddi4(a, b, &rem);
    return rem;
}

int64_t __divdi3(int64_t a, int64_t b) {
    int neg = 0;
    if (a < 0) { a = -a; neg = !neg; }
    if (b < 0) { b = -b; neg = !neg; }
    uint64_t res = __udivdi3((uint64_t)a, (uint64_t)b);
    return neg ? -(int64_t)res : (int64_t)res;
}

int64_t __moddi3(int64_t a, int64_t b) {
    int neg = 0;
    if (a < 0) { a = -a; neg = 1; }
    if (b < 0) { b = -b; }
    uint64_t rem = __umoddi3((uint64_t)a, (uint64_t)b);
    return neg ? -(int64_t)rem : (int64_t)rem;
}
