#include <syscall.h>

int ipo_syscall(uint32_t num, uint32_t argc, uint32_t *argv) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num),
          "b"(argc),
          "c"(argv)
        : "memory"
    );
    return ret;
}

void syscall_reset_user_heap(void) {
}
