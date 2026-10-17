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
    void *cur = ipo_sbrk(0);
    if (cur != (void *)-1 && (uintptr_t)cur > 0x08000000u) {
        int32_t dec = (int32_t)(0x08000000u - (uintptr_t)cur);
        ipo_sbrk(dec);
    }
}
