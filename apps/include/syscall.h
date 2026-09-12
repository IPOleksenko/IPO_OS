#ifndef _IPO_SYSCALL_H
#define _IPO_SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define IPO_SYSCALL_ENOSYS       0xFFFFFFFFu
#define IPO_SYSCALL_OK           0x00000000u
#define IPO_SYSCALL_REGISTER     0x0001u
#define IPO_SYSCALL_CALL         0x0002u

#define IPO_SYSCALL_PRINT        0x1001u
#define IPO_SYSCALL_WRITE        0x1002u

#define IPO_SYSCALL_FS_CREATE    0x1010u
#define IPO_SYSCALL_FS_OPEN      0x1011u
#define IPO_SYSCALL_FS_READ      0x1012u
#define IPO_SYSCALL_FS_WRITE     0x1013u
#define IPO_SYSCALL_FS_DELETE    0x1014u
#define IPO_SYSCALL_FS_STAT      0x1015u
#define IPO_SYSCALL_FS_LIST      0x1016u
#define IPO_SYSCALL_FS_RENAME    0x1017u
#define IPO_SYSCALL_FS_CLOSE     0x1018u
#define IPO_SYSCALL_FS_SEEK      0x1019u

#define IPO_SYSCALL_EXEC         0x1020u
#define IPO_SYSCALL_READ         0x1021u
#define IPO_SYSCALL_TERMINAL_INPUT 0x1022u
#define IPO_SYSCALL_PROCESS_YIELD 0x1023u
#define IPO_SYSCALL_PROCESS_IS_FOREGROUND 0x1024u
#define IPO_SYSCALL_GETCWD       0x1025u
#define IPO_SYSCALL_CHDIR        0x1026u
#define IPO_SYSCALL_GET_EXIT_CODE 0x1027u

#define IPO_SYSCALL_ASYNC_START  0x1030u
#define IPO_SYSCALL_ASYNC_STOP   0x1031u
#define IPO_SYSCALL_STACK_GROW   0x1040u
#define IPO_SYSCALL_STACK_SHRINK 0x1041u
#define IPO_SYSCALL_VAR_SET      0x1042u
#define IPO_SYSCALL_VAR_GET      0x1043u
#define IPO_SYSCALL_VAR_DELETE   0x1044u
#define IPO_SYSCALL_SBRK         0x1045u
#define IPO_SYSCALL_TIME         0x1046u
#define IPO_SYSCALL_FREE         0x1047u

#define IPO_SYSCALL_EXIT         0xFFFFu

#define IPO_INODE_TYPE_DIR  0x1
#define IPO_INODE_TYPE_FILE 0x2

struct ipo_inode {
    uint32_t mode;
    uint32_t links_count;
    uint64_t size;
    uint8_t  _data[112];
};

#ifdef __cplusplus
extern "C" {
#endif

int ipo_syscall(uint32_t num, uint32_t argc, uint32_t *argv);

static inline int ipo_print(const char *text) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)text;
    return ipo_syscall(IPO_SYSCALL_WRITE, 1u, args);
}

static inline int ipo_exec(const char *path, int argc, char **argv) {
    uint32_t args[3];
    args[0] = (uint32_t)(uintptr_t)path;
    args[1] = (uint32_t)argc;
    args[2] = (uint32_t)(uintptr_t)argv;
    return ipo_syscall(IPO_SYSCALL_EXEC, 3u, args);
}

static inline int ipo_open(const char *path) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)path;
    return ipo_syscall(IPO_SYSCALL_FS_OPEN, 1u, args);
}

static inline int ipo_close(int fd) {
    uint32_t args[1];
    args[0] = (uint32_t)fd;
    return ipo_syscall(IPO_SYSCALL_FS_CLOSE, 1u, args);
}

static inline int ipo_read(int fd, void *buf, uint32_t count, uint32_t offset) {
    uint32_t args[4];
    args[0] = (uint32_t)fd;
    args[1] = (uint32_t)(uintptr_t)buf;
    args[2] = count;
    args[3] = offset;
    return ipo_syscall(IPO_SYSCALL_FS_READ, 4u, args);
}

static inline int ipo_create(const char *path, uint8_t type) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)path;
    args[1] = (uint32_t)type;
    return ipo_syscall(IPO_SYSCALL_FS_CREATE, 2u, args);
}

static inline int ipo_write(int fd, const void *buf, uint32_t count, uint32_t offset) {
    uint32_t args[4];
    args[0] = (uint32_t)fd;
    args[1] = (uint32_t)(uintptr_t)buf;
    args[2] = count;
    args[3] = offset;
    return ipo_syscall(IPO_SYSCALL_FS_WRITE, 4u, args);
}

static inline int ipo_delete(const char *path) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)path;
    return ipo_syscall(IPO_SYSCALL_FS_DELETE, 1u, args);
}

static inline int ipo_seek(int fd, int32_t offset, int whence) {
    uint32_t args[3];
    args[0] = (uint32_t)fd;
    args[1] = (uint32_t)offset;
    args[2] = (uint32_t)whence;
    return ipo_syscall(IPO_SYSCALL_FS_SEEK, 3u, args);
}

static inline void *ipo_sbrk(int32_t increment) {
    uint32_t args[1];
    args[0] = (uint32_t)increment;
    int res = ipo_syscall(IPO_SYSCALL_SBRK, 1u, args);
    return (void *)(intptr_t)res;
}

static inline uint32_t ipo_time(uint32_t *tloc) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)tloc;
    return (uint32_t)ipo_syscall(IPO_SYSCALL_TIME, 1u, args);
}

static inline void ipo_exit(int status) {
    uint32_t args[1];
    args[0] = (uint32_t)status;
    ipo_syscall(IPO_SYSCALL_EXIT, 1u, args);
    while (1) { }
}

static inline int ipo_getcwd(char *buf, uint32_t size) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)buf;
    args[1] = size;
    return ipo_syscall(IPO_SYSCALL_GETCWD, 2u, args);
}

static inline int ipo_chdir(const char *path) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)path;
    return ipo_syscall(IPO_SYSCALL_CHDIR, 1u, args);
}

static inline int ipo_get_exit_code(void) {
    return ipo_syscall(IPO_SYSCALL_GET_EXIT_CODE, 0u, NULL);
}

static inline int ipo_stat(const char *path, struct ipo_inode *st) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)path;
    args[1] = (uint32_t)(uintptr_t)st;
    return ipo_syscall(IPO_SYSCALL_FS_STAT, 2u, args);
}

static inline int ipo_read_line(char *buf, uint32_t max_len) {
    if (buf == NULL || max_len == 0u) return -1;
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)buf;
    args[1] = max_len;
    return ipo_syscall(IPO_SYSCALL_READ, 2u, args);
}

static inline void ipo_kfree(void *ptr) {
    if (ptr == NULL) return;
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)ptr;
    ipo_syscall(IPO_SYSCALL_FREE, 1u, args);
}

static inline int ipo_read_line_dynamic(char **out_ptr) {
    if (out_ptr == NULL) return -1;
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)out_ptr;
    args[1] = 0u;
    return ipo_syscall(IPO_SYSCALL_READ, 2u, args);
}

#ifdef __cplusplus
}
#endif

#endif

