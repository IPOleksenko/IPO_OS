#ifndef IPO_SYSCALL_H

#define IPO_SYSCALL_H

#include <stdint.h>
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

#define IPO_SYSCALL_EXEC         0x1020u

#define IPO_SYSCALL_READ         0x1021u

#define IPO_SYSCALL_TERMINAL_INPUT 0x1022u

#define IPO_SYSCALL_ASYNC_START  0x1030u

#define IPO_SYSCALL_ASYNC_STOP   0x1031u

#define IPO_SYSCALL_STACK_GROW   0x1040u

#define IPO_SYSCALL_STACK_SHRINK 0x1041u

#define IPO_SYSCALL_VAR_SET      0x1042u

#define IPO_SYSCALL_VAR_GET      0x1043u

#define IPO_SYSCALL_VAR_DELETE   0x1044u

#define IPO_SYSCALL_KEYMAP_SET      0x1050u
#define IPO_SYSCALL_KEYMAP_GET      0x1051u
#define IPO_SYSCALL_FONT_LOAD       0x1052u
#define IPO_SYSCALL_KEYMAP_DISABLE  0x1053u
#define IPO_SYSCALL_KEYMAP_ENABLE   0x1054u
#define IPO_SYSCALL_KEYMAP_REMOVE   0x1055u

#define IPO_SYSCALL_DRIVER_REGISTER   0x1060u
#define IPO_SYSCALL_DRIVER_UNREGISTER 0x1061u
#define IPO_SYSCALL_DRIVER_LIST       0x1062u

#define IPO_SYSCALL_EXIT         0xFFFFu

typedef uint32_t (*ipo_syscall_handler_t)(uint32_t, uint32_t, uint32_t *);

void syscall_init(void);

void ipo_register_syscall(uint32_t num, ipo_syscall_handler_t handler);

int ipo_syscall(uint32_t num, uint32_t argc, uint32_t *argv);

static inline int ipo_exec(const char *path, int argc, char **argv) {
    uint32_t args[3];
    args[0] = (uint32_t)(uintptr_t)path;
    args[1] = (uint32_t)argc;
    args[2] = (uint32_t)(uintptr_t)argv;
    return ipo_syscall(IPO_SYSCALL_EXEC, 3u, args);
}

static inline int ipo_terminal_input(const char *text, int auto_execute) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)text;
    args[1] = (uint32_t)auto_execute;
    return ipo_syscall(IPO_SYSCALL_TERMINAL_INPUT, 2u, args);
}

static inline int ipo_var_set(const char *name, const void *value, uint32_t value_size) {
    uint32_t args[3];
    args[0] = (uint32_t)(uintptr_t)name;
    args[1] = (uint32_t)(uintptr_t)value;
    args[2] = value_size;
    return ipo_syscall(IPO_SYSCALL_VAR_SET, 3u, args);
}

static inline int ipo_var_get(const char *name, void *buffer, uint32_t buffer_size) {
    uint32_t args[3];
    args[0] = (uint32_t)(uintptr_t)name;
    args[1] = (uint32_t)(uintptr_t)buffer;
    args[2] = buffer_size;
    return ipo_syscall(IPO_SYSCALL_VAR_GET, 3u, args);
}

static inline int ipo_var_delete(const char *name) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)name;
    return ipo_syscall(IPO_SYSCALL_VAR_DELETE, 1u, args);
}

static inline int ipo_var_set_int(const char *name, int value) {
    return ipo_var_set(name, &value, sizeof(value));
}

static inline int ipo_var_get_int(const char *name, int *out_value) {
    return ipo_var_get(name, out_value, sizeof(*out_value));
}

static inline int ipo_var_set_float(const char *name, float value) {
    return ipo_var_set(name, &value, sizeof(value));
}

static inline int ipo_var_get_float(const char *name, float *out_value) {
    return ipo_var_get(name, out_value, sizeof(*out_value));
}

static inline int ipo_var_set_bool(const char *name, int value) {
    return ipo_var_set(name, &value, sizeof(value));
}

static inline int ipo_var_get_bool(const char *name, int *out_value) {
    return ipo_var_get(name, out_value, sizeof(*out_value));
}

