#include <stdint.h>
#include <stdio.h>
#include <syscall.h>

static uint32_t my_driver_handler(uint32_t num, uint32_t arg1, uint32_t arg2,
                                 uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    printf("custom driver: syscall=0x%x a1=%u a2=%u a3=%u\n",
           num, arg1, arg2, arg3);
    return 1337u;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    uint32_t custom_id = 0x1234u;

    int reg = ipo_syscall(IPO_SYSCALL_REGISTER,
                          custom_id,
                          (uint32_t)(uintptr_t)my_driver_handler,
                          0,
                          0,
                          0);

    if (reg != 0) {
        printf("register failed: %d\n", reg);
        return 1;
    }

    int result = ipo_syscall(custom_id,
                             77,
                             88,
                             99,
                             0,
                             0);

    printf("driver result: %d\n", result);
    return 0;
}
