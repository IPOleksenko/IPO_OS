#include <stddef.h>

typedef void (*func_ptr)(void);

extern func_ptr __preinit_array_start[] __attribute__((weak));
extern func_ptr __preinit_array_end[] __attribute__((weak));
extern func_ptr __init_array_start[] __attribute__((weak));
extern func_ptr __init_array_end[] __attribute__((weak));
extern func_ptr __fini_array_start[] __attribute__((weak));
extern func_ptr __fini_array_end[] __attribute__((weak));

extern void _init(void) __attribute__((weak));
extern void _fini(void) __attribute__((weak));

void __libc_init_array(void) {
    size_t count;
    size_t i;

    if (__preinit_array_start && __preinit_array_end) {
        count = __preinit_array_end - __preinit_array_start;
        for (i = 0; i < count; i++) {
            if (__preinit_array_start[i]) {
                __preinit_array_start[i]();
            }
        }
    }

    if (_init) {
        _init();
    }

    if (__init_array_start && __init_array_end) {
        count = __init_array_end - __init_array_start;
        for (i = 0; i < count; i++) {
            if (__init_array_start[i]) {
                __init_array_start[i]();
            }
        }
    }
}

void __libc_fini_array(void) {
    size_t count;
    size_t i;

    if (__fini_array_start && __fini_array_end) {
        count = __fini_array_end - __fini_array_start;
        for (i = count; i > 0; i--) {
            if (__fini_array_start[i - 1]) {
                __fini_array_start[i - 1]();
            }
        }
    }

    if (_fini) {
        _fini();
    }
}

