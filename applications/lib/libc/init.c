#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <syscall.h>

typedef void (*func_ptr)(void);

extern func_ptr __preinit_array_start[] __attribute__((weak));
extern func_ptr __preinit_array_end[] __attribute__((weak));
extern func_ptr __init_array_start[] __attribute__((weak));
extern func_ptr __init_array_end[] __attribute__((weak));
extern func_ptr __fini_array_start[] __attribute__((weak));
extern func_ptr __fini_array_end[] __attribute__((weak));

extern void _init(void) __attribute__((weak));
extern void _fini(void) __attribute__((weak));
extern void __cxa_finalize(void *dso_handle) __attribute__((weak));

__attribute__((weak)) char **environ = NULL;
__attribute__((weak)) char *__progname = NULL;
__attribute__((weak)) const char *__ipo_return_format = NULL;
int fflush(void *stream);

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

    if (__cxa_finalize) {
        __cxa_finalize(NULL);
    }

    if (_fini) {
        _fini();
    }
}

static bool is_func_struct_return(void *func_ptr) {
    if (!func_ptr) return false;
    const uint8_t *code = (const uint8_t *)func_ptr;
    if (code[0] == 0xE9) {
        int32_t rel = *(const int32_t *)(code + 1);
        code = code + 5 + rel;
    }
    for (int i = 0; i < 512; i++) {
        if (code[i] == 0xC2 && code[i + 1] == 0x04 && code[i + 2] == 0x00) {
            return true;
        }
        if (code[i] == 0xC3) {
            if (i + 3 < 512 && code[i + 1] == 0x55 && code[i + 2] == 0x89 && code[i + 3] == 0xE5) {
                return false;
            }
        }
    }
    return false;
}

uint64_t __libc_start_main(void *main_func, int argc, char **argv, char **envp) {
    environ = envp;
    if (argv && argv[0]) {
        __progname = argv[0];
    }

    /* Run global constructors (.init / .init_array) */
    __libc_init_array();

    bool is_struct = is_func_struct_return(main_func);

    uint32_t ret_eax = 0;
    uint32_t ret_edx = 0;

    if (is_struct) {
        static uint8_t struct_buf[512];
        for (size_t i = 0; i < sizeof(struct_buf); i++) {
            struct_buf[i] = 0xDD;
        }

        typedef void* (*struct_main_fn)(void *ret_buf, int argc, char **argv, char **envp);
        struct_main_fn s_main = (struct_main_fn)main_func;
        s_main(struct_buf, argc, argv, envp);

        ret_eax = (uint32_t)(uintptr_t)struct_buf;
        ret_edx = 0;

        int last_written = -1;
        for (int i = (int)sizeof(struct_buf) - 1; i >= 0; i--) {
            if (struct_buf[i] != 0xDD) {
                last_written = i;
                break;
            }
        }
        uint32_t written_size = (last_written >= 0) ? (uint32_t)(last_written + 1) : 0;
        const char *fmt = __ipo_return_format ? __ipo_return_format : "struct";
        ipo_set_return_custom(fmt, struct_buf, written_size);
    } else {
        typedef int (*scalar_main_fn)(int, char**, char**);
        scalar_main_fn scalar_main = (scalar_main_fn)main_func;
        __asm__ volatile(
            "pushl %3\n\t"
            "pushl %4\n\t"
            "pushl %5\n\t"
            "call *%2\n\t"
            "addl $12, %%esp\n\t"
            : "=a"(ret_eax), "=d"(ret_edx)
            : "r"(scalar_main), "r"(envp), "r"(argv), "r"(argc)
            : "ecx", "memory", "cc"
        );
    }

    /* Run global destructors (.fini / .fini_array) */
    __libc_fini_array();

    /* Flush stdio */
    fflush(NULL);

    return ((uint64_t)ret_edx << 32) | (uint64_t)ret_eax;
}
