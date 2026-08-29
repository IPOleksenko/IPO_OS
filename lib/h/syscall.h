#ifndef IPO_SYSCALL_H
#define IPO_SYSCALL_H

#include <stdint.h>

#define IPO_SYSCALL_ENOSYS      0xFFFFFFFFu
#define IPO_SYSCALL_OK          0x00000000u

#define IPO_SYSCALL_REGISTER    0x0001u
#define IPO_SYSCALL_CALL        0x0002u
#define IPO_SYSCALL_PRINT       0x1001u
#define IPO_SYSCALL_FS_CREATE   0x1010u
#define IPO_SYSCALL_FS_OPEN     0x1011u
#define IPO_SYSCALL_FS_READ     0x1012u
#define IPO_SYSCALL_FS_WRITE    0x1013u
#define IPO_SYSCALL_FS_DELETE   0x1014u
#define IPO_SYSCALL_FS_STAT     0x1015u
#define IPO_SYSCALL_FS_LIST     0x1016u
#define IPO_SYSCALL_FS_RENAME   0x1017u
#define IPO_SYSCALL_EXEC        0x1020u
#define IPO_SYSCALL_ASYNC_START  0x1030u
#define IPO_SYSCALL_ASYNC_STOP   0x1031u
#define IPO_SYSCALL_EXIT        0xFFFFu

typedef uint32_t (*ipo_syscall_handler_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

void syscall_init(void);
void ipo_register_syscall(uint32_t num, ipo_syscall_handler_t handler);
uint32_t syscall_dispatch(uint32_t num, uint32_t arg1, uint32_t arg2,
                         uint32_t arg3, uint32_t arg4, uint32_t arg5);

int ipo_syscall(uint32_t num, uint32_t arg1, uint32_t arg2, uint32_t arg3,
                uint32_t arg4, uint32_t arg5);

#endif