static inline int ipo_var_set_str(const char *name, const char *value) {
    if (value == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    return ipo_var_set(name, value, (uint32_t)strlen(value) + 1u);
}

static inline int ipo_var_get_str(const char *name, char *buffer, uint32_t buffer_size) {
    if (buffer == NULL || buffer_size == 0u) {
        return IPO_SYSCALL_ENOSYS;
    }
    return ipo_var_get(name, buffer, buffer_size);
}

#define IPO_VAR_SET(name, value) \
    __extension__ ({ __typeof__(value) _ipo_var_tmp = (value); ipo_var_set((name), &_ipo_var_tmp, sizeof(_ipo_var_tmp)); })

#define IPO_VAR_GET(name, out_var) \
    __extension__ ({ \
        __typeof__(*(out_var)) _ipo_var_tmp; \
        memset(&_ipo_var_tmp, 0, sizeof(_ipo_var_tmp)); \
        int _ipo_var_rc = ipo_var_get((name), &_ipo_var_tmp, sizeof(_ipo_var_tmp)); \
        if (_ipo_var_rc >= 0) { *(out_var) = _ipo_var_tmp; } \
        _ipo_var_rc; \
    })

#define IPO_VAR_SET_STR(name, value) ipo_var_set_str((name), (value))
#define IPO_VAR_GET_STR(name, buffer, buffer_size) ipo_var_get_str((name), (buffer), (buffer_size))

static inline int ipo_keymap_set_with_font(const char *name,
                                           const void *entries,
                                           uint32_t count,
                                           const void *glyphs,
                                           uint32_t glyph_count) {
    uint32_t args[5];
    args[0] = (uint32_t)(uintptr_t)name;
    args[1] = (uint32_t)(uintptr_t)entries;
    args[2] = count;
    args[3] = (uint32_t)(uintptr_t)glyphs;
    args[4] = glyph_count;
    return ipo_syscall(IPO_SYSCALL_KEYMAP_SET, 5u, args);
}

static inline int ipo_keymap_set(const char *name, const void *entries, uint32_t count) {
    return ipo_keymap_set_with_font(name, entries, count, NULL, 0u);
}

static inline int ipo_open(const char *path) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)path;
    return ipo_syscall(IPO_SYSCALL_FS_OPEN, 1u, args);
}

static inline int ipo_read(int fd, void *buf, uint32_t offset, uint32_t count) {
    uint32_t args[4];
    args[0] = (uint32_t)fd;
    args[1] = (uint32_t)(uintptr_t)buf;
    args[2] = offset;
    args[3] = count;
    return ipo_syscall(IPO_SYSCALL_FS_READ, 4u, args);
}

static inline int ipo_font_load_cyrillic(const char *path) {
    if (path != NULL) {
        uint32_t args[1];
        args[0] = (uint32_t)(uintptr_t)path;
        return ipo_syscall(IPO_SYSCALL_FONT_LOAD, 1u, args);
    }
    return ipo_syscall(IPO_SYSCALL_FONT_LOAD, 0u, NULL);
}

static inline int ipo_keymap_disable(const char *name) {
    if (name == NULL) return -1;
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)name;
    return ipo_syscall(IPO_SYSCALL_KEYMAP_DISABLE, 1u, args);
}

static inline int ipo_keymap_enable(const char *name) {
    if (name == NULL) return -1;
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)name;
    return ipo_syscall(IPO_SYSCALL_KEYMAP_ENABLE, 1u, args);
}

static inline int ipo_keymap_remove(const char *name) {
    if (name == NULL) return -1;
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)name;
    return ipo_syscall(IPO_SYSCALL_KEYMAP_REMOVE, 1u, args);
}

static inline int ipo_driver_register(void *drv) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)drv;
    return ipo_syscall(IPO_SYSCALL_DRIVER_REGISTER, 1u, args);
}

static inline int ipo_driver_unregister(const char *name) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)name;
    return ipo_syscall(IPO_SYSCALL_DRIVER_UNREGISTER, 1u, args);
}

#endif