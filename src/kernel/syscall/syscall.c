#include <syscall.h>
#include <stdio.h>
#include <string.h>
#include <file_system/ipo_fs.h>
#include <kernel/process.h>
#include <kernel/smp.h>
#include <kernel/ipc.h>
#include <memory/kmalloc.h>
#include <memory/memory_map.h>
#include <memory/address_space.h>
#include <kernel/terminal.h>
#include <kernel/async.h>
#include <driver/input/keyboard.h>
#include <driver/input/mouse.h>
#include <driver/input/keymap/keymap.h>
#include <driver/input/keymap/dynamic_keymap.h>
#include <kernel/driver.h>
#include <kernel/irq.h>
#include <kernel/input_ext.h>
#include <kernel/device.h>
#include <kernel/kmod.h>
#include <kernel/ext.h>
#include <kernel/dynmod.h>
#include <driver/pci.h>
#include <system/state.h>
#include <system/timer.h>
#include <system/rtc.h>
#include <driver/sound.h>
#include <driver/audio_core.h>
#include <vga.h>
#include <vga_gfx.h>
#include <wm.h>
#include <ioport.h>
#include <arch/cpu_regs.h>

static ipo_syscall_handler_t *ipo_syscall_table = NULL;
static ipo_uarg_t ipo_syscall_table_size = 0;

typedef struct {
    char *name;
    void *value;
    ipo_size_t value_len;
} ipo_shared_var_t;

static ipo_shared_var_t *ipo_shared_vars = NULL;
static uint32_t ipo_shared_var_count = 0u;
static uint32_t ipo_shared_var_capacity = 0u;

extern void syscall_trap_init(void);

static ipo_shared_var_t *find_shared_var_locked(const char *name) {
    if (name == NULL) {
        return NULL;
    }

    for (uint32_t i = 0u; i < ipo_shared_var_count; i++) {
        if (ipo_shared_vars[i].name != NULL &&
            strcmp(ipo_shared_vars[i].name, name) == 0) {
            return &ipo_shared_vars[i];
        }
    }

    return NULL;
}

static int set_shared_var(const char *name, const void *value, uint32_t value_len) {
    if (name == NULL) {
        return -1;
    }

    ipo_shared_var_t *existing = find_shared_var_locked(name);
    if (existing != NULL) {
        void *new_value = kmalloc(value_len == 0u ? 1u : value_len);
        if (new_value == NULL) {
            return -1;
        }

        if (value != NULL && value_len > 0u) {
            memcpy(new_value, value, value_len);
        } else {
            memset(new_value, 0, value_len == 0u ? 1u : value_len);
        }

        kfree(existing->value);
        existing->value = new_value;
        existing->value_len = value_len;
        return 0;
    }

    if (ipo_shared_var_count >= ipo_shared_var_capacity) {
        uint32_t new_capacity = (ipo_shared_var_capacity == 0u) ? 8u : ipo_shared_var_capacity * 2u;
        ipo_shared_var_t *new_table = kmalloc(new_capacity * sizeof(ipo_shared_var_t));
        if (new_table == NULL) {
            return -1;
        }

        memset(new_table, 0, new_capacity * sizeof(ipo_shared_var_t));
        if (ipo_shared_vars != NULL) {
            memcpy(new_table, ipo_shared_vars, ipo_shared_var_count * sizeof(ipo_shared_var_t));
            kfree(ipo_shared_vars);
        }

        ipo_shared_vars = new_table;
        ipo_shared_var_capacity = new_capacity;
    }

    size_t name_len = strlen(name) + 1u;
    char *name_copy = kmalloc(name_len);
    if (name_copy == NULL) {
        return -1;
    }
    memcpy(name_copy, name, name_len);

    void *value_copy = kmalloc(value_len == 0u ? 1u : value_len);
    if (value_copy == NULL) {
        kfree(name_copy);
        return -1;
    }

    if (value != NULL && value_len > 0u) {
        memcpy(value_copy, value, value_len);
    } else {
        memset(value_copy, 0, value_len == 0u ? 1u : value_len);
    }

    ipo_shared_vars[ipo_shared_var_count].name = name_copy;
    ipo_shared_vars[ipo_shared_var_count].value = value_copy;
    ipo_shared_vars[ipo_shared_var_count].value_len = value_len;
    ipo_shared_var_count++;

    return 0;
}

static int get_shared_var(const char *name, void *out_buffer, uint32_t out_capacity, uint32_t *out_len) {
    if (name == NULL || out_buffer == NULL || out_capacity == 0u) {
        return -1;
    }

    ipo_shared_var_t *entry = find_shared_var_locked(name);
    if (entry == NULL) {
        return -1;
    }

    uint32_t copy_len = entry->value_len;
    if (copy_len > out_capacity) {
        copy_len = out_capacity;
    }

    if (entry->value != NULL && copy_len > 0u) {
        memcpy(out_buffer, entry->value, copy_len);
    }

    if (out_len != NULL) {
        *out_len = entry->value_len;
    }

    return (int)copy_len;
}

static int delete_shared_var(const char *name) {
    if (name == NULL) {
        return -1;
    }

    for (uint32_t i = 0u; i < ipo_shared_var_count; i++) {
        if (ipo_shared_vars[i].name != NULL &&
            strcmp(ipo_shared_vars[i].name, name) == 0) {
            kfree(ipo_shared_vars[i].name);
            kfree(ipo_shared_vars[i].value);

            for (uint32_t j = i + 1u; j < ipo_shared_var_count; j++) {
                ipo_shared_vars[j - 1u] = ipo_shared_vars[j];
            }

            ipo_shared_var_count--;
            memset(&ipo_shared_vars[ipo_shared_var_count], 0, sizeof(ipo_shared_var_t));
            return 0;
        }
    }

    return -1;
}

void ipo_register_syscall(ipo_uarg_t num, ipo_syscall_handler_t handler) {
    if (num >= 65536u) {
        return;
    }

    if (ipo_syscall_table == NULL || num >= ipo_syscall_table_size) {
        ipo_uarg_t new_size = ipo_syscall_table_size ? ipo_syscall_table_size : 64u;
        while (new_size <= num) {
            new_size *= 2u;
        }

        ipo_syscall_handler_t *new_table =
            (ipo_syscall_handler_t *)kmalloc(
                (size_t)new_size * sizeof(ipo_syscall_handler_t));

        if (new_table == NULL) {
            return;
        }

        if (ipo_syscall_table != NULL) {
            memcpy(new_table,
                   ipo_syscall_table,
                   (size_t)ipo_syscall_table_size * sizeof(ipo_syscall_handler_t));
            kfree(ipo_syscall_table);
        }

        memset(new_table + ipo_syscall_table_size,
               0,
               (size_t)(new_size - ipo_syscall_table_size) *
                   sizeof(ipo_syscall_handler_t));

        ipo_syscall_table = new_table;
        ipo_syscall_table_size = new_size;
    }

    ipo_syscall_table[num] = handler;
}

void ipo_unregister_syscall(ipo_uarg_t num) {
    if (ipo_syscall_table != NULL && num < ipo_syscall_table_size) {
        ipo_syscall_table[num] = NULL;
    }
}

ipo_syscall_handler_t ipo_get_syscall_handler(ipo_uarg_t num) {
    if (ipo_syscall_table != NULL && num < ipo_syscall_table_size) {
        return ipo_syscall_table[num];
    }
    return NULL;
}

static ipo_uarg_t syscall_builtin_register(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t syscall_num = argv[0];
    uint32_t handler_ptr = argv[1];

    if (syscall_num < 0x2000u || syscall_num >= 65536u || handler_ptr == 0u) {
        return IPO_SYSCALL_ENOSYS;
    }

    if (process_in_process_context() &&
        !ipc_user_range((const void *)(uintptr_t)handler_ptr, 1u)) {
        return IPO_SYSCALL_ENOSYS;
    }

    ipo_register_syscall(
        syscall_num,
        (ipo_syscall_handler_t)(uintptr_t)handler_ptr);

    return IPO_SYSCALL_OK;
}

static ipo_uarg_t syscall_builtin_call(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t syscall_num = argv[0];

    if (syscall_num >= ipo_syscall_table_size) {
        return IPO_SYSCALL_ENOSYS;
    }

    ipo_syscall_handler_t fn = ipo_syscall_table[syscall_num];

    if (fn == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return fn(syscall_num,
              argc > 0u ? argc - 1u : 0u,
              argc > 1u ? &argv[1] : NULL);
}

static bool syscall_user_buf_ok(const void *ptr, ipo_size_t length) {
    if (!process_in_process_context()) {
        return true;
    }
    if (length == 0u) {
        return true;
    }
    return ptr != NULL && ipc_user_range(ptr, length);
}

static bool syscall_user_str_ok(const char *s) {
    if (!process_in_process_context()) {
        return true;
    }
    if (s == NULL) {
        return false;
    }
    for (ipo_size_t i = 0u; ; i++) {
        if (!ipc_user_range(&s[i], 1u)) {
            return false;
        }
        if (s[i] == '\0') {
            return true;
        }
    }
}

bool ipo_syscall_user_buf_ok(const void *ptr, ipo_size_t length) {
    return syscall_user_buf_ok(ptr, length);
}

bool ipo_syscall_user_str_ok(const char *s) {
    return syscall_user_str_ok(s);
}

static ipo_uarg_t syscall_builtin_print(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *str = (const char *)(uintptr_t)argv[0];
    if (!syscall_user_str_ok(str) || str == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)printf("%s", str);
}

static ipo_uarg_t syscall_builtin_write(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *text = (const char *)(uintptr_t)argv[0];

    if (text == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t count = 0;
    if (argc >= 2u) {
        uint32_t len = argv[1];
        if (!syscall_user_buf_ok(text, len)) {
            return IPO_SYSCALL_ENOSYS;
        }
        putchar_write(text, (ipo_size_t)len);
        count = len;
    } else {
        if (!syscall_user_str_ok(text)) {
            return IPO_SYSCALL_ENOSYS;
        }
        count = (uint32_t)strlen(text);
        putchar_write(text, (ipo_size_t)count);
    }

    return count;
}

static ipo_uarg_t syscall_builtin_fs_open(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *path = (const char *)(uintptr_t)argv[0];
    if (!syscall_user_str_ok(path)) {
        return (uint32_t)-1;
    }
    serial_printf("[fs_open] path='%s'\n", path ? path : "NULL");

    return (uint32_t)ipo_fs_open(path);
}

static ipo_uarg_t syscall_builtin_fs_close(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)ipo_fs_close((int)argv[0]);
}

static ipo_uarg_t syscall_builtin_fs_read(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    int fd = (int)argv[0];
    void *buf = (void *)(uintptr_t)argv[1];
    uint32_t count = argv[2];
    if (!syscall_user_buf_ok(buf, count)) {
        return (uint32_t)-1;
    }
    uint32_t offset = 0;

    if (argc >= 4u) {
        offset = argv[3];
    } else {
        if (fd < 0 || (uint32_t)fd >= fds_capacity || !fds || !fds[fd].used) return (uint32_t)-1;
        offset = fds[fd].offset;
    }

    int res = ipo_fs_read(fd, buf, count, offset);
    if (res > 0 && argc < 4u) {
        fds[fd].offset += (uint32_t)res;
    }
    return (uint32_t)res;
}

static ipo_uarg_t syscall_builtin_fs_write(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    int fd = (int)argv[0];
    const void *buf = (const void *)(uintptr_t)argv[1];
    uint32_t count = argv[2];
    if (!syscall_user_buf_ok(buf, count)) {
        return (uint32_t)-1;
    }
    uint32_t offset = 0;

    if (argc >= 4u) {
        offset = argv[3];
    } else {
        if (fd < 0 || (uint32_t)fd >= fds_capacity || !fds || !fds[fd].used) return (uint32_t)-1;
        offset = fds[fd].offset;
    }

    int res = ipo_fs_write(fd, buf, count, offset);
    if (res > 0 && argc < 4u) {
        fds[fd].offset += (uint32_t)res;
    }
    return (uint32_t)res;
}

static ipo_uarg_t syscall_builtin_fs_seek(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return (uint32_t)-1;
    }

    int fd = (int)argv[0];
    int32_t offset = (int32_t)argv[1];
    int whence = (int)argv[2];

    if (fd < 0 || (uint32_t)fd >= fds_capacity || !fds || !fds[fd].used) {
        return (uint32_t)-1;
    }

    struct ipo_inode inode;
    if (!read_inode(fds[fd].inode, &inode)) {
        return (uint32_t)-1;
    }

    int32_t new_pos = 0;
    if (whence == 0) { // SEEK_SET
        new_pos = offset;
    } else if (whence == 1) { // SEEK_CUR
        new_pos = (int32_t)fds[fd].offset + offset;
    } else if (whence == 2) { // SEEK_END
        new_pos = (int32_t)inode.size + offset;
    } else {
        return (uint32_t)-1;
    }

    if (new_pos < 0) {
        return (uint32_t)-1;
    }

    fds[fd].offset = (uint32_t)new_pos;
    return (uint32_t)new_pos;
}

static ipo_uarg_t syscall_builtin_fs_delete(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *path = (const char *)(uintptr_t)argv[0];
    if (!syscall_user_str_ok(path)) {
        return 1u;
    }

    return (uint32_t)(ipo_fs_delete(path) ? 0u : 1u);
}

static ipo_uarg_t syscall_builtin_fs_stat(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    struct ipo_inode *st =
        (struct ipo_inode *)(uintptr_t)argv[1];

    if (st == NULL || !syscall_user_buf_ok(st, sizeof(*st))) {
        return 1u;
    }

    const char *path = (const char *)(uintptr_t)argv[0];
    if (!syscall_user_str_ok(path)) {
        return 1u;
    }
    bool res = ipo_fs_stat(path, st);
    serial_printf("[fs_stat] path='%s' res=%d\n", path ? path : "NULL", res);

    return (uint32_t)(res ? 0u : 1u);
}

static ipo_uarg_t syscall_builtin_fs_statvfs(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return IPO_SYSCALL_ENOSYS;
    uint32_t *out = (uint32_t *)(uintptr_t)argv[0];
    if (out == NULL ||
        (process_in_process_context() && !ipc_user_range(out, 7u * sizeof(uint32_t))) ||
        !fs_mounted) {
        return 1u;
    }

    uint32_t data_start = (uint32_t)sb.data_blocks_start;
    uint32_t total = (sb.fs_size_blocks > data_start)
                     ? (uint32_t)(sb.fs_size_blocks - data_start) : 0;
    uint32_t used = total ? bitmap_count_set(sb.block_bitmap_start, total) : 0;
    uint64_t ino_total = 0, ino_used = 0, ino_free = 0, dyn_chunks = 0;
    ipo_fs_get_inode_stats(&ino_total, &ino_used, &ino_free, &dyn_chunks);

    out[0] = sb.block_size;                 /* f_bsize */
    out[1] = sb.block_size;                 /* f_frsize */
    out[2] = total;                         /* f_blocks */
    out[3] = total > used ? total - used : 0; /* f_bfree */
    out[4] = out[3];                        /* f_bavail */
    out[5] = (uint32_t)ino_total;           /* f_files */
    out[6] = (uint32_t)ino_free;            /* f_ffree */
    return 0u;
}

static ipo_uarg_t syscall_builtin_fs_list(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *path = (const char *)(uintptr_t)argv[0];
    char *out = (char *)(uintptr_t)argv[1];
    int out_size = (int)argv[2];
    if (!syscall_user_str_ok(path) || out_size < 0 ||
        !syscall_user_buf_ok(out, (uint32_t)out_size)) {
        return (uint32_t)-1;
    }

    return (uint32_t)ipo_fs_list_dir(path, out, out_size);
}

static ipo_uarg_t syscall_builtin_fs_rename(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *oldpath = (const char *)(uintptr_t)argv[0];
    const char *newpath = (const char *)(uintptr_t)argv[1];
    if (!syscall_user_str_ok(oldpath) || !syscall_user_str_ok(newpath)) {
        return 1u;
    }

    return (uint32_t)(ipo_fs_rename(oldpath, newpath) ? 0u : 1u);
}

static ipo_uarg_t syscall_builtin_exec(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)process_exec(
        (const char *)(uintptr_t)argv[0],
        (int)argv[1],
        (char **)(uintptr_t)argv[2]);
}

static ipo_uarg_t syscall_builtin_getcwd(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || argv == NULL) return (uint32_t)0;
    char *buf = (char *)(uintptr_t)argv[0];
    uint32_t size = argv[1];
    if (!buf || size == 0 || !syscall_user_buf_ok(buf, size)) return (uint32_t)0;
    const char *cwd = terminal_get_cwd();
    if (!cwd) cwd = "/";
    strncpy(buf, cwd, size - 1);
    buf[size - 1] = '\0';
    return (ipo_uarg_t)(uintptr_t)buf;
}

static ipo_uarg_t syscall_builtin_chdir(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return (uint32_t)-1;
    const char *path = (const char *)(uintptr_t)argv[0];
    if (!syscall_user_str_ok(path)) return (uint32_t)-1;
    return (uint32_t)terminal_set_cwd(path);
}

static ipo_uarg_t syscall_builtin_get_exit_code(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)process_get_exit_code();
}

static ipo_uarg_t syscall_builtin_set_return_value(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || argv == NULL) return (uint32_t)-1;
    process_return_type_t type = (process_return_type_t)argv[0];
    const char *type_name = "";
    const char *str_val = NULL;
    int32_t int_val = 0;
    const void *data = NULL;
    uint32_t size = 0;

    if (type == PROC_RET_INT) {
        int_val = (int32_t)argv[1];
        type_name = "int";
    } else if (type == PROC_RET_CHAR) {
        int_val = (int32_t)(char)argv[1];
        type_name = "char";
    } else if (type == PROC_RET_STRING) {
        str_val = (const char *)(uintptr_t)argv[1];
        if (argc > 2u && argv[2] != 0) {
            type_name = (const char *)(uintptr_t)argv[2];
        } else {
            type_name = "char*";
        }
    } else if (type == PROC_RET_STRUCT) {
        data = (const void *)(uintptr_t)argv[1];
        size = argc > 2u ? argv[2] : 0;
        if (argc > 3u && argv[3] != 0) {
            type_name = (const char *)(uintptr_t)argv[3];
        } else {
            type_name = "struct";
        }
    } else if (type == PROC_RET_POINTER) {
        data = (const void *)(uintptr_t)argv[1];
        type_name = "void*";
    } else if ((uint32_t)type == 99u) {
        if (argc > 1u && argv[1] != 0) {
            process_set_return_format(process_get_current(), (const char *)(uintptr_t)argv[1]);
        }
        return 0;
    }

    process_set_current_return_value(type, type_name, str_val, int_val, data, size);
    return 0;
}

static ipo_uarg_t syscall_builtin_get_return_value(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || argv == NULL) return (uint32_t)-1;
    char *buf = (char *)(uintptr_t)argv[0];
    uint32_t max_len = argv[1];
    if (!buf || max_len == 0 || !syscall_user_buf_ok(buf, max_len)) return (uint32_t)-1;

    const char *val_str = process_get_last_return_string();
    if (!val_str) val_str = "";
    strncpy(buf, val_str, max_len - 1);
    buf[max_len - 1] = '\0';
    return (uint32_t)strlen(buf);
}

static ipo_uarg_t syscall_builtin_terminal_input(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *text = (const char *)(uintptr_t)argv[0];
    if (!syscall_user_str_ok(text)) {
        return IPO_SYSCALL_ENOSYS;
    }
    int auto_exec = (int)argv[1];
    terminal_inject_input(text, auto_exec != 0);
    return IPO_SYSCALL_OK;
}

static ipo_uarg_t syscall_builtin_keymap_set(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    const char *name = (const char *)(uintptr_t)argv[0];
    const keymap_entry_t *entries = (const keymap_entry_t *)(uintptr_t)argv[1];
    uint32_t count = argv[2];
    const dynamic_glyph_def_t *glyphs = (argc >= 5) ? (const dynamic_glyph_def_t *)(uintptr_t)argv[3] : NULL;
    uint32_t glyph_count = (argc >= 5) ? argv[4] : 0u;

    int res = dynamic_keymap_register_with_font(name, entries, count, glyphs, glyph_count);
    terminal_render_language_bar();
    return (uint32_t)res;
}

static ipo_uarg_t syscall_builtin_keymap_get(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc == 0 || argv == NULL) {
        return (uint32_t)(uintptr_t)dynamic_keymap_get_name_local();
    }

    uint32_t cmd = argv[0];
    switch (cmd) {
        case 0: { /* active layout name */
            const char *name = dynamic_keymap_get_name_local();
            if (argc >= 3u && argv[1] != 0 && argv[2] > 0) {
                char *buf = (char *)(uintptr_t)argv[1];
                uint32_t max_len = argv[2];
                if (!syscall_user_buf_ok(buf, max_len)) return IPO_SYSCALL_ENOSYS;
                strncpy(buf, name ? name : "Unknown", max_len - 1);
                buf[max_len - 1] = '\0';
                return 0;
            }
            return (uint32_t)(uintptr_t)name;
        }
        case 1: { /* scancode -> char lookup table */
            if (argc >= 3u && argv[1] != 0 && argv[2] > 0) {
                char *out_table = (char *)(uintptr_t)argv[1];
                uint32_t max_len = argv[2];
                if (!syscall_user_buf_ok(out_table, max_len)) return IPO_SYSCALL_ENOSYS;
                bool shift = (argc >= 4u) ? (bool)argv[3] : false;
                memset(out_table, 0, max_len);
                for (uint32_t sc = 0; sc < max_len; sc++) {
                    const char *s = dynamic_keymap_translate_local((uint8_t)sc, shift);
                    if (s != NULL && s[0] != '\0') {
                        out_table[sc] = s[0];
                    }
                }
                return 0;
            }
            return IPO_SYSCALL_ENOSYS;
        }
        case 2: { /* copy entries: argv[1]=slot, argv[2]=out, argv[3]=max */
            if (argc >= 4u && argv[2] != 0 && argv[3] > 0) {
                uint32_t slot = argv[1];
                keymap_entry_t *out = (keymap_entry_t *)(uintptr_t)argv[2];
                uint32_t max_count = argv[3];
                if (max_count > 0xFFFFFFFFu / sizeof(keymap_entry_t) ||
                    !syscall_user_buf_ok(out, max_count * sizeof(keymap_entry_t))) {
                    return 0;
                }
                return dynamic_keymap_copy_entries(slot, out, max_count);
            }
            return 0;
        }
        case 3: { /* layout count */
            return dynamic_keymap_get_count();
        }
        case 4: { /* entry count: argv[1]=slot */
            if (argc >= 2u) {
                return dynamic_keymap_get_entries_count(argv[1]);
            }
            return 0;
        }
        case 5: { /* slot name: argv[1]=slot, argv[2]=buf, argv[3]=max */
            if (argc >= 4u && argv[2] != 0 && argv[3] > 0) {
                uint32_t slot = argv[1];
                char *buf = (char *)(uintptr_t)argv[2];
                uint32_t max_len = argv[3];
                if (!syscall_user_buf_ok(buf, max_len)) return IPO_SYSCALL_ENOSYS;
                const char *sname = dynamic_keymap_get_slot_name(slot);
                strncpy(buf, sname ? sname : "Unknown", max_len - 1);
                buf[max_len - 1] = '\0';
                return 0;
            }
            return IPO_SYSCALL_ENOSYS;
        }
        case 6: { /* slot enabled: argv[1]=slot */
            if (argc >= 2u) {
                return dynamic_keymap_is_slot_enabled(argv[1]) ? 1u : 0u;
            }
            return 0;
        }
        default:
            break;
    }

    return (uint32_t)(uintptr_t)dynamic_keymap_get_name_local();
}

static ipo_uarg_t syscall_builtin_font_load(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    const char *path = (argc >= 1u && argv != NULL) ? (const char *)(uintptr_t)argv[0] : NULL;
    int res = vga_font_load_database(path);
    return (res == 0) ? IPO_SYSCALL_OK : IPO_SYSCALL_ENOSYS;
}

static ipo_uarg_t syscall_builtin_font_get_info(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    char *name_buf = (argc >= 1u && argv != NULL) ? (char *)(uintptr_t)argv[0] : NULL;
    uint32_t max_len = (argc >= 2u && argv != NULL) ? argv[1] : 0;
    uint32_t *out_count = (argc >= 3u && argv != NULL) ? (uint32_t *)(uintptr_t)argv[2] : NULL;
    return (uint32_t)vga_font_get_info(name_buf, max_len, out_count);
}

static ipo_uarg_t syscall_builtin_vga_glyph(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return (uint32_t)-1;
    if (argc >= 2u && argv[0] != 0) {
        const char *str = (const char *)(uintptr_t)argv[0];
        size_t str_len = (size_t)argv[1];
        if (!syscall_user_buf_ok(str, (uint32_t)str_len)) {
            return (uint32_t)-1;
        }
        size_t consumed = 0;
        uint8_t glyph = utf8_to_vga_glyph(str, str_len, &consumed);
        if (argc >= 3u && argv[2] != 0) {
            uint32_t *out_consumed = (uint32_t *)(uintptr_t)argv[2];
            if (!syscall_user_buf_ok(out_consumed, sizeof(uint32_t))) {
                return (uint32_t)-1;
            }
            *out_consumed = (uint32_t)consumed;
        }
        return (uint32_t)glyph;
    }
    uint32_t codepoint = argv[0];
    int slot = vga_font_register_glyph(codepoint, NULL);
    return (uint32_t)slot;
}

static ipo_uarg_t syscall_builtin_keymap_disable(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return IPO_SYSCALL_ENOSYS;
    const char *target = (const char *)(uintptr_t)argv[0];
    return (uint32_t)dynamic_keymap_disable(target);
}

static ipo_uarg_t syscall_builtin_keymap_enable(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return IPO_SYSCALL_ENOSYS;
    const char *target = (const char *)(uintptr_t)argv[0];
    return (uint32_t)dynamic_keymap_enable(target);
}

static ipo_uarg_t syscall_builtin_keymap_remove(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return IPO_SYSCALL_ENOSYS;
    const char *target = (const char *)(uintptr_t)argv[0];
    return (uint32_t)dynamic_keymap_remove(target);
}

static ipo_uarg_t syscall_builtin_keymap_translate(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || argv == NULL) return 0;
    uint8_t scancode = (uint8_t)argv[0];
    bool shift = (bool)argv[1];
    const char *s = dynamic_keymap_translate_local(scancode, shift);
    return (uint32_t)(uintptr_t)s;
}

static ipo_uarg_t syscall_builtin_keymap_is_active(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)dynamic_keymap_is_active_local();
}

static ipo_uarg_t syscall_builtin_keymap_cycle_next(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    dynamic_keymap_cycle_next_local();
    return 0;
}

static ipo_uarg_t syscall_builtin_keymap_cycle_prev(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    dynamic_keymap_cycle_prev_local();
    return 0;
}

static ipo_uarg_t syscall_builtin_keymap_get_name(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    const char *name = dynamic_keymap_get_name_local();
    if (argc >= 2u && argv != NULL && argv[0] != 0 && argv[1] > 0) {
        char *user_buf = (char *)(uintptr_t)argv[0];
        uint32_t max_len = argv[1];
        strncpy(user_buf, name ? name : default_keymap_name, max_len - 1);
        user_buf[max_len - 1] = '\0';
        return 0;
    }
    return (uint32_t)(uintptr_t)name;
}

static ipo_uarg_t syscall_builtin_driver_register(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    driver_t *drv = (driver_t *)(uintptr_t)argv[0];
    int res = driver_register(drv);
    return (uint32_t)res;
}

static ipo_uarg_t syscall_builtin_driver_unregister(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    const char *name = (const char *)(uintptr_t)argv[0];
    int res = driver_unregister(name);
    return (uint32_t)res;
}

static ipo_uarg_t syscall_builtin_driver_list(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    driver_print_list();
    return IPO_SYSCALL_OK;
}

static ipo_uarg_t syscall_builtin_driver_set_desc(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    const char *name = (const char *)(uintptr_t)argv[0];
    const char *desc = (const char *)(uintptr_t)argv[1];
    int res = driver_set_description(name, desc);
    return (uint32_t)res;
}

static ipo_uarg_t syscall_builtin_port_io(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    if (argv == NULL) return IPO_SYSCALL_ENOSYS;

    switch (num) {
        case IPO_SYSCALL_PORT_IN8:
            if (argc < 1u) return IPO_SYSCALL_ENOSYS;
            return inb((uint16_t)argv[0]);
        case IPO_SYSCALL_PORT_OUT8:
            if (argc < 2u) return IPO_SYSCALL_ENOSYS;
            outb((uint16_t)argv[0], (uint8_t)argv[1]);
            return IPO_SYSCALL_OK;
        case IPO_SYSCALL_PORT_IN16:
            if (argc < 1u) return IPO_SYSCALL_ENOSYS;
            return inw((uint16_t)argv[0]);
        case IPO_SYSCALL_PORT_OUT16:
            if (argc < 2u) return IPO_SYSCALL_ENOSYS;
            outw((uint16_t)argv[0], (uint16_t)argv[1]);
            return IPO_SYSCALL_OK;
        case IPO_SYSCALL_PORT_IN32:
            if (argc < 1u) return IPO_SYSCALL_ENOSYS;
            return inl((uint16_t)argv[0]);
        case IPO_SYSCALL_PORT_OUT32:
            if (argc < 2u) return IPO_SYSCALL_ENOSYS;
            outl((uint16_t)argv[0], argv[1]);
            return IPO_SYSCALL_OK;
        default:
            return IPO_SYSCALL_ENOSYS;
    }
}

static ipo_uarg_t syscall_builtin_binfmt_register(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 5u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    const char *name = (const char *)(uintptr_t)argv[0];
    const uint8_t *magic = (const uint8_t *)(uintptr_t)argv[1];
    uint32_t magic_len = argv[2];
    uint32_t offset = argv[3];
    const char *runner_path = (const char *)(uintptr_t)argv[4];

    int res = binfmt_register(name, magic, magic_len, offset, runner_path);
    return (uint32_t)res;
}

static ipo_uarg_t syscall_builtin_binfmt_unregister(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    const char *name = (const char *)(uintptr_t)argv[0];
    int res = binfmt_unregister(name);
    return (uint32_t)res;
}

static ipo_uarg_t syscall_builtin_binfmt_list(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    binfmt_list();
    return IPO_SYSCALL_OK;
}

static uint32_t utf8_prev_char(const char *buf, uint32_t pos) {
    if (pos == 0u) return 0u;
    pos--;
    while (pos > 0u && ((unsigned char)buf[pos] & 0xC0u) == 0x80u) {
        pos--;
    }
    return pos;
}

static uint32_t utf8_next_char(const char *buf, uint32_t len, uint32_t pos) {
    if (pos >= len) return len;
    pos++;
    while (pos < len && ((unsigned char)buf[pos] & 0xC0u) == 0x80u) {
        pos++;
    }
    return pos;
}

static ipo_uarg_t syscall_builtin_read(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    if (process_in_process_context() &&
        !ipc_user_range(argv, 2u * sizeof(uint32_t))) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t max_len = argv[1];
    char **out_ptr = NULL;
    char *buffer = NULL;
    uint32_t capacity = 0u;

    if (max_len == 0u) {
        out_ptr = (char **)(uintptr_t)argv[0];
        if (out_ptr == NULL ||
            (process_in_process_context() && !ipc_user_range(out_ptr, sizeof(char *)))) {
            return IPO_SYSCALL_ENOSYS;
        }
        capacity = 256u;
        buffer = (char *)kmalloc(capacity);
        if (buffer == NULL) {
            return IPO_SYSCALL_ENOSYS;
        }
    } else {
        buffer = (char *)(uintptr_t)argv[0];
        if (buffer == NULL ||
            (process_in_process_context() && !ipc_user_range(buffer, max_len))) {
            return IPO_SYSCALL_ENOSYS;
        }
        capacity = max_len;
    }

    process_t *proc = process_get_current();
    uint32_t pid = proc ? proc->pid : 0u;

    /* Wait for foreground focus before reading console input */
    while (!process_is_foreground()) {
        if (proc) {
            proc->waiting_for_input = true;
        }
        process_yield();
        if (proc && !proc->is_running) {
            if (max_len == 0u && out_ptr != NULL) {
                kfree(buffer);
                *out_ptr = NULL;
            }
            return (uint32_t)(-2);
        }
    }
    if (proc) {
        proc->waiting_for_input = false;
    }

    system_state_t prev_state = system_get_state();
    system_set_state(SYSTEM_STATE_TEXT_INPUT);
    keyboard_set_app_input_mode(true);

    serial_printf("[syscall_read] started: pid=%u max_len=%u (state=TEXT_INPUT)\n", pid, max_len);

    uint32_t len = 0u;
    uint32_t cursor_pos = 0u;
    size_t prev_vis_len = 0u;
    int32_t start_origin = (int32_t)vga_get_cursor_position();
    if (start_origin < VGA_START_CURSOR_POSITION || start_origin >= VGA_WIDTH * VGA_HEIGHT) {
        start_origin = VGA_START_CURSOR_POSITION;
        vga_set_cursor((uint16_t)start_origin);
    }
    int32_t start_cursor = start_origin;
    buffer[0] = '\0';
    bool was_backgrounded = false;

    for (;;) {
        if (!process_is_foreground()) {
            if (proc) {
                proc->waiting_for_input = true;
            }
            was_backgrounded = true;
            process_yield();
            if (proc && !proc->is_running) {
                keyboard_set_app_input_mode(false);
                system_set_state(proc ? SYSTEM_STATE_PROCESS_RUNNING : prev_state);
                if (max_len == 0u && out_ptr != NULL) {
                    kfree(buffer);
                    *out_ptr = NULL;
                }
                return (uint32_t)(-2);
            }
            continue;
        }
        if (proc) {
            proc->waiting_for_input = false;
        }
        if (was_backgrounded) {
            was_backgrounded = false;
            system_set_state(SYSTEM_STATE_TEXT_INPUT);
            keyboard_set_app_input_mode(true);
            if (len == 0u) {
                start_origin = (int32_t)vga_get_cursor_position();
                if (start_origin < VGA_START_CURSOR_POSITION || start_origin >= VGA_WIDTH * VGA_HEIGHT) {
                    start_origin = VGA_START_CURSOR_POSITION;
                    vga_set_cursor((uint16_t)start_origin);
                }
                start_cursor = start_origin;
                prev_vis_len = 0u;
            }
            vga_show_cursor();
        }

        if (system_is_interrupted()) {
            if (process_is_foreground()) {
                keyboard_set_app_input_mode(false);
                system_set_state(proc ? SYSTEM_STATE_PROCESS_RUNNING : prev_state);
                if (max_len == 0u && out_ptr != NULL) {
                    kfree(buffer);
                    *out_ptr = NULL;
                }
                return (uint32_t)(-2);
            }
            process_yield();
            continue;
        }

        uint8_t scancode = keyboard_get_scancode();
        if (scancode == 0x00u) {
            process_yield();
            io_wait();
            continue;
        }
        process_yield();

        update_hot_key_state(scancode);

        if (scancode & 0x80u) {
            continue;
        }

        if (keyboard_is_ctrl_pressed() && scancode == 0x2E) {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            size_t vis_len = terminal_visual_offset_range(buffer, len, len);
            int32_t cur = start_cursor + (int32_t)vis_len;
            if (cur < VGA_START_CURSOR_POSITION) cur = VGA_START_CURSOR_POSITION;
            if (cur >= VGA_WIDTH * VGA_HEIGHT) cur = VGA_WIDTH * VGA_HEIGHT - 1;
            vga_set_cursor((uint16_t)cur);
            printf("^C\n");
            system_request_interrupt();
            keyboard_set_app_input_mode(false);
            system_set_state(proc ? SYSTEM_STATE_PROCESS_RUNNING : prev_state);
            if (max_len == 0u && out_ptr != NULL) {
                kfree(buffer);
                *out_ptr = NULL;
            }
            return (uint32_t)(-2);
        }

        if (scancode == 0x20 && keyboard_is_ctrl_pressed()) {
            if (len == 0u) {
                keyboard_set_app_input_mode(false);
                system_set_state(proc ? SYSTEM_STATE_PROCESS_RUNNING : prev_state);
                if (max_len == 0u && out_ptr != NULL) {
                    kfree(buffer);
                    *out_ptr = NULL;
                }
                return (uint32_t)(-3);
            }
        }

        if (scancode == 0x49) {
            if (terminal_get_top_buffer_count() > 0) {
                terminal_scroll_up();
                start_cursor += VGA_WIDTH;
            }
            continue;
        }
        if (scancode == 0x51) {
            if (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            continue;
        }
        if (scancode == 0x48) {
            if (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_up();
                start_cursor += VGA_WIDTH;
                continue;
            }
            if (cursor_pos >= VGA_WIDTH) {
                cursor_pos -= VGA_WIDTH;
                while (cursor_pos > 0u && ((unsigned char)buffer[cursor_pos] & 0xC0u) == 0x80u) {
                    cursor_pos--;
                }
                terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                          &prev_vis_len, cursor_pos,
                                          proc ? proc->text_vram_backup : NULL, true);
            } else if (cursor_pos > 0) {
                cursor_pos = 0u;
                terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                          &prev_vis_len, cursor_pos,
                                          proc ? proc->text_vram_backup : NULL, true);
            }
            continue;
        }
        if (scancode == 0x50) {
            if (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
                continue;
            }
            if (cursor_pos + VGA_WIDTH <= len) {
                cursor_pos += VGA_WIDTH;
                while (cursor_pos < len && ((unsigned char)buffer[cursor_pos] & 0xC0u) == 0x80u) {
                    cursor_pos++;
                }
                terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                          &prev_vis_len, cursor_pos,
                                          proc ? proc->text_vram_backup : NULL, true);
            } else if (cursor_pos < len) {
                cursor_pos = len;
                terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                          &prev_vis_len, cursor_pos,
                                          proc ? proc->text_vram_backup : NULL, true);
            }
            continue;
        }
        if (scancode == 0x4B) {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            if (cursor_pos > 0u) {
                cursor_pos = utf8_prev_char(buffer, cursor_pos);
                terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                          &prev_vis_len, cursor_pos,
                                          proc ? proc->text_vram_backup : NULL, true);
            }
            continue;
        }
        if (scancode == 0x4D) {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            if (cursor_pos < len) {
                cursor_pos = utf8_next_char(buffer, len, cursor_pos);
                terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                          &prev_vis_len, cursor_pos,
                                          proc ? proc->text_vram_backup : NULL, true);
            }
            continue;
        }
        if (scancode == 0x47) {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            cursor_pos = 0u;
            terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                      &prev_vis_len, cursor_pos,
                                      proc ? proc->text_vram_backup : NULL, true);
            continue;
        }
        if (scancode == 0x4F) {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            cursor_pos = len;
            terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                      &prev_vis_len, cursor_pos,
                                      proc ? proc->text_vram_backup : NULL, true);
            continue;
        }
        if (scancode == 0x53) {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            if (cursor_pos < len) {
                uint32_t next_pos = utf8_next_char(buffer, len, cursor_pos);
                uint32_t del_bytes = next_pos - cursor_pos;
                memmove(&buffer[cursor_pos], &buffer[next_pos], len - next_pos + 1u);
                len -= del_bytes;
                buffer[len] = '\0';
                terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                          &prev_vis_len, cursor_pos,
                                          proc ? proc->text_vram_backup : NULL, true);
            }
            continue;
        }

        char ch = get_char(scancode);

        if (ch == '\r' || ch == '\n') {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            size_t vis_len = terminal_visual_offset_range(buffer, len, len);
            int32_t cur = start_cursor + (int32_t)vis_len;
            if (cur < VGA_START_CURSOR_POSITION) cur = VGA_START_CURSOR_POSITION;
            if (cur >= VGA_WIDTH * VGA_HEIGHT) cur = VGA_WIDTH * VGA_HEIGHT - 1;
            vga_set_cursor((uint16_t)cur);
            if (proc && !proc->wants_graphics && !proc->is_wm_app && len > 0u) {
                process_append_text_output(proc, buffer, len);
                if (process_is_batch_active() && !process_get_separate_windows() &&
                    (!vga_is_graphics_mode() || vga_gfx_has_saved_text())) {
                    proc->text_vga_written_len = proc->text_output_len;
                }
            }
            putchar('\n');
            buffer[len] = '\0';
            break;
        }

        if (ch == '\b' || ch == 127) {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            if (cursor_pos > 0u) {
                uint32_t prev_pos = utf8_prev_char(buffer, cursor_pos);
                uint32_t del_bytes = cursor_pos - prev_pos;
                memmove(&buffer[prev_pos], &buffer[cursor_pos], len - cursor_pos + 1u);
                cursor_pos = prev_pos;
                len -= del_bytes;
                buffer[len] = '\0';
                terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                          &prev_vis_len, cursor_pos,
                                          proc ? proc->text_vram_backup : NULL, true);
            }
            continue;
        }

        if (ch == '\t') {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            if (max_len == 0u) {
                if (len + 1u >= capacity) {
                    if (capacity > UINT32_MAX / 2u - 16u) {
                        continue;
                    }
                    uint32_t new_cap = (capacity + 16u) * 2u;
                    char *new_buf = (char *)kmalloc(new_cap);
                    if (new_buf == NULL) {
                        continue;
                    }
                    memcpy(new_buf, buffer, len + 1);
                    kfree(buffer);
                    buffer = new_buf;
                    capacity = new_cap;
                }
            } else {
                if (len + 1u >= capacity) continue;
            }

            memmove(&buffer[cursor_pos + 1], &buffer[cursor_pos], len - cursor_pos + 1);
            buffer[cursor_pos] = '\t';
            cursor_pos++;
            len++;
            buffer[len] = '\0';
            terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                      &prev_vis_len, cursor_pos,
                                      proc ? proc->text_vram_backup : NULL, true);
            continue;
        }

        const char *out_str = keyboard_get_key_string(scancode);
        if (out_str != NULL && out_str[0] != '\0') {
            size_t slen = strlen(out_str);
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            if (max_len == 0u) {
                if (slen >= (size_t)(capacity - len)) {
                    if (slen > UINT32_MAX / 2u || capacity > UINT32_MAX / 2u - slen - 32u) {
                        continue;
                    }
                    uint32_t new_cap = (capacity + (uint32_t)slen + 32u) * 2u;
                    char *new_buf = (char *)kmalloc(new_cap);
                    if (new_buf == NULL) {
                        continue;
                    }
                    memcpy(new_buf, buffer, len + 1);
                    kfree(buffer);
                    buffer = new_buf;
                    capacity = new_cap;
                }
            } else {
                if (slen >= (size_t)(capacity - len)) {
                    continue;
                }
            }

            memmove(&buffer[cursor_pos + slen], &buffer[cursor_pos], len - cursor_pos + 1);
            memcpy(&buffer[cursor_pos], out_str, slen);
            cursor_pos += (uint32_t)slen;
            len += (uint32_t)slen;
            buffer[len] = '\0';
            terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                      &prev_vis_len, cursor_pos,
                                      proc ? proc->text_vram_backup : NULL, true);
        }
    }

    buffer[len] = '\0';

    if (max_len == 0u && out_ptr != NULL) {
        *out_ptr = buffer;
    }

    keyboard_set_app_input_mode(false);
    system_set_state(proc ? SYSTEM_STATE_PROCESS_RUNNING : prev_state);

    serial_printf("[syscall_read] completed: pid=%u bytes_read=%u text=\"%s\"\n",
                  pid, len, buffer);

    return len;
}

static ipo_uarg_t syscall_builtin_async_start(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *task_name =
        (const char *)(uintptr_t)argv[0];

    uint32_t interval_ms =
        argv[1] ? argv[1] : 10000u;

    void (*task_fn)(void) =
        (void (*)(void))(uintptr_t)argv[2];

    if (task_fn == NULL) {
        serial_printf(
            "[syscall] async start rejected: null fn for '%s'\n",
            task_name ? task_name : "(null)");

        return IPO_SYSCALL_ENOSYS;
    }

    int result =
        async_start_task(task_name, interval_ms, task_fn);

    if (result < 0) {
        serial_printf(
            "[syscall] async start failed for '%s'\n",
            task_name ? task_name : "(null)");

        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)result;
}

static ipo_uarg_t syscall_builtin_async_stop(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *task_name =
        (const char *)(uintptr_t)argv[0];

    return (uint32_t)async_stop_task(task_name);
}

static ipo_uarg_t syscall_builtin_var_set(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *name = (const char *)(uintptr_t)argv[0];
    const void *value = (const void *)(uintptr_t)argv[1];
    uint32_t value_len = argv[2];

    if (name == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    if (set_shared_var(name, value, value_len) < 0) {
        serial_printf("[syscall] var set failed for '%s'\n", name);
        return IPO_SYSCALL_ENOSYS;
    }

    serial_printf("[syscall] var set: name='%s' bytes=%u\n", name, value_len);
    return IPO_SYSCALL_OK;
}

static ipo_uarg_t syscall_builtin_var_get(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *name = (const char *)(uintptr_t)argv[0];
    void *out_buffer = (void *)(uintptr_t)argv[1];
    uint32_t out_capacity = argv[2];

    if (name == NULL || out_buffer == NULL || out_capacity == 0u) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t out_len = 0u;
    int copied = get_shared_var(name, out_buffer, out_capacity, &out_len);
    if (copied < 0) {
        serial_printf("[syscall] var get failed for '%s'\n", name);
        return IPO_SYSCALL_ENOSYS;
    }

    serial_printf("[syscall] var get: name='%s' copied=%d total=%u\n",
                 name, copied, out_len);
    return copied;
}

static ipo_uarg_t syscall_builtin_var_delete(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *name = (const char *)(uintptr_t)argv[0];
    if (name == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    if (delete_shared_var(name) < 0) {
        serial_printf("[syscall] var delete failed for '%s'\n", name);
        return IPO_SYSCALL_ENOSYS;
    }

    serial_printf("[syscall] var delete: name='%s'\n", name);
    return IPO_SYSCALL_OK;
}

static ipo_uarg_t syscall_builtin_stack_grow(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    process_t *proc = process_get_current();
    if (proc == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t before = proc->stack_size;
    int32_t delta = (int32_t)argv[0];
    int result = process_adjust_stack_size(proc, delta);
    if (result < 0) {
        serial_printf("[syscall] stack grow failed: before=%u delta=%d\n",
                     before, delta);
        return IPO_SYSCALL_ENOSYS;
    }

    serial_printf("[syscall] stack grow: before=%u added=%d now=%u\n",
                 before, delta, (uint32_t)result);
    return (uint32_t)result;
}

static ipo_uarg_t syscall_builtin_stack_shrink(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    process_t *proc = process_get_current();
    if (proc == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t before = proc->stack_size;
    uint32_t shrink = argv[0];
    if (shrink > 0x7FFFFFFFu) {
        return IPO_SYSCALL_ENOSYS;
    }
    int32_t delta = -(int32_t)shrink;
    int result = process_adjust_stack_size(proc, delta);
    if (result < 0) {
        serial_printf("[syscall] stack shrink failed: before=%u removed=%u\n",
                     before, shrink);
        return IPO_SYSCALL_ENOSYS;
    }

    serial_printf("[syscall] stack shrink: before=%u removed=%u now=%u\n",
                 before, shrink, (uint32_t)result);
    return (uint32_t)result;
}

static ipo_vaddr_t user_heap_break = USER_HEAP_START;

static ipo_uarg_t syscall_builtin_sbrk(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    ipo_sarg_t inc = 0;
    if (argc >= 1u && argv != NULL) {
        inc = (ipo_sarg_t)argv[0];
    }
    process_t *proc = process_get_current();

    if (proc && proc->user_heap_base != 0) {
        if (proc->heap_chunk_count == 0) {
            if (!process_heap_ensure_capacity(proc, 1)) {
                return (ipo_uarg_t)(ipo_sarg_t)-1;
            }
            proc->heap_chunk_count = 1;
            proc->heap_chunks[0].base = proc->user_heap_base;
            proc->heap_chunks[0].limit = proc->user_heap_limit;
            proc->heap_chunks[0].brk = proc->user_heap_break;
        }
        if (!proc->heap_chunks) {
            return (ipo_uarg_t)(ipo_sarg_t)-1;
        }
        uint32_t active_idx = proc->heap_chunk_count - 1;
        ipo_vaddr_t prev = proc->heap_chunks[active_idx].brk;
        if (inc == 0) {
            return (ipo_uarg_t)prev;
        }
        if (inc > 0) {
            ipo_vaddr_t current_brk = proc->heap_chunks[active_idx].brk;
            ipo_vaddr_t current_limit = proc->heap_chunks[active_idx].limit;
            ipo_size_t inc_sz = (ipo_size_t)inc;
            ipo_vaddr_t needed = 0u;
            if (!ipo_vaddr_add_ok(current_brk, inc_sz, &needed) ||
                needed > (ipo_vaddr_t)0xB0000000ULL) {
                return (ipo_uarg_t)(ipo_sarg_t)-1;
            }

            bool can_expand = false;
            ipo_vaddr_t aligned_limit = 0;
            if (needed <= current_limit) {
                can_expand = true;
                aligned_limit = current_limit;
            } else {
                ipo_vaddr_t aligned = 0u;
                if (ipo_vaddr_align_up_ok(needed, 4096u, &aligned) &&
                    aligned <= (ipo_vaddr_t)0xB0000000ULL) {
                    aligned_limit = aligned;
                    can_expand = process_heap_chunk_can_expand(proc, active_idx, aligned_limit);
                }
            }

            if (can_expand) {
                if (aligned_limit > current_limit) {
                    ipo_vaddr_t mapped_start = ipo_vaddr_align_down(current_limit, 4096u);
                    ipo_vaddr_t mapped_end = aligned_limit;
                    if (mapped_end > mapped_start &&
                        !process_update_memory_mappings(
                            proc, mapped_start,
                            (ipo_size_t)(mapped_end - mapped_start),
                            true)) {
                        return (ipo_uarg_t)(ipo_sarg_t)-1;
                    }
                    proc->heap_chunks[active_idx].limit = aligned_limit;
                    proc->user_heap_limit = aligned_limit;
                    serial_printf("[process] Dynamic on-demand heap expanded for pid=%u chunk %u: limit=0x%llx (+%llu KiB)\n",
                                  proc->pid, active_idx,
                                  (unsigned long long)aligned_limit,
                                  (unsigned long long)(inc_sz / 1024u));
                }
                if (inc_sz <= 65536u) {
                    memset((void *)(uintptr_t)current_brk, 0, inc_sz);
                } else {
                    memset((void *)(uintptr_t)current_brk, 0, 4096u);
                }
                proc->heap_chunks[active_idx].brk = needed;
                proc->user_heap_break = needed;
                return (ipo_uarg_t)current_brk;
            } else {
                /* Current heap chunk cannot expand here; allocate a new chunk elsewhere */
                ipo_vaddr_t new_chunk_base = process_heap_allocate_new_chunk(proc, inc_sz);
                if (new_chunk_base == 0) {
                    serial_printf("[process] Out of physical RAM for pid=%u heap: failed to allocate %llu KiB\n",
                                  proc->pid, (unsigned long long)(inc_sz / 1024u));
                    return (ipo_uarg_t)(ipo_sarg_t)-1;
                }
                if (inc_sz <= 65536u) {
                    memset((void *)(uintptr_t)new_chunk_base, 0, inc_sz);
                } else {
                    memset((void *)(uintptr_t)new_chunk_base, 0, 4096u);
                }
                return (ipo_uarg_t)new_chunk_base;
            }
        } else {
            ipo_size_t dec = (ipo_size_t)(0 - inc);
            ipo_vaddr_t current_brk = proc->heap_chunks[active_idx].brk;
            ipo_vaddr_t current_base = proc->heap_chunks[active_idx].base;

            if (dec > (ipo_size_t)(current_brk - current_base)) {
                return (ipo_uarg_t)(ipo_sarg_t)-1;
            }
            ipo_vaddr_t new_break = current_brk - (ipo_vaddr_t)dec;
            if (new_break == current_base && proc->heap_chunk_count > 1) {
                ipo_vaddr_t mapped_start = ipo_vaddr_align_down(current_base, 4096u);
                ipo_vaddr_t mapped_end = 0u;
                if (ipo_vaddr_align_up_ok(proc->heap_chunks[active_idx].limit, 4096u, &mapped_end) &&
                    mapped_end > mapped_start) {
                    process_update_memory_mappings(proc, mapped_start,
                                                   (ipo_size_t)(mapped_end - mapped_start), false);
                }
                proc->heap_chunk_count--;
                active_idx = proc->heap_chunk_count - 1;
                proc->user_heap_break = proc->heap_chunks[active_idx].brk;
                proc->user_heap_limit = proc->heap_chunks[active_idx].limit;
                return (ipo_uarg_t)current_brk;
            }
            ipo_vaddr_t new_limit = 0u;
            if (!ipo_vaddr_align_up_ok(new_break, 4096u, &new_limit) ||
                new_limit > (ipo_vaddr_t)0xB0000000ULL) {
                return (ipo_uarg_t)(ipo_sarg_t)-1;
            }
            if (new_limit < proc->heap_chunks[active_idx].limit) {
                ipo_vaddr_t mapped_start = ipo_vaddr_align_down(new_limit, 4096u);
                ipo_vaddr_t mapped_end = 0u;
                if (!ipo_vaddr_align_up_ok(proc->heap_chunks[active_idx].limit, 4096u, &mapped_end) ||
                    (mapped_end > mapped_start &&
                     !process_update_memory_mappings(
                         proc, mapped_start,
                         (ipo_size_t)(mapped_end - mapped_start),
                         false))) {
                    return (ipo_uarg_t)(ipo_sarg_t)-1;
                }
                proc->heap_chunks[active_idx].limit = new_limit;
                proc->user_heap_limit = new_limit;
            }
            proc->heap_chunks[active_idx].brk = new_break;
            proc->user_heap_break = new_break;
            return (ipo_uarg_t)current_brk;
        }
    }

    /* Fallback for system / non-process context */
    ipo_vaddr_t prev = user_heap_break;
    if (inc == 0) {
        return (ipo_uarg_t)prev;
    }
    if (inc > 0) {
        ipo_size_t inc_sz = (ipo_size_t)inc;
        uint64_t total_ram = memory_map_get_total_ram();
        uint64_t limit_ram = total_ram > 0 ? total_ram : 0xB0000000ULL;
        if ((uint64_t)user_heap_break + (uint64_t)inc_sz > limit_ram) {
            return (ipo_uarg_t)(ipo_sarg_t)-1;
        }
        memset((void *)(uintptr_t)prev, 0, inc_sz);
        user_heap_break += (ipo_vaddr_t)inc_sz;
        return (ipo_uarg_t)prev;
    } else {
        ipo_size_t dec = (ipo_size_t)(0 - inc);
        if (dec > (ipo_size_t)(user_heap_break - (ipo_vaddr_t)USER_HEAP_START)) {
            return (ipo_uarg_t)(ipo_sarg_t)-1;
        }
        user_heap_break -= (ipo_vaddr_t)dec;
        return (ipo_uarg_t)prev;
    }
}

static ipo_uarg_t syscall_builtin_getpid(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    process_t *proc = process_get_current();
    return proc ? proc->pid : 1;
}

static ipo_uarg_t syscall_builtin_time(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    timer_update();
    uint32_t secs = timer_seconds();
    if (argc >= 1u && argv != NULL && argv[0] != 0u) {
        uint32_t *out = (uint32_t *)(uintptr_t)argv[0];
        *out = secs;
    }
    return secs;
}

static ipo_uarg_t syscall_builtin_millis(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    timer_update();
    return timer_millis();
}

static ipo_uarg_t syscall_builtin_elapsed_ms(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) {
        return 0u;
    }
    timer_update();
    return timer_elapsed_ms(argv[0]);
}

static ipo_uarg_t syscall_builtin_timer_after(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || argv == NULL) {
        return 0u;
    }
    timer_update();
    return (uint32_t)timer_after(argv[0], argv[1]);
}

static ipo_uarg_t syscall_rtc_copy_alloc_str(char *str, rtc_status_t err,
                                             char *out_buf, ipo_uarg_t cap) {
    if (!str) {
        return (err < 0) ? (ipo_uarg_t)(ipo_sarg_t)err : (ipo_uarg_t)(ipo_sarg_t)-1;
    }
    size_t len = strlen(str);
    if (!out_buf && cap == 0u) {
        kfree(str);
        return (ipo_uarg_t)len;
    }
    if (!out_buf || (size_t)cap < len + 1u) {
        if (out_buf && cap > 0u) out_buf[0] = '\0';
        kfree(str);
        return (ipo_uarg_t)(ipo_sarg_t)RTC_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(out_buf, str, len + 1u);
    kfree(str);
    return (ipo_uarg_t)len;
}

static ipo_uarg_t syscall_builtin_rtc_get_str(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 3u || !argv) return (ipo_uarg_t)(ipo_sarg_t)-1;
    const char *fmt = (const char *)(uintptr_t)argv[0];
    if (fmt && !syscall_user_str_ok(fmt)) return (ipo_uarg_t)(ipo_sarg_t)-1;
    char *out_buf = (char *)(uintptr_t)argv[1];
    ipo_uarg_t cap = argv[2];
    if (out_buf && cap > 0u && !syscall_user_buf_ok(out_buf, (ipo_size_t)cap)) {
        return (ipo_uarg_t)(ipo_sarg_t)-1;
    }
    rtc_status_t err = RTC_OK;
    char *s = rtc_format_alloc(fmt, NULL, &err);
    return syscall_rtc_copy_alloc_str(s, err, out_buf, cap);
}

static ipo_uarg_t syscall_builtin_rtc_set_str(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return (ipo_uarg_t)(ipo_sarg_t)-1;
    const char *spec = (const char *)(uintptr_t)argv[0];
    if (!syscall_user_str_ok(spec)) return (ipo_uarg_t)(ipo_sarg_t)-1;
    uint32_t kind = (argc >= 2u) ? (uint32_t)argv[1] : RTC_SET_AUTO;
    rtc_status_t err = RTC_OK;
    return rtc_set_spec(spec, kind, &err) ? 0u : (ipo_uarg_t)(ipo_sarg_t)err;
}

static ipo_uarg_t syscall_builtin_rtc_get_format(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv) return (ipo_uarg_t)(ipo_sarg_t)-1;
    char *out_buf = (char *)(uintptr_t)argv[0];
    ipo_uarg_t cap = argv[1];
    if (out_buf && cap > 0u && !syscall_user_buf_ok(out_buf, (ipo_size_t)cap)) {
        return (ipo_uarg_t)(ipo_sarg_t)-1;
    }
    return syscall_rtc_copy_alloc_str(rtc_get_time_format_alloc(), RTC_ERR_NOMEM, out_buf, cap);
}

static ipo_uarg_t syscall_builtin_rtc_set_format(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return (ipo_uarg_t)(ipo_sarg_t)-1;
    const char *fmt = (const char *)(uintptr_t)argv[0];
    if (!syscall_user_str_ok(fmt)) return (ipo_uarg_t)(ipo_sarg_t)-1;
    return rtc_set_time_format(fmt) ? 0u : (ipo_uarg_t)(ipo_sarg_t)RTC_ERR_INVALID_FORMAT;
}

static ipo_uarg_t syscall_builtin_rtc_add_str(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[0] || !argv[1]) return (ipo_uarg_t)(ipo_sarg_t)-1;
    const char *unit = (const char *)(uintptr_t)argv[0];
    const char *amt  = (const char *)(uintptr_t)argv[1];
    if (!syscall_user_str_ok(unit) || !syscall_user_str_ok(amt)) {
        return (ipo_uarg_t)(ipo_sarg_t)-1;
    }
    rtc_status_t err = RTC_OK;
    return rtc_add_unit_dec(unit, amt, &err) ? 0u : (ipo_uarg_t)(ipo_sarg_t)err;
}

static ipo_uarg_t syscall_builtin_rtc_get_date_format(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv) return (ipo_uarg_t)(ipo_sarg_t)-1;
    char *out_buf = (char *)(uintptr_t)argv[0];
    ipo_uarg_t cap = argv[1];
    if (out_buf && cap > 0u && !syscall_user_buf_ok(out_buf, (ipo_size_t)cap)) {
        return (ipo_uarg_t)(ipo_sarg_t)-1;
    }
    return syscall_rtc_copy_alloc_str(rtc_get_date_format_alloc(), RTC_ERR_NOMEM, out_buf, cap);
}

static ipo_uarg_t syscall_builtin_rtc_set_date_format(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return (ipo_uarg_t)(ipo_sarg_t)-1;
    const char *fmt = (const char *)(uintptr_t)argv[0];
    if (!syscall_user_str_ok(fmt)) return (ipo_uarg_t)(ipo_sarg_t)-1;
    return rtc_set_date_format(fmt) ? 0u : (ipo_uarg_t)(ipo_sarg_t)RTC_ERR_INVALID_FORMAT;
}

static ipo_uarg_t syscall_builtin_rtc_get_date_str(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv) return (ipo_uarg_t)(ipo_sarg_t)-1;
    char *out_buf = (char *)(uintptr_t)argv[0];
    ipo_uarg_t cap = argv[1];
    if (out_buf && cap > 0u && !syscall_user_buf_ok(out_buf, (ipo_size_t)cap)) {
        return (ipo_uarg_t)(ipo_sarg_t)-1;
    }
    rtc_status_t err = RTC_OK;
    char *s = rtc_format_date_alloc(NULL, &err);
    return syscall_rtc_copy_alloc_str(s, err, out_buf, cap);
}

static ipo_uarg_t syscall_builtin_rtc_get_status(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv) return (ipo_uarg_t)(ipo_sarg_t)-1;
    char *out_buf = (char *)(uintptr_t)argv[0];
    ipo_uarg_t cap = argv[1];
    if (out_buf && cap > 0u && !syscall_user_buf_ok(out_buf, (ipo_size_t)cap)) {
        return (ipo_uarg_t)(ipo_sarg_t)-1;
    }
    rtc_info_t info;
    rtc_get_info(&info);
    char msg[128];
    snprintf(msg, sizeof(msg), "hw=%s irq8=%s subsec=%s err=%d",
             info.hw_ready ? "ready" : "err",
             info.irq8_active ? "active" : "inactive",
             info.subsecond_available ? "pie1024" : "1s",
             (int)info.last_error);
    size_t mlen = strlen(msg);
    char *copy = (char *)kmalloc(mlen + 1u);
    if (copy) memcpy(copy, msg, mlen + 1u);
    return syscall_rtc_copy_alloc_str(copy, RTC_ERR_NOMEM, out_buf, cap);
}

static ipo_uarg_t syscall_builtin_free(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc >= 1u && argv != NULL && argv[0] != 0u) {
        kfree((void *)(uintptr_t)argv[0]);
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_exit(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    int code = 0;
    if (argc > 0 && argv != NULL) {
        code = (int)argv[0];
    }
    sound_stop();
    process_t *proc = process_get_current();
    if (proc != NULL) {
        wm_destroy_windows_for_pid(proc->pid);
        proc->is_running = 0;
        proc->exit_code = code;
        if (!process_get_separate_windows() ||
            (proc->is_wm_app && !wm_has_windows_for_pid(proc->pid)) ||
            (proc->wants_graphics && !proc->is_wm_app)) {
            proc->completed_and_acknowledged = true;
        }
        if (!proc->return_val_explicit) {
            process_set_current_return_value(PROC_RET_INT, "int", NULL, code, NULL, 0);
        }
    }
    process_set_last_exit_code(code);
    process_yield_kernel();
    while (1) {
        process_yield_kernel();
    }
    return 0;
}

void syscall_reset_user_heap(void) {
    user_heap_break = USER_HEAP_START;
}

static ipo_uarg_t syscall_builtin_wm_create_window(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc;
    if (!argv || !argv[0]) return 0;
    const wm_window_options_t *opts = (const wm_window_options_t *)(uintptr_t)argv[0];
    return (uint32_t)(uintptr_t)wm_create_window(opts);
}

static ipo_uarg_t syscall_builtin_wm_destroy_window(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc;
    if (!argv || !argv[0]) return 0;
    wm_destroy_window((wm_window_t *)(uintptr_t)argv[0]);
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_session_start(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    wm_session_start();
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_session_stop(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    wm_session_stop();
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_session_active(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)wm_session_active();
}

static ipo_uarg_t syscall_builtin_wm_get_count(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)wm_get_window_count();
}

static ipo_uarg_t syscall_builtin_wm_get_focused(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)(uintptr_t)wm_get_focused();
}

static ipo_uarg_t syscall_builtin_wm_get_list(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (ipo_uarg_t)(uintptr_t)wm_get_window_list();
}

static ipo_uarg_t syscall_builtin_wm_set_focus(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc;
    if (!argv || !argv[0]) return 0;
    wm_set_focus((wm_window_t *)(uintptr_t)argv[0]);
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_focus_next(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    wm_focus_next();
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_focus_prev(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    wm_focus_prev();
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_invalidate(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc;
    if (!argv || !argv[0]) return 0;
    wm_invalidate((wm_window_t *)(uintptr_t)argv[0]);
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_is_valid(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc;
    if (!argv || !argv[0]) return 0;
    return wm_is_window_valid((wm_window_t *)(uintptr_t)argv[0]) ? 1u : 0u;
}

static ipo_uarg_t syscall_builtin_wm_toggle_maximize(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc;
    if (!argv || !argv[0]) return 0;
    wm_toggle_maximize((wm_window_t *)(uintptr_t)argv[0]);
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_resize(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc;
    if (!argv || argc < 3 || !argv[0]) return 0;
    return wm_resize((wm_window_t *)(uintptr_t)argv[0], (uint16_t)argv[1], (uint16_t)argv[2]) ? 1u : 0u;
}

static wm_window_t *syscall_wm_window_arg(ipo_uarg_t argc, ipo_uarg_t *argv) {
    if (argc < 1u || !argv || !argv[0]) return NULL;
    wm_window_t *win = (wm_window_t *)(uintptr_t)argv[0];
    return wm_is_window_valid(win) ? win : NULL;
}

static ipo_uarg_t syscall_builtin_wm_init(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    if (wm_get_window_count() == 0) wm_init();
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_invalidate_all(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    wm_invalidate_all();
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_set_fullscreen(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    wm_window_t *win = syscall_wm_window_arg(argc, argv);
    if (win && argc >= 2u) wm_set_fullscreen(win, argv[1] != 0u);
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_minimize(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    wm_window_t *win = syscall_wm_window_arg(argc, argv);
    if (win) wm_minimize(win);
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_restore(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    wm_window_t *win = syscall_wm_window_arg(argc, argv);
    if (win) wm_restore(win);
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_move(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    wm_window_t *win = syscall_wm_window_arg(argc, argv);
    if (win && argc >= 3u) {
        wm_move(win, (int16_t)argv[1], (int16_t)argv[2]);
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_set_title(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    wm_window_t *win = syscall_wm_window_arg(argc, argv);
    if (win && argc >= 2u) {
        wm_set_title(win, (const char *)(uintptr_t)argv[1]);
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_set_shape(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    wm_window_t *win = syscall_wm_window_arg(argc, argv);
    if (win && argc >= 4u) {
        wm_set_shape(win, (const uint8_t *)(uintptr_t)argv[1],
                     (uint16_t)argv[2], (uint16_t)argv[3]);
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_poll_event(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv ||
        (process_in_process_context() && !ipc_user_range(argv, 2u * sizeof(uint32_t)))) {
        return 0u;
    }
    wm_window_t *win = (wm_window_t *)(uintptr_t)argv[0];
    wm_event_t *out = (wm_event_t *)(uintptr_t)argv[1];
    if (!out ||
        (process_in_process_context() && !ipc_user_range(out, sizeof(wm_event_t)))) {
        return 0u;
    }
    return wm_poll_event(win, out) ? 1u : 0u;
}

static ipo_uarg_t syscall_builtin_wm_compositor_tick(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    wm_compositor_tick();
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_get_count_for_pid(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return 0;
    return (uint32_t)wm_get_window_count_for_pid(argv[0]);
}

static ipo_uarg_t syscall_builtin_wm_has_windows_for_pid(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return 0;
    return wm_has_windows_for_pid(argv[0]) ? 1u : 0u;
}

static ipo_uarg_t syscall_builtin_wm_focus_window_for_pid(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc >= 1u && argv) wm_focus_window_for_pid(argv[0]);
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_set_cursor_visible(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    wm_window_t *win = syscall_wm_window_arg(argc, argv);
    if (win && argc >= 2u) {
        wm_set_cursor_visible(win, argv[1] != 0u);
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_wm_get_cursor_visible(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    wm_window_t *win = syscall_wm_window_arg(argc, argv);
    return wm_get_cursor_visible(win) ? 1u : 0u;
}

static ipo_uarg_t syscall_builtin_process_yield(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    process_t *cur = process_get_current();
    if (cur && argc > 0 && argv) {
        cur->waiting_for_input = (argv[0] != 0);
    }
    process_yield_kernel();
    return 0;
}

static ipo_uarg_t syscall_builtin_process_is_foreground(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    if (!process_in_process_context()) {
        return 1;
    }
    process_t *cur = process_get_current();
    process_t *fg = process_get_foreground();
    if (!fg || !cur) return 1;
    bool is_fg = (cur == fg);
    return is_fg ? 1 : 0;
}

static ipo_uarg_t syscall_builtin_process_is_batch_active(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    return process_is_batch_active() ? 1 : 0;
}

static ipo_uarg_t syscall_builtin_process_switch_fg(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (!process_is_batch_active()) return 0;
    if (argc > 0 && argv) {
        if (argv[0] == 1) {
            process_switch_foreground_prev();
        } else if (argv[0] >= 10) {
            process_switch_foreground_to_workspace((int)(argv[0] - 10));
        } else {
            process_switch_foreground_next();
        }
    } else {
        process_switch_foreground_next();
    }
    return 0;
}

static bool syscall_cur_owns_foreground_vga(process_t *cur);

static ipo_uarg_t syscall_builtin_vga_set_mode(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc == 0 || !argv) return 0;
    if (argv[0] == 2 && argc >= 2 && argv[1] != 0) {
        vga_gfx_flip((const uint8_t *)(uintptr_t)argv[1]);
        return 0;
    }
    if (argv[0] == 3 && argc >= 2 && argv[1] != 0) {
        if (argc >= 4u) {
            vga_gfx_flip_scaled_buffer((const uint8_t *)(uintptr_t)argv[1],
                                       (int)argv[2],
                                       (int)argv[3]);
        } else {
            vga_gfx_flip_desktop((const uint8_t *)(uintptr_t)argv[1]);
        }
        return 0;
    }
    if (argv[0] == 4 && argc >= 3) {
        return vga_gfx_set_resolution((int)argv[1], (int)argv[2]) ? 1u : 0u;
    }
    process_t *cur = process_get_current();
    if (argv[0] == 1) {
        if (cur) cur->wants_graphics = true;
        if (cur && process_is_batch_active() && process_get_separate_windows()) {
            process_t *fg = process_get_foreground();
            if (!fg || (!fg->is_wm_app && !fg->wants_graphics)) {
                process_set_foreground(cur);
            }
        }
        if (syscall_cur_owns_foreground_vga(cur) && !vga_is_graphics_mode()) {
            vga_set_mode_13h_hardware();
        }
    } else {
        if (cur) {
            cur->wants_graphics = false;
        }
        if (syscall_cur_owns_foreground_vga(cur) && !process_wm_has_focus()) {
            if (vga_is_graphics_mode()) {
                vga_set_mode_text_hardware();
            }
        }
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_vga_get_mode(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    return vga_is_graphics_mode() ? 1 : 0;
}

static ipo_uarg_t syscall_builtin_vga_get_bounds(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[0] || !argv[1]) {
        return IPO_SYSCALL_ENOSYS;
    }

    int32_t *w_ptr = (int32_t *)(uintptr_t)argv[0];
    int32_t *h_ptr = (int32_t *)(uintptr_t)argv[1];
    if (process_in_process_context() &&
        (!ipc_user_range(argv, 2u * sizeof(uint32_t)) ||
         !ipc_user_range(w_ptr, sizeof(int32_t)) ||
         !ipc_user_range(h_ptr, sizeof(int32_t)))) {
        return IPO_SYSCALL_ENOSYS;
    }

    int width;
    int height;
    vga_gfx_get_screen_bounds(&width, &height);
    *w_ptr = width;
    *h_ptr = height;
    return 0;
}

static ipo_uarg_t syscall_builtin_system_interrupt(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc > 0 && argv) {
        if (argv[0] == 1u) {
            system_request_interrupt();
            serial_printf("[syscall] system_interrupt requested\n");
            return 1u;
        }
        if (argv[0] == 0u) {
            system_clear_interrupt();
            return 0u;
        }
    }
    return system_is_interrupted() ? 1u : 0u;
}

static ipo_uarg_t syscall_builtin_kbd_get_scancode(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)keyboard_get_scancode();
}

static ipo_uarg_t syscall_builtin_kbd_wait_scancode(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)keyboard_wait_scancode();
}

static ipo_uarg_t syscall_builtin_kbd_set_app_mode(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (process_is_batch_active() && process_in_process_context()) {
        process_t *cur = process_get_current();
        process_t *fg = process_get_foreground();
        if (cur && fg && cur != fg) return 0;
    }
    if (argc > 0 && argv) {
        keyboard_set_app_input_mode(argv[0] != 0);
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_kbd_flush(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    if (process_is_batch_active() && process_in_process_context()) {
        process_t *cur = process_get_current();
        process_t *fg = process_get_foreground();
        if (cur && fg && cur != fg) return 0;
    }
    keyboard_flush_queue();
    keyboard_flush_app_queue();
    keyboard_flush_hardware();
    keyboard_clear_key_state();
    return 0;
}

static ipo_uarg_t syscall_builtin_kbd_get_modifiers(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)keyboard_get_modifiers();
}

static ipo_uarg_t syscall_builtin_kbd_poll(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    keyboard_poll();
    return 0;
}

static ipo_uarg_t syscall_builtin_kbd_get_key_string(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return 0;
    uint8_t scancode = (uint8_t)argv[0];
    const char *s = keyboard_get_key_string(scancode);
    if (!s) return 0;
    if (argc >= 3u && argv[1] != 0 && argv[2] > 0) {
        char *user_buf = (char *)(uintptr_t)argv[1];
        uint32_t max_len = argv[2];
        if (!syscall_user_buf_ok(user_buf, max_len)) {
            return 0;
        }
        strncpy(user_buf, s, max_len - 1);
        user_buf[max_len - 1] = '\0';
        return 1;
    }
    if (process_in_process_context()) {
        return 0;
    }
    return (uint32_t)(uintptr_t)s;
}

static ipo_uarg_t syscall_builtin_mouse_get_state(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc > 0 && argv && argv[0]) {
        mouse_state_t *out = (mouse_state_t *)(uintptr_t)argv[0];
        if (!syscall_user_buf_ok(out, sizeof(*out))) {
            return IPO_SYSCALL_ENOSYS;
        }
        mouse_get_state(out);
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_mouse_get_motion(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[0] || !argv[1]) {
        return IPO_SYSCALL_ENOSYS;
    }
    int64_t *dx = (int64_t *)(uintptr_t)argv[0];
    int64_t *dy = (int64_t *)(uintptr_t)argv[1];
    if (!syscall_user_buf_ok(dx, sizeof(*dx)) || !syscall_user_buf_ok(dy, sizeof(*dy))) {
        return IPO_SYSCALL_ENOSYS;
    }
    mouse_get_cumulative_motion(dx, dy);
    return 0;
}

static bool syscall_cur_owns_foreground_vga(process_t *cur) {
    if (!cur || !process_is_batch_active() || !process_get_separate_windows()) {
        return true;
    }
    process_t *fg = process_get_displayed_foreground();
    return (!fg || fg->window_id == cur->window_id);
}

static ipo_uarg_t syscall_builtin_vga_set_cursor(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc > 0 && argv) {
        uint16_t pos = (uint16_t)argv[0];
        process_t *cur = process_get_current();
        if (cur) {
            cur->text_cursor_pos = pos;
            process_sync_workspace_text_state(cur);
        }
        if (syscall_cur_owns_foreground_vga(cur)) {
            vga_set_cursor(pos);
        }
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_vga_get_cursor(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    process_t *cur = process_get_current();
    if (cur && !syscall_cur_owns_foreground_vga(cur)) {
        return (uint32_t)cur->text_cursor_pos;
    }
    return (uint32_t)vga_get_cursor_position();
}

static ipo_uarg_t syscall_builtin_vga_show_cursor(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    process_t *cur = process_get_current();
    if (cur) {
        cur->text_cursor_visible = true;
        process_sync_workspace_text_state(cur);
    }
    if (syscall_cur_owns_foreground_vga(cur)) {
        vga_show_cursor();
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_vga_hide_cursor(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    process_t *cur = process_get_current();
    if (cur) {
        cur->text_cursor_visible = false;
        process_sync_workspace_text_state(cur);
    }
    if (syscall_cur_owns_foreground_vga(cur)) {
        vga_hide_cursor();
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_vga_set_palette(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc >= 4 && argv) {
        vga_gfx_set_palette((uint8_t)argv[0], (uint8_t)argv[1], (uint8_t)argv[2], (uint8_t)argv[3]);
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_vga_set_overlay_flags(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    process_t *cur = process_get_current();
    if (cur && argc > 0 && argv) {
        cur->gfx_overlay_flags = argv[0];
        if (wm_session_active()) {
            wm_invalidate_all();
        }
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_vga_get_overlay_flags(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    process_t *cur = process_get_current();
    return cur ? cur->gfx_overlay_flags : 0;
}

static ipo_uarg_t syscall_builtin_vga_get_text_info(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 3u || !argv) return IPO_SYSCALL_ENOSYS;
    uint16_t *w_ptr = (uint16_t *)(uintptr_t)argv[0];
    uint16_t *h_ptr = (uint16_t *)(uintptr_t)argv[1];
    uint32_t *vram_ptr = (uint32_t *)(uintptr_t)argv[2];
    if (process_in_process_context() &&
        (!ipc_user_range(argv, 3u * sizeof(uint32_t)) ||
         (w_ptr && !ipc_user_range(w_ptr, sizeof(uint16_t))) ||
         (h_ptr && !ipc_user_range(h_ptr, sizeof(uint16_t))) ||
         (vram_ptr && !ipc_user_range(vram_ptr, sizeof(uint32_t))))) {
        return IPO_SYSCALL_ENOSYS;
    }
    vga_get_text_mode_info(w_ptr, h_ptr, vram_ptr);
    return 0;
}

static ipo_uarg_t syscall_builtin_audio_init(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    return audio_init() ? 1u : 0u;
}

static ipo_uarg_t syscall_builtin_audio_tick(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    audio_tick();
    return 0;
}

static ipo_uarg_t syscall_builtin_audio_stop_all(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    if (process_is_batch_active()) {
        process_t *cur = process_get_current();
        if (cur) {
            audio_cleanup_streams_for_owner(cur);
        }
        return 0;
    }
    audio_stop_all();
    sb16_stop();
    return 0;
}

static ipo_uarg_t syscall_builtin_audio_stream_create(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc >= 5 && argv) {
        return (ipo_uarg_t)(uintptr_t)audio_stream_create((int)argv[0], (int)argv[1], (uint32_t)argv[2], (const void *)(uintptr_t)argv[3], (size_t)argv[4]);
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_audio_stream_create_ring(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc >= 6 && argv) {
        return (ipo_uarg_t)(uintptr_t)audio_stream_create_ring((int)argv[0], (int)argv[1], (uint32_t)argv[2],
                                                               (const void *)(uintptr_t)argv[3], (size_t)argv[4], (size_t)argv[5]);
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_audio_stream_destroy(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc >= 1 && argv && argv[0]) {
        audio_stream_destroy((audio_stream_t *)(uintptr_t)argv[0]);
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_audio_stream_play(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc >= 1 && argv && argv[0]) {
        audio_stream_play((audio_stream_t *)(uintptr_t)argv[0]);
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_audio_stream_pause(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc >= 1 && argv && argv[0]) {
        audio_stream_pause((audio_stream_t *)(uintptr_t)argv[0]);
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_audio_stream_stop(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc >= 1 && argv && argv[0]) {
        audio_stream_stop((audio_stream_t *)(uintptr_t)argv[0]);
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_audio_stream_seek(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc >= 2 && argv && argv[0]) {
        double frac = *(const double *)(uintptr_t)argv[1];
        audio_stream_seek((audio_stream_t *)(uintptr_t)argv[0], frac);
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_audio_stream_set_vol(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc >= 2 && argv && argv[0]) {
        float vol = *(const float *)(uintptr_t)argv[1];
        audio_stream_set_volume((audio_stream_t *)(uintptr_t)argv[0], vol);
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_audio_stream_progress(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc >= 2 && argv && argv[0] && argv[1]) {
        double prog = audio_stream_get_progress((audio_stream_t *)(uintptr_t)argv[0]);
        *(double *)(uintptr_t)argv[1] = prog;
    }
    return 0;
}

static ipo_uarg_t syscall_builtin_audio_stream_update(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc >= 3 && argv && argv[0]) {
        audio_stream_update_buffered((audio_stream_t *)(uintptr_t)argv[0], (size_t)argv[1], (bool)(argv[2] != 0));
    }
    return 0;
}

static uint32_t syscall_caller_pid(void) {
    process_t *p = process_get_current();
    return p ? p->pid : 0u;
}

static ipo_uarg_t syscall_builtin_input_poll_key_ext(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return (uint32_t)-1;
    ipo_key_event_t *out_ev = (ipo_key_event_t *)(uintptr_t)argv[0];
    if (!syscall_user_buf_ok(out_ev, sizeof(*out_ev))) return (uint32_t)-1;
    if (out_ev->raw_bytes && out_ev->raw_cap > 0u &&
        !syscall_user_buf_ok(out_ev->raw_bytes, out_ev->raw_cap)) {
        return (uint32_t)-1;
    }
    return ipo_input_poll_key_event(syscall_caller_pid(), out_ev) ? 1u : 0u;
}

static ipo_uarg_t syscall_builtin_input_get_key_state(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return 0u;
    return ipo_input_is_key_down(argv[0]) ? 1u : 0u;
}

static ipo_uarg_t syscall_builtin_input_get_modifiers(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    return ipo_input_get_modifiers();
}

static ipo_uarg_t syscall_builtin_shortcut_register(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return 0u;
    return ipo_shortcut_register((const ipo_shortcut_spec_t *)(uintptr_t)argv[0], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_shortcut_unregister(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)-1;
    return (uint32_t)ipo_shortcut_unregister(argv[0], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_shortcut_poll(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return (uint32_t)-1;
    return ipo_shortcut_poll(syscall_caller_pid(), (ipo_shortcut_event_t *)(uintptr_t)argv[0]) ? 1u : 0u;
}

static ipo_uarg_t syscall_builtin_input_dev_register(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return 0u;
    const ipo_input_dev_desc_t *desc = (const ipo_input_dev_desc_t *)(uintptr_t)argv[0];
    if (!syscall_user_buf_ok(desc, sizeof(*desc)) || !syscall_user_str_ok(desc->name)) {
        return 0u;
    }
    return ipo_input_dev_register(desc, syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_input_dev_unregister(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)-1;
    return (uint32_t)ipo_input_dev_unregister(argv[0], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_input_dev_emit(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[1]) return (uint32_t)-1;
    return (uint32_t)ipo_input_dev_emit(argv[0], (const ipo_input_event_t *)(uintptr_t)argv[1], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_input_dev_list(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv) return 0u;
    ipo_input_dev_info_t *out_arr = (ipo_input_dev_info_t *)(uintptr_t)argv[0];
    ipo_count_t max_cnt = (ipo_count_t)argv[1];
    if (out_arr && max_cnt > 0u) {
        if (max_cnt > IPO_SIZE_MAX / sizeof(ipo_input_dev_info_t) ||
            !syscall_user_buf_ok(out_arr, (ipo_size_t)max_cnt * sizeof(ipo_input_dev_info_t))) {
            return 0u;
        }
        for (ipo_count_t i = 0u; i < max_cnt; i++) {
            if (out_arr[i].name && out_arr[i].name_cap > 0u &&
                !syscall_user_buf_ok(out_arr[i].name, out_arr[i].name_cap)) {
                return 0u;
            }
        }
    }
    return ipo_input_dev_list(out_arr, (uint32_t)max_cnt);
}

static ipo_uarg_t syscall_builtin_input_dev_poll_event(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[1]) return (uint32_t)-1;
    return ipo_input_dev_poll_event(syscall_caller_pid(), argv[0], (ipo_input_event_t *)(uintptr_t)argv[1]) ? 1u : 0u;
}

static ipo_uarg_t syscall_builtin_input_dev_get_state(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 4u || !argv || !argv[3]) return (uint32_t)-1;
    return (uint32_t)ipo_input_dev_get_state(argv[0], (uint16_t)argv[1], argv[2], (int32_t *)(uintptr_t)argv[3]);
}

static ipo_uarg_t syscall_builtin_mouse_inject(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 5u || !argv) return (uint32_t)-1;
    mouse_inject_event((int32_t)argv[0], (int32_t)argv[1], (int32_t)argv[2], argv[3], argv[4] != 0u);
    return 0u;
}

static ipo_uarg_t syscall_builtin_ps2_hook_register(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv) return 0u;
    return ipo_ps2_hook_register(argv[0], argv[1], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_ps2_hook_unregister(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)-1;
    return (uint32_t)ipo_ps2_hook_unregister(argv[0], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_ps2_hook_poll(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 3u || !argv || !argv[1]) return 0u;
    return ipo_ps2_hook_poll(argv[0], syscall_caller_pid(), (ipo_ps2_byte_event_t *)(uintptr_t)argv[1], argv[2]);
}

static ipo_uarg_t syscall_builtin_ps2_transact(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return (uint32_t)-1;
    return (uint32_t)ipo_ps2_transact((const ipo_ps2_transact_req_t *)(uintptr_t)argv[0]);
}

static ipo_uarg_t syscall_builtin_irq_subscribe(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return 0u;
    return irq_subscribe((uint8_t)argv[0], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_irq_unsubscribe(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)-1;
    return (uint32_t)irq_unsubscribe(argv[0], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_irq_poll(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 3u || !argv) return 0u;
    return irq_poll(argv[0], syscall_caller_pid(), (uint32_t *)(uintptr_t)argv[1], (uint64_t *)(uintptr_t)argv[2]);
}

static ipo_uarg_t syscall_builtin_irq_get_count(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return 0u;
    return irq_get_count((uint8_t)argv[0]);
}

static ipo_uarg_t syscall_builtin_irq_register_cb(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 3u || !argv || !argv[1]) return 0u;
    return irq_register_handler((uint8_t)argv[0], (irq_handler_fn_t)(uintptr_t)argv[1],
                                (void *)(uintptr_t)argv[2], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_irq_unregister_cb(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)-1;
    return (uint32_t)irq_unregister_handler(argv[0], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_hw_map_mmio(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 3u || !argv) return 0u;
    return (ipo_uarg_t)(uintptr_t)ipo_hw_map_mmio((ipo_paddr_t)argv[0], (ipo_size_t)argv[1], (uint32_t)argv[2], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_hw_unmap_mmio(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv) return (ipo_uarg_t)(ipo_sarg_t)-1;
    return (ipo_uarg_t)(ipo_sarg_t)ipo_hw_unmap_mmio((void *)(uintptr_t)argv[0], (ipo_size_t)argv[1], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_hw_alloc_dma(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 3u || !argv) return 0u;
    ipo_paddr_t *out_phys = (ipo_paddr_t *)(uintptr_t)argv[2];
    if (out_phys && !syscall_user_buf_ok(out_phys, sizeof(*out_phys))) return 0u;
    return (ipo_uarg_t)(uintptr_t)ipo_hw_alloc_dma((ipo_size_t)argv[0], (ipo_size_t)argv[1], out_phys, syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_hw_free_dma(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (ipo_uarg_t)(ipo_sarg_t)-1;
    return (ipo_uarg_t)(ipo_sarg_t)ipo_hw_free_dma((void *)(uintptr_t)argv[0], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_hw_virt_to_phys(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return 0u;
    return (ipo_uarg_t)ipo_hw_virt_to_phys((const void *)(uintptr_t)argv[0], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_pci_get_count(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    return pci_get_count();
}

static ipo_uarg_t syscall_builtin_pci_get_device(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[1]) return (uint32_t)-1;
    ipo_pci_dev_info_t *out_info = (ipo_pci_dev_info_t *)(uintptr_t)argv[1];
    if (!syscall_user_buf_ok(out_info, sizeof(*out_info))) return (uint32_t)-1;
    if (out_info->bars && out_info->bars_cap > 0u) {
        if (out_info->bars_cap > IPO_SIZE_MAX / sizeof(uint32_t) ||
            !syscall_user_buf_ok(out_info->bars, (ipo_size_t)out_info->bars_cap * sizeof(uint32_t))) {
            return (uint32_t)-1;
        }
    }
    return (uint32_t)ipo_pci_fill_info(argv[0], out_info);
}

static ipo_uarg_t syscall_builtin_pci_config_read(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 5u || !argv) return 0xFFFFFFFFu;
    uint8_t bus = (uint8_t)argv[0], slot = (uint8_t)argv[1], func = (uint8_t)argv[2], off = (uint8_t)argv[3];
    if (argv[4] == 1u) return (uint32_t)pci_read_byte(bus, slot, func, off);
    if (argv[4] == 2u) return (uint32_t)pci_read_word(bus, slot, func, off);
    return pci_read_dword(bus, slot, func, off);
}

static ipo_uarg_t syscall_builtin_pci_config_write(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 6u || !argv) return (uint32_t)-1;
    uint8_t bus = (uint8_t)argv[0], slot = (uint8_t)argv[1], func = (uint8_t)argv[2], off = (uint8_t)argv[3];
    if (argv[4] == 1u) pci_write_byte(bus, slot, func, off, (uint8_t)argv[5]);
    else if (argv[4] == 2u) pci_write_word(bus, slot, func, off, (uint16_t)argv[5]);
    else pci_write_dword(bus, slot, func, off, argv[5]);
    return 0u;
}

static ipo_uarg_t syscall_builtin_pci_enable_bus_master(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 3u || !argv) return (uint32_t)-1;
    pci_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.bus = (uint8_t)argv[0];
    dev.slot = (uint8_t)argv[1];
    dev.func = (uint8_t)argv[2];
    pci_enable_bus_mastering(&dev);
    return 0u;
}

static ipo_uarg_t syscall_builtin_dev_get_count(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    return device_get_count();
}

static ipo_uarg_t syscall_builtin_dev_get_info(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[1]) return (uint32_t)-1;
    ipo_device_info_t *out_info = (ipo_device_info_t *)(uintptr_t)argv[1];
    if (!syscall_user_buf_ok(out_info, sizeof(*out_info))) return (uint32_t)-1;
    if (out_info->name && out_info->name_cap > 0u &&
        !syscall_user_buf_ok(out_info->name, out_info->name_cap)) return (uint32_t)-1;
    if (out_info->vendor && out_info->vendor_cap > 0u &&
        !syscall_user_buf_ok(out_info->vendor, out_info->vendor_cap)) return (uint32_t)-1;
    if (out_info->model && out_info->model_cap > 0u &&
        !syscall_user_buf_ok(out_info->model, out_info->model_cap)) return (uint32_t)-1;
    if (out_info->description && out_info->desc_cap > 0u &&
        !syscall_user_buf_ok(out_info->description, out_info->desc_cap)) return (uint32_t)-1;
    if (out_info->driver_name && out_info->driver_name_cap > 0u &&
        !syscall_user_buf_ok(out_info->driver_name, out_info->driver_name_cap)) return (uint32_t)-1;
    return (uint32_t)ipo_device_fill_info(argv[0], out_info);
}

static ipo_uarg_t syscall_builtin_dev_register(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return 0u;
    const ipo_device_info_t *info = (const ipo_device_info_t *)(uintptr_t)argv[0];
    if (!syscall_user_buf_ok(info, sizeof(*info))) return 0u;
    if (!syscall_user_str_ok(info->name)) return 0u;
    if (info->vendor && !syscall_user_str_ok(info->vendor)) return 0u;
    if (info->model && !syscall_user_str_ok(info->model)) return 0u;
    if (info->description && !syscall_user_str_ok(info->description)) return 0u;
    if (info->driver_name && !syscall_user_str_ok(info->driver_name)) return 0u;
    return ipo_device_register_user(info, syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_dev_unregister(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)-1;
    return (uint32_t)ipo_device_unregister_user(argv[0], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_kmod_load(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return (uint32_t)-1;
    const char *path = (const char *)(uintptr_t)argv[0];
    if (!syscall_user_str_ok(path)) return (uint32_t)-2;
    kmod_t *mod = NULL;
    int rc = kmod_load_file(path, &mod);
    if (rc < 0) return (uint32_t)rc;
    if (argc >= 2u && argv[1] && syscall_user_buf_ok((void *)(uintptr_t)argv[1], sizeof(uint32_t))) {
        *(uint32_t *)(uintptr_t)argv[1] = mod ? mod->id : 0u;
    }
    return 0u;
}

static ipo_uarg_t syscall_builtin_kmod_unload(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return (uint32_t)-1;
    const char *name = (const char *)(uintptr_t)argv[0];
    if (!syscall_user_str_ok(name)) return (uint32_t)-2;
    return (uint32_t)kmod_unload_by_name(name);
}

static ipo_uarg_t syscall_builtin_kmod_count(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num; (void)argc; (void)argv;
    return kmod_count();
}

static bool syscall_ext_desc_ok(const ipo_ext_desc_t *desc) {
    if (!desc || !syscall_user_buf_ok(desc, sizeof(*desc))) return false;
    if (!desc->name || !syscall_user_str_ok(desc->name)) return false;
    if (desc->category && !syscall_user_str_ok(desc->category)) return false;
    if (desc->description && !syscall_user_str_ok(desc->description)) return false;
    if (desc->ops_table && desc->ops_size > 0u &&
        !syscall_user_buf_ok(desc->ops_table, desc->ops_size)) {
        return false;
    }
    return true;
}

static ipo_uarg_t syscall_builtin_ext_register(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return 0u;
    const ipo_ext_desc_t *desc = (const ipo_ext_desc_t *)(uintptr_t)argv[0];
    if (!syscall_ext_desc_ok(desc)) return 0u;
    return ipo_ext_register(desc, syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_ext_unregister(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)-1;
    return (uint32_t)ipo_ext_unregister(argv[0], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_ext_update(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[1]) return (uint32_t)-1;
    const ipo_ext_desc_t *desc = (const ipo_ext_desc_t *)(uintptr_t)argv[1];
    if (!syscall_ext_desc_ok(desc)) return (uint32_t)-1;
    return (uint32_t)ipo_ext_update(argv[0], desc, syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_ext_call(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[0]) return (ipo_uarg_t)(ipo_sarg_t)-1;
    const char *name = (const char *)(uintptr_t)argv[0];
    if (!syscall_user_str_ok(name)) return (ipo_uarg_t)(ipo_sarg_t)-1;
    ipo_uarg_t a0 = (argc >= 3u) ? argv[2] : 0u;
    ipo_uarg_t a1 = (argc >= 4u) ? argv[3] : 0u;
    ipo_uarg_t a2 = (argc >= 5u) ? argv[4] : 0u;
    ipo_uarg_t a3 = (argc >= 6u) ? argv[5] : 0u;
    return (ipo_uarg_t)ipo_ext_call(name, (uint32_t)argv[1], a0, a1, a2, a3);
}

static ipo_uarg_t syscall_builtin_ext_list(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    ipo_ext_info_t *out_arr = (argc >= 1u && argv) ? (ipo_ext_info_t *)(uintptr_t)argv[0] : NULL;
    ipo_count_t max_cnt = (argc >= 2u && argv) ? (ipo_count_t)argv[1] : 0u;
    if (out_arr && max_cnt > 0u) {
        if (max_cnt > IPO_SIZE_MAX / sizeof(ipo_ext_info_t) ||
            !syscall_user_buf_ok(out_arr, (ipo_size_t)max_cnt * sizeof(ipo_ext_info_t))) {
            return (ipo_uarg_t)(ipo_sarg_t)-1;
        }
        for (ipo_count_t i = 0; i < max_cnt; i++) {
            if (out_arr[i].name && out_arr[i].name_cap > 0u &&
                !syscall_user_buf_ok(out_arr[i].name, out_arr[i].name_cap)) return (ipo_uarg_t)(ipo_sarg_t)-1;
            if (out_arr[i].category && out_arr[i].category_cap > 0u &&
                !syscall_user_buf_ok(out_arr[i].category, out_arr[i].category_cap)) return (ipo_uarg_t)(ipo_sarg_t)-1;
            if (out_arr[i].description && out_arr[i].desc_cap > 0u &&
                !syscall_user_buf_ok(out_arr[i].description, out_arr[i].desc_cap)) return (ipo_uarg_t)(ipo_sarg_t)-1;
        }
    }
    return (ipo_uarg_t)ipo_ext_list(out_arr, (uint32_t)max_cnt);
}

static ipo_uarg_t syscall_builtin_chan_list(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    ipo_chan_info_t *out_arr = (argc >= 1u && argv) ? (ipo_chan_info_t *)(uintptr_t)argv[0] : NULL;
    ipo_count_t max_cnt = (argc >= 2u && argv) ? (ipo_count_t)argv[1] : 0u;
    if (out_arr && max_cnt > 0u) {
        if (max_cnt > IPO_SIZE_MAX / sizeof(ipo_chan_info_t) ||
            !syscall_user_buf_ok(out_arr, (ipo_size_t)max_cnt * sizeof(ipo_chan_info_t))) {
            return (ipo_uarg_t)(ipo_sarg_t)-1;
        }
        for (ipo_count_t i = 0; i < max_cnt; i++) {
            if (out_arr[i].name && out_arr[i].name_cap > 0u &&
                !syscall_user_buf_ok(out_arr[i].name, out_arr[i].name_cap)) return (ipo_uarg_t)(ipo_sarg_t)-1;
            if (out_arr[i].hw_addr && out_arr[i].hw_addr_cap > 0u &&
                !syscall_user_buf_ok(out_arr[i].hw_addr, out_arr[i].hw_addr_cap)) return (ipo_uarg_t)(ipo_sarg_t)-1;
        }
    }
    return (ipo_uarg_t)ipo_chan_list(out_arr, (uint32_t)max_cnt);
}

static ipo_uarg_t syscall_builtin_chan_tx(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 3u || !argv || !argv[1] || argv[2] == 0u) {
        return (uint32_t)-1;
    }
    const void *frame = (const void *)(uintptr_t)argv[1];
    if (!syscall_user_buf_ok(frame, argv[2])) return (uint32_t)-1;
    return (uint32_t)ipo_chan_transmit(argv[0], frame, argv[2]);
}

static ipo_uarg_t syscall_builtin_chan_rx(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 3u || !argv || !argv[1] || argv[2] == 0u) return (uint32_t)-1;
    void *buf = (void *)(uintptr_t)argv[1];
    if (!syscall_user_buf_ok(buf, argv[2])) return (uint32_t)-1;
    ipo_chan_poll_all();
    return (uint32_t)ipo_chan_poll_sub(argv[0], buf, argv[2]);
}

static ipo_uarg_t syscall_builtin_chan_sub(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv) return 0u;
    return ipo_chan_subscribe(argv[0], (uint16_t)argv[1], NULL, NULL, syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_chan_unsub(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)-1;
    return (uint32_t)ipo_chan_unsubscribe(argv[0], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_smp_get_count(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc >= 1u && argv && argv[0]) {
        if (!syscall_user_buf_ok((void *)(uintptr_t)argv[0], sizeof(uint32_t))) return (uint32_t)-1;
        *(uint32_t *)(uintptr_t)argv[0] = smp_get_active_cpu_count();
    }
    return smp_get_cpu_count();
}

static ipo_uarg_t syscall_builtin_smp_get_info(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[1]) return (uint32_t)SMP_ERR_INVALID_PARAM;
    cpu_info_t *out = (cpu_info_t *)(uintptr_t)argv[1];
    if (!syscall_user_buf_ok(out, sizeof(*out))) return (uint32_t)SMP_ERR_INVALID_PARAM;
    const cpu_info_t *info = smp_get_cpu_info(argv[0]);
    if (!info) return (uint32_t)SMP_ERR_INVALID_CPU;
    memcpy(out, info, sizeof(*out));
    return (uint32_t)SMP_ERR_OK;
}

static ipo_uarg_t syscall_builtin_smp_submit(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 4u || !argv || !argv[1]) return (uint32_t)SMP_ERR_INVALID_PARAM;
    uint32_t target_cpu = argv[0];
    smp_work_fn_t fn = (smp_work_fn_t)(uintptr_t)argv[1];
    if (!syscall_user_buf_ok((const void *)(uintptr_t)argv[1], 1u)) {
        return (uint32_t)SMP_ERR_INVALID_PARAM;
    }
    uintptr_t arg = (uintptr_t)argv[2];
    uint32_t *out_tid = (uint32_t *)(uintptr_t)argv[3];
    if (out_tid && !syscall_user_buf_ok(out_tid, sizeof(uint32_t))) {
        return (uint32_t)SMP_ERR_INVALID_PARAM;
    }
    uint32_t tid = 0u;
    int rc = smp_submit_task(target_cpu, fn, arg, 0u, &tid);
    if (rc == SMP_ERR_OK && out_tid) {
        *out_tid = tid;
    }
    return (uint32_t)rc;
}

static ipo_uarg_t syscall_builtin_smp_wait(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv) return (uint32_t)SMP_ERR_INVALID_PARAM;
    smp_task_result_t *out_res = (argc >= 3u && argv[2]) ? (smp_task_result_t *)(uintptr_t)argv[2] : NULL;
    if (out_res && !syscall_user_buf_ok(out_res, sizeof(*out_res))) {
        return (uint32_t)SMP_ERR_INVALID_PARAM;
    }
    return (uint32_t)smp_wait_task(argv[0], argv[1], out_res);
}

static ipo_uarg_t syscall_builtin_smp_current(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc >= 1u && argv && argv[0]) {
        if (!syscall_user_buf_ok((void *)(uintptr_t)argv[0], sizeof(uint32_t))) return (uint32_t)-1;
        *(uint32_t *)(uintptr_t)argv[0] = smp_get_hw_lapic_id();
    }
    return smp_get_current_cpu_id();
}

static ipo_uarg_t syscall_builtin_dynmod_auth(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)DYNMOD_ERR_PARAM;
    return (uint32_t)dynmod_authorize(syscall_caller_pid(), argv[0]);
}

static ipo_uarg_t syscall_builtin_dynmod_resolve(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[0] || !argv[1]) return (ipo_uarg_t)(ipo_sarg_t)DYNMOD_ERR_PARAM;
    const char *name = (const char *)(uintptr_t)argv[0];
    ipo_vaddr_t *out_addr = (ipo_vaddr_t *)(uintptr_t)argv[1];
    if (!syscall_user_str_ok(name) || !syscall_user_buf_ok(out_addr, sizeof(*out_addr))) {
        return (ipo_uarg_t)(ipo_sarg_t)DYNMOD_ERR_ADDR;
    }
    return (ipo_uarg_t)(ipo_sarg_t)dynmod_resolve_symbol(name, out_addr);
}

static ipo_uarg_t syscall_builtin_dynmod_read(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 3u || !argv || !argv[1] || argv[2] == 0u) return (uint32_t)DYNMOD_ERR_PARAM;
    void *out_buf = (void *)(uintptr_t)argv[1];
    if (!syscall_user_buf_ok(out_buf, argv[2])) return (uint32_t)DYNMOD_ERR_ADDR;
    return (uint32_t)dynmod_read_mem((uintptr_t)argv[0], out_buf, (size_t)argv[2]);
}

static ipo_uarg_t syscall_builtin_dynmod_write_var(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 3u || !argv || !argv[1] || argv[2] == 0u) return (uint32_t)DYNMOD_ERR_PARAM;
    const void *in_buf = (const void *)(uintptr_t)argv[1];
    if (!syscall_user_buf_ok(in_buf, argv[2])) return (uint32_t)DYNMOD_ERR_ADDR;
    uint32_t *out_pid = (argc >= 4u && argv[3]) ? (uint32_t *)(uintptr_t)argv[3] : NULL;
    if (out_pid && !syscall_user_buf_ok(out_pid, sizeof(uint32_t))) return (uint32_t)DYNMOD_ERR_ADDR;
    return (uint32_t)dynmod_write_var((uintptr_t)argv[0], in_buf, (size_t)argv[2], out_pid, syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_dynmod_hook_func(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv) return (uint32_t)DYNMOD_ERR_PARAM;
    uint32_t *out_pid = (argc >= 3u && argv[2]) ? (uint32_t *)(uintptr_t)argv[2] : NULL;
    if (out_pid && !syscall_user_buf_ok(out_pid, sizeof(uint32_t))) return (uint32_t)DYNMOD_ERR_ADDR;
    return (uint32_t)dynmod_hook_function((uintptr_t)argv[0], (uintptr_t)argv[1], out_pid, syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_dynmod_hook_syscall(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 2u || !argv) return (uint32_t)DYNMOD_ERR_PARAM;
    ipo_syscall_handler_t *out_old = (argc >= 3u && argv[2]) ? (ipo_syscall_handler_t *)(uintptr_t)argv[2] : NULL;
    uint32_t *out_pid = (argc >= 4u && argv[3]) ? (uint32_t *)(uintptr_t)argv[3] : NULL;
    if (out_old && !syscall_user_buf_ok(out_old, sizeof(uintptr_t))) return (uint32_t)DYNMOD_ERR_ADDR;
    if (out_pid && !syscall_user_buf_ok(out_pid, sizeof(uint32_t))) return (uint32_t)DYNMOD_ERR_ADDR;
    return (uint32_t)dynmod_hook_syscall(argv[0], (ipo_syscall_handler_t)(uintptr_t)argv[1],
                                         out_old, out_pid, syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_dynmod_restore(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)DYNMOD_ERR_PARAM;
    return (uint32_t)dynmod_restore(argv[0], syscall_caller_pid());
}

static ipo_uarg_t syscall_builtin_dynmod_list(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    ipo_dynmod_info_t *out_arr = (argc >= 1u && argv) ? (ipo_dynmod_info_t *)(uintptr_t)argv[0] : NULL;
    ipo_count_t max_cnt = (argc >= 2u && argv) ? (ipo_count_t)argv[1] : 0u;
    if (out_arr && max_cnt > 0u) {
        if (max_cnt > IPO_SIZE_MAX / sizeof(ipo_dynmod_info_t) ||
            !syscall_user_buf_ok(out_arr, (ipo_size_t)max_cnt * sizeof(ipo_dynmod_info_t))) {
            return (ipo_uarg_t)(ipo_sarg_t)-1;
        }
        for (ipo_count_t i = 0; i < max_cnt; i++) {
            if (out_arr[i].orig_bytes && out_arr[i].orig_cap > 0u &&
                !syscall_user_buf_ok(out_arr[i].orig_bytes, out_arr[i].orig_cap)) return (ipo_uarg_t)(ipo_sarg_t)-1;
            if (out_arr[i].new_bytes && out_arr[i].new_cap > 0u &&
                !syscall_user_buf_ok(out_arr[i].new_bytes, out_arr[i].new_cap)) return (ipo_uarg_t)(ipo_sarg_t)-1;
        }
    }
    return (ipo_uarg_t)dynmod_list(out_arr, (uint32_t)max_cnt);
}

static ipo_uarg_t syscall_builtin_dynmod_compute_demo(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    (void)num;
    uint32_t x = (argc >= 1u && argv) ? argv[0] : 0u;
    uint32_t y = (argc >= 2u && argv) ? argv[1] : 0u;
    return dynmod_target_compute(x, y);
}

void syscall_init(void) {
    if (!ipc_init()) {
        serial_printf("[syscall] IPC subsystem unavailable\n");
    }


    ipo_register_syscall(
        IPO_SYSCALL_REGISTER,
        syscall_builtin_register);

    ipo_register_syscall(
        IPO_SYSCALL_CALL,
        syscall_builtin_call);

    ipo_register_syscall(
        IPO_SYSCALL_PRINT,
        syscall_builtin_print);

    ipo_register_syscall(
        IPO_SYSCALL_WRITE,
        syscall_builtin_write);

    ipo_register_syscall(
        IPO_SYSCALL_FS_OPEN,
        syscall_builtin_fs_open);

    ipo_register_syscall(
        IPO_SYSCALL_FS_CLOSE,
        syscall_builtin_fs_close);

    ipo_register_syscall(
        IPO_SYSCALL_FS_SEEK,
        syscall_builtin_fs_seek);

    ipo_register_syscall(
        IPO_SYSCALL_FS_READ,
        syscall_builtin_fs_read);

    ipo_register_syscall(
        IPO_SYSCALL_FS_WRITE,
        syscall_builtin_fs_write);

    ipo_register_syscall(
        IPO_SYSCALL_FS_DELETE,
        syscall_builtin_fs_delete);

    ipo_register_syscall(
        IPO_SYSCALL_FS_STAT,
        syscall_builtin_fs_stat);

    ipo_register_syscall(
        IPO_SYSCALL_FS_STATVFS,
        syscall_builtin_fs_statvfs);

    ipo_register_syscall(
        IPO_SYSCALL_FS_LIST,
        syscall_builtin_fs_list);

    ipo_register_syscall(
        IPO_SYSCALL_FS_RENAME,
        syscall_builtin_fs_rename);

    ipo_register_syscall(
        IPO_SYSCALL_EXEC,
        syscall_builtin_exec);

    ipo_register_syscall(
        IPO_SYSCALL_READ,
        syscall_builtin_read);

    ipo_register_syscall(
        IPO_SYSCALL_TERMINAL_INPUT,
        syscall_builtin_terminal_input);

    ipo_register_syscall(
        IPO_SYSCALL_PROCESS_YIELD,
        syscall_builtin_process_yield);

    ipo_register_syscall(
        IPO_SYSCALL_PROCESS_IS_FOREGROUND,
        syscall_builtin_process_is_foreground);

    ipo_register_syscall(
        IPO_SYSCALL_PROCESS_IS_BATCH_ACTIVE,
        syscall_builtin_process_is_batch_active);

    ipo_register_syscall(
        IPO_SYSCALL_PROCESS_SWITCH_FG,
        syscall_builtin_process_switch_fg);

    ipo_register_syscall(
        IPO_SYSCALL_GETCWD,
        syscall_builtin_getcwd);

    ipo_register_syscall(
        IPO_SYSCALL_CHDIR,
        syscall_builtin_chdir);

    ipo_register_syscall(
        IPO_SYSCALL_GET_EXIT_CODE,
        syscall_builtin_get_exit_code);

    ipo_register_syscall(
        IPO_SYSCALL_SET_RETURN_VALUE,
        syscall_builtin_set_return_value);

    ipo_register_syscall(
        IPO_SYSCALL_GET_RETURN_VALUE,
        syscall_builtin_get_return_value);

    ipo_register_syscall(
        IPO_SYSCALL_ASYNC_START,
        syscall_builtin_async_start);

    ipo_register_syscall(
        IPO_SYSCALL_ASYNC_STOP,
        syscall_builtin_async_stop);

    ipo_register_syscall(
        IPO_SYSCALL_STACK_GROW,
        syscall_builtin_stack_grow);

    ipo_register_syscall(
        IPO_SYSCALL_STACK_SHRINK,
        syscall_builtin_stack_shrink);

    ipo_register_syscall(
        IPO_SYSCALL_SBRK,
        syscall_builtin_sbrk);

    ipo_register_syscall(
        IPO_SYSCALL_TIME,
        syscall_builtin_time);

    ipo_register_syscall(
        IPO_SYSCALL_MILLIS,
        syscall_builtin_millis);

    ipo_register_syscall(
        IPO_SYSCALL_ELAPSED_MS,
        syscall_builtin_elapsed_ms);

    ipo_register_syscall(
        IPO_SYSCALL_TIMER_AFTER,
        syscall_builtin_timer_after);

    ipo_register_syscall(IPO_SYSCALL_RTC_GET_STR, syscall_builtin_rtc_get_str);
    ipo_register_syscall(IPO_SYSCALL_RTC_SET_STR, syscall_builtin_rtc_set_str);
    ipo_register_syscall(IPO_SYSCALL_RTC_GET_FORMAT, syscall_builtin_rtc_get_format);
    ipo_register_syscall(IPO_SYSCALL_RTC_SET_FORMAT, syscall_builtin_rtc_set_format);
    ipo_register_syscall(IPO_SYSCALL_RTC_ADD_STR, syscall_builtin_rtc_add_str);
    ipo_register_syscall(IPO_SYSCALL_RTC_GET_DATE_FORMAT, syscall_builtin_rtc_get_date_format);
    ipo_register_syscall(IPO_SYSCALL_RTC_SET_DATE_FORMAT, syscall_builtin_rtc_set_date_format);
    ipo_register_syscall(IPO_SYSCALL_RTC_GET_DATE_STR, syscall_builtin_rtc_get_date_str);
    ipo_register_syscall(IPO_SYSCALL_RTC_GET_STATUS, syscall_builtin_rtc_get_status);

    ipo_register_syscall(IPO_SYSCALL_IPC_CREATE, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_OPEN, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_CLOSE, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_DESTROY, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_SEND, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_RECEIVE, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_PIPE_CREATE, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_PIPE_OPEN, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_PIPE_CLOSE, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_PIPE_DESTROY, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_PIPE_READ, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_PIPE_WRITE, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_NOTIFY_SEND, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_NOTIFY_WAIT, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_SHM_CREATE,
                         ipc_shared_memory_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_SHM_OPEN,
                         ipc_shared_memory_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_SHM_ATTACH,
                         ipc_shared_memory_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_SHM_DETACH,
                         ipc_shared_memory_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_SHM_DESTROY,
                         ipc_shared_memory_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_SIGACTION,
                         process_signal_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_SIGPROCMASK,
                         process_signal_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_KILL,
                         process_signal_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_SIGALTSTACK,
                         process_signal_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_THREAD_CREATE,
                         process_thread_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_THREAD_EXIT,
                         process_thread_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_THREAD_JOIN,
                         process_thread_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_THREAD_SELF,
                         process_thread_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_THREAD_SYNC,
                         process_thread_syscall_handler);

    ipo_register_syscall(
        IPO_SYSCALL_FREE,
        syscall_builtin_free);

    ipo_register_syscall(
        IPO_SYSCALL_EXIT,
        syscall_builtin_exit);

    ipo_register_syscall(
        IPO_SYSCALL_VAR_SET,
        syscall_builtin_var_set);

    ipo_register_syscall(
        IPO_SYSCALL_VAR_GET,
        syscall_builtin_var_get);

    ipo_register_syscall(
        IPO_SYSCALL_VAR_DELETE,
        syscall_builtin_var_delete);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_SET,
        syscall_builtin_keymap_set);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_GET,
        syscall_builtin_keymap_get);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_DISABLE,
        syscall_builtin_keymap_disable);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_ENABLE,
        syscall_builtin_keymap_enable);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_REMOVE,
        syscall_builtin_keymap_remove);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_TRANSLATE,
        syscall_builtin_keymap_translate);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_IS_ACTIVE,
        syscall_builtin_keymap_is_active);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_CYCLE_NEXT,
        syscall_builtin_keymap_cycle_next);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_CYCLE_PREV,
        syscall_builtin_keymap_cycle_prev);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_GET_NAME,
        syscall_builtin_keymap_get_name);

    ipo_register_syscall(
        IPO_SYSCALL_FONT_LOAD,
        syscall_builtin_font_load);

    ipo_register_syscall(
        IPO_SYSCALL_FONT_GET_INFO,
        syscall_builtin_font_get_info);

    ipo_register_syscall(
        IPO_SYSCALL_VGA_GLYPH,
        syscall_builtin_vga_glyph);

    ipo_register_syscall(
        IPO_SYSCALL_VGA_SET_MODE,
        syscall_builtin_vga_set_mode);

    ipo_register_syscall(
        IPO_SYSCALL_VGA_GET_MODE,
        syscall_builtin_vga_get_mode);

    ipo_register_syscall(
        IPO_SYSCALL_VGA_GET_BOUNDS,
        syscall_builtin_vga_get_bounds);

    ipo_register_syscall(
        IPO_SYSCALL_SYSTEM_INTERRUPT,
        syscall_builtin_system_interrupt);

    ipo_register_syscall(
        IPO_SYSCALL_DRIVER_REGISTER,
        syscall_builtin_driver_register);

    ipo_register_syscall(
        IPO_SYSCALL_DRIVER_UNREGISTER,
        syscall_builtin_driver_unregister);

    ipo_register_syscall(
        IPO_SYSCALL_DRIVER_LIST,
        syscall_builtin_driver_list);

    ipo_register_syscall(
        IPO_SYSCALL_DRIVER_SET_DESC,
        syscall_builtin_driver_set_desc);

    ipo_register_syscall(IPO_SYSCALL_PORT_IN8, syscall_builtin_port_io);
    ipo_register_syscall(IPO_SYSCALL_PORT_OUT8, syscall_builtin_port_io);
    ipo_register_syscall(IPO_SYSCALL_PORT_IN16, syscall_builtin_port_io);
    ipo_register_syscall(IPO_SYSCALL_PORT_OUT16, syscall_builtin_port_io);
    ipo_register_syscall(IPO_SYSCALL_PORT_IN32, syscall_builtin_port_io);
    ipo_register_syscall(IPO_SYSCALL_PORT_OUT32, syscall_builtin_port_io);

    ipo_register_syscall(
        IPO_SYSCALL_WM_CREATE_WINDOW,
        syscall_builtin_wm_create_window);

    ipo_register_syscall(
        IPO_SYSCALL_WM_DESTROY_WINDOW,
        syscall_builtin_wm_destroy_window);

    ipo_register_syscall(
        IPO_SYSCALL_WM_SESSION_START,
        syscall_builtin_wm_session_start);

    ipo_register_syscall(
        IPO_SYSCALL_WM_SESSION_STOP,
        syscall_builtin_wm_session_stop);

    ipo_register_syscall(
        IPO_SYSCALL_WM_SESSION_ACTIVE,
        syscall_builtin_wm_session_active);

    ipo_register_syscall(
        IPO_SYSCALL_WM_GET_COUNT,
        syscall_builtin_wm_get_count);

    ipo_register_syscall(
        IPO_SYSCALL_WM_GET_FOCUSED,
        syscall_builtin_wm_get_focused);

    ipo_register_syscall(
        IPO_SYSCALL_WM_GET_LIST,
        syscall_builtin_wm_get_list);

    ipo_register_syscall(
        IPO_SYSCALL_WM_SET_FOCUS,
        syscall_builtin_wm_set_focus);

    ipo_register_syscall(
        IPO_SYSCALL_WM_FOCUS_NEXT,
        syscall_builtin_wm_focus_next);

    ipo_register_syscall(
        IPO_SYSCALL_WM_FOCUS_PREV,
        syscall_builtin_wm_focus_prev);

    ipo_register_syscall(
        IPO_SYSCALL_WM_INVALIDATE,
        syscall_builtin_wm_invalidate);

    ipo_register_syscall(
        IPO_SYSCALL_WM_IS_VALID,
        syscall_builtin_wm_is_valid);

    ipo_register_syscall(
        IPO_SYSCALL_WM_TOGGLE_MAXIMIZE,
        syscall_builtin_wm_toggle_maximize);

    ipo_register_syscall(
        IPO_SYSCALL_WM_RESIZE,
        syscall_builtin_wm_resize);

    ipo_register_syscall(IPO_SYSCALL_WM_INIT, syscall_builtin_wm_init);
    ipo_register_syscall(IPO_SYSCALL_WM_INVALIDATE_ALL, syscall_builtin_wm_invalidate_all);
    ipo_register_syscall(IPO_SYSCALL_WM_SET_FULLSCREEN, syscall_builtin_wm_set_fullscreen);
    ipo_register_syscall(IPO_SYSCALL_WM_MINIMIZE, syscall_builtin_wm_minimize);
    ipo_register_syscall(IPO_SYSCALL_WM_RESTORE, syscall_builtin_wm_restore);
    ipo_register_syscall(IPO_SYSCALL_WM_MOVE, syscall_builtin_wm_move);
    ipo_register_syscall(IPO_SYSCALL_WM_SET_TITLE, syscall_builtin_wm_set_title);
    ipo_register_syscall(IPO_SYSCALL_WM_SET_SHAPE, syscall_builtin_wm_set_shape);
    ipo_register_syscall(IPO_SYSCALL_WM_POLL_EVENT, syscall_builtin_wm_poll_event);
    ipo_register_syscall(IPO_SYSCALL_WM_COMPOSITOR_TICK, syscall_builtin_wm_compositor_tick);
    ipo_register_syscall(IPO_SYSCALL_WM_GET_COUNT_PID, syscall_builtin_wm_get_count_for_pid);
    ipo_register_syscall(IPO_SYSCALL_WM_HAS_WINDOWS_PID, syscall_builtin_wm_has_windows_for_pid);
    ipo_register_syscall(IPO_SYSCALL_WM_FOCUS_PID, syscall_builtin_wm_focus_window_for_pid);
    ipo_register_syscall(IPO_SYSCALL_WM_SET_CURSOR_VISIBLE, syscall_builtin_wm_set_cursor_visible);
    ipo_register_syscall(IPO_SYSCALL_WM_GET_CURSOR_VISIBLE, syscall_builtin_wm_get_cursor_visible);

    ipo_register_syscall(
        IPO_SYSCALL_GETPID,
        syscall_builtin_getpid);

    ipo_register_syscall(IPO_SYSCALL_KBD_GET_SCANCODE, syscall_builtin_kbd_get_scancode);
    ipo_register_syscall(IPO_SYSCALL_KBD_WAIT_SCANCODE, syscall_builtin_kbd_wait_scancode);
    ipo_register_syscall(IPO_SYSCALL_KBD_SET_APP_MODE, syscall_builtin_kbd_set_app_mode);
    ipo_register_syscall(IPO_SYSCALL_KBD_FLUSH, syscall_builtin_kbd_flush);
    ipo_register_syscall(IPO_SYSCALL_KBD_GET_MODIFIERS, syscall_builtin_kbd_get_modifiers);
    ipo_register_syscall(IPO_SYSCALL_KBD_POLL, syscall_builtin_kbd_poll);
    ipo_register_syscall(IPO_SYSCALL_KBD_GET_KEY_STRING, syscall_builtin_kbd_get_key_string);

    ipo_register_syscall(IPO_SYSCALL_MOUSE_GET_STATE, syscall_builtin_mouse_get_state);
    ipo_register_syscall(IPO_SYSCALL_MOUSE_GET_MOTION, syscall_builtin_mouse_get_motion);

    ipo_register_syscall(IPO_SYSCALL_VGA_SET_CURSOR, syscall_builtin_vga_set_cursor);
    ipo_register_syscall(IPO_SYSCALL_VGA_GET_CURSOR, syscall_builtin_vga_get_cursor);
    ipo_register_syscall(IPO_SYSCALL_VGA_SHOW_CURSOR, syscall_builtin_vga_show_cursor);
    ipo_register_syscall(IPO_SYSCALL_VGA_HIDE_CURSOR, syscall_builtin_vga_hide_cursor);
    ipo_register_syscall(IPO_SYSCALL_VGA_SET_PALETTE, syscall_builtin_vga_set_palette);
    ipo_register_syscall(IPO_SYSCALL_VGA_SET_OVERLAY_FLAGS, syscall_builtin_vga_set_overlay_flags);
    ipo_register_syscall(IPO_SYSCALL_VGA_GET_OVERLAY_FLAGS, syscall_builtin_vga_get_overlay_flags);
    ipo_register_syscall(IPO_SYSCALL_VGA_GET_TEXT_INFO, syscall_builtin_vga_get_text_info);

    ipo_register_syscall(IPO_SYSCALL_AUDIO_INIT, syscall_builtin_audio_init);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_TICK, syscall_builtin_audio_tick);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STOP_ALL, syscall_builtin_audio_stop_all);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_CREATE, syscall_builtin_audio_stream_create);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_CREATE_RING, syscall_builtin_audio_stream_create_ring);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_DESTROY, syscall_builtin_audio_stream_destroy);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_PLAY, syscall_builtin_audio_stream_play);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_PAUSE, syscall_builtin_audio_stream_pause);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_STOP, syscall_builtin_audio_stream_stop);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_SEEK, syscall_builtin_audio_stream_seek);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_SET_VOL, syscall_builtin_audio_stream_set_vol);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_PROGRESS, syscall_builtin_audio_stream_progress);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_UPDATE, syscall_builtin_audio_stream_update);

    ipo_register_syscall(IPO_SYSCALL_BINFMT_REGISTER, syscall_builtin_binfmt_register);
    ipo_register_syscall(IPO_SYSCALL_BINFMT_UNREGISTER, syscall_builtin_binfmt_unregister);
    ipo_register_syscall(IPO_SYSCALL_BINFMT_LIST, syscall_builtin_binfmt_list);

    ipo_register_syscall(IPO_SYSCALL_INPUT_POLL_KEY_EXT, syscall_builtin_input_poll_key_ext);
    ipo_register_syscall(IPO_SYSCALL_INPUT_GET_KEY_STATE, syscall_builtin_input_get_key_state);
    ipo_register_syscall(IPO_SYSCALL_INPUT_GET_MODIFIERS, syscall_builtin_input_get_modifiers);
    ipo_register_syscall(IPO_SYSCALL_SHORTCUT_REGISTER, syscall_builtin_shortcut_register);
    ipo_register_syscall(IPO_SYSCALL_SHORTCUT_UNREGISTER, syscall_builtin_shortcut_unregister);
    ipo_register_syscall(IPO_SYSCALL_SHORTCUT_POLL, syscall_builtin_shortcut_poll);
    ipo_register_syscall(IPO_SYSCALL_INPUT_DEV_REGISTER, syscall_builtin_input_dev_register);
    ipo_register_syscall(IPO_SYSCALL_INPUT_DEV_UNREGISTER, syscall_builtin_input_dev_unregister);
    ipo_register_syscall(IPO_SYSCALL_INPUT_DEV_EMIT, syscall_builtin_input_dev_emit);
    ipo_register_syscall(IPO_SYSCALL_INPUT_DEV_LIST, syscall_builtin_input_dev_list);
    ipo_register_syscall(IPO_SYSCALL_INPUT_DEV_POLL_EVENT, syscall_builtin_input_dev_poll_event);
    ipo_register_syscall(IPO_SYSCALL_INPUT_DEV_GET_STATE, syscall_builtin_input_dev_get_state);
    ipo_register_syscall(IPO_SYSCALL_MOUSE_INJECT, syscall_builtin_mouse_inject);

    ipo_register_syscall(IPO_SYSCALL_PS2_HOOK_REGISTER, syscall_builtin_ps2_hook_register);
    ipo_register_syscall(IPO_SYSCALL_PS2_HOOK_UNREGISTER, syscall_builtin_ps2_hook_unregister);
    ipo_register_syscall(IPO_SYSCALL_PS2_HOOK_POLL, syscall_builtin_ps2_hook_poll);
    ipo_register_syscall(IPO_SYSCALL_PS2_TRANSACT, syscall_builtin_ps2_transact);

    ipo_register_syscall(IPO_SYSCALL_IRQ_SUBSCRIBE, syscall_builtin_irq_subscribe);
    ipo_register_syscall(IPO_SYSCALL_IRQ_UNSUBSCRIBE, syscall_builtin_irq_unsubscribe);
    ipo_register_syscall(IPO_SYSCALL_IRQ_POLL, syscall_builtin_irq_poll);
    ipo_register_syscall(IPO_SYSCALL_IRQ_GET_COUNT, syscall_builtin_irq_get_count);
    ipo_register_syscall(IPO_SYSCALL_IRQ_REGISTER_CB, syscall_builtin_irq_register_cb);
    ipo_register_syscall(IPO_SYSCALL_IRQ_UNREGISTER_CB, syscall_builtin_irq_unregister_cb);

    ipo_register_syscall(IPO_SYSCALL_HW_MAP_MMIO, syscall_builtin_hw_map_mmio);
    ipo_register_syscall(IPO_SYSCALL_HW_UNMAP_MMIO, syscall_builtin_hw_unmap_mmio);
    ipo_register_syscall(IPO_SYSCALL_HW_ALLOC_DMA, syscall_builtin_hw_alloc_dma);
    ipo_register_syscall(IPO_SYSCALL_HW_FREE_DMA, syscall_builtin_hw_free_dma);
    ipo_register_syscall(IPO_SYSCALL_HW_VIRT_TO_PHYS, syscall_builtin_hw_virt_to_phys);

    ipo_register_syscall(IPO_SYSCALL_PCI_GET_COUNT, syscall_builtin_pci_get_count);
    ipo_register_syscall(IPO_SYSCALL_PCI_GET_DEVICE, syscall_builtin_pci_get_device);
    ipo_register_syscall(IPO_SYSCALL_PCI_CONFIG_READ, syscall_builtin_pci_config_read);
    ipo_register_syscall(IPO_SYSCALL_PCI_CONFIG_WRITE, syscall_builtin_pci_config_write);
    ipo_register_syscall(IPO_SYSCALL_PCI_ENABLE_BUS_MASTER, syscall_builtin_pci_enable_bus_master);

    ipo_register_syscall(IPO_SYSCALL_DEV_GET_COUNT, syscall_builtin_dev_get_count);
    ipo_register_syscall(IPO_SYSCALL_DEV_GET_INFO, syscall_builtin_dev_get_info);
    ipo_register_syscall(IPO_SYSCALL_DEV_REGISTER, syscall_builtin_dev_register);
    ipo_register_syscall(IPO_SYSCALL_DEV_UNREGISTER, syscall_builtin_dev_unregister);

    ipo_register_syscall(IPO_SYSCALL_KMOD_LOAD, syscall_builtin_kmod_load);
    ipo_register_syscall(IPO_SYSCALL_KMOD_UNLOAD, syscall_builtin_kmod_unload);
    ipo_register_syscall(IPO_SYSCALL_KMOD_COUNT, syscall_builtin_kmod_count);

    ipo_register_syscall(IPO_SYSCALL_EXT_REGISTER, syscall_builtin_ext_register);
    ipo_register_syscall(IPO_SYSCALL_EXT_UNREGISTER, syscall_builtin_ext_unregister);
    ipo_register_syscall(IPO_SYSCALL_EXT_UPDATE, syscall_builtin_ext_update);
    ipo_register_syscall(IPO_SYSCALL_EXT_CALL, syscall_builtin_ext_call);
    ipo_register_syscall(IPO_SYSCALL_EXT_LIST, syscall_builtin_ext_list);

    ipo_register_syscall(IPO_SYSCALL_CHAN_LIST, syscall_builtin_chan_list);
    ipo_register_syscall(IPO_SYSCALL_CHAN_TX, syscall_builtin_chan_tx);
    ipo_register_syscall(IPO_SYSCALL_CHAN_RX, syscall_builtin_chan_rx);
    ipo_register_syscall(IPO_SYSCALL_CHAN_SUB, syscall_builtin_chan_sub);
    ipo_register_syscall(IPO_SYSCALL_CHAN_UNSUB, syscall_builtin_chan_unsub);

    ipo_register_syscall(IPO_SYSCALL_SMP_GET_COUNT, syscall_builtin_smp_get_count);
    ipo_register_syscall(IPO_SYSCALL_SMP_GET_INFO, syscall_builtin_smp_get_info);
    ipo_register_syscall(IPO_SYSCALL_SMP_SUBMIT, syscall_builtin_smp_submit);
    ipo_register_syscall(IPO_SYSCALL_SMP_WAIT, syscall_builtin_smp_wait);
    ipo_register_syscall(IPO_SYSCALL_SMP_CURRENT, syscall_builtin_smp_current);

    ipo_register_syscall(IPO_SYSCALL_DYNMOD_AUTH, syscall_builtin_dynmod_auth);
    ipo_register_syscall(IPO_SYSCALL_DYNMOD_RESOLVE, syscall_builtin_dynmod_resolve);
    ipo_register_syscall(IPO_SYSCALL_DYNMOD_READ, syscall_builtin_dynmod_read);
    ipo_register_syscall(IPO_SYSCALL_DYNMOD_WRITE_VAR, syscall_builtin_dynmod_write_var);
    ipo_register_syscall(IPO_SYSCALL_DYNMOD_HOOK_FUNC, syscall_builtin_dynmod_hook_func);
    ipo_register_syscall(IPO_SYSCALL_DYNMOD_HOOK_SYSCALL, syscall_builtin_dynmod_hook_syscall);
    ipo_register_syscall(IPO_SYSCALL_DYNMOD_RESTORE, syscall_builtin_dynmod_restore);
    ipo_register_syscall(IPO_SYSCALL_DYNMOD_LIST, syscall_builtin_dynmod_list);
    ipo_register_syscall(IPO_SYSCALL_DYNMOD_COMPUTE_DEMO, syscall_builtin_dynmod_compute_demo);

    syscall_trap_init();
    printf("[syscall] System call dispatch table initialized (IDT vector 0x80 registered).\n");
}

uint64_t syscall_dispatch(ipo_uarg_t num,
                          ipo_uarg_t argc,
                          ipo_uarg_t *argv)
{
    process_t *curr = process_get_current();
    if (curr && curr->demand_loading) {
        process_demand_load_step(curr);
    }
    uint64_t result;
    if (num == IPO_SYSCALL_FS_CREATE) {
        if (argc < 2u || argv == NULL ||
            (process_in_process_context() && !ipc_user_range(argv, 2u * sizeof(ipo_uarg_t)))) {
            result = (uint64_t)(int64_t)-1;
        } else {
            const char *path = (const char *)(uintptr_t)argv[0];
            if (path == NULL ||
                (process_in_process_context() && !ipc_user_range(path, 1u))) {
                result = (uint64_t)(int64_t)-1;
            } else {
                result = (uint64_t)ipo_fs_create(path, (uint8_t)argv[1]);
            }
        }
    } else {
        ipo_syscall_handler_t fn = (ipo_syscall_table && num < ipo_syscall_table_size)
                                       ? ipo_syscall_table[num]
                                       : NULL;
        if (fn == NULL) {
            return IPO_SYSCALL_ENOSYS;
        }
        result = (uint64_t)fn(num, argc, argv);
    }
    process_signal_deliver_pending();
    if (curr && !curr->is_running && process_in_process_context()) {
        process_yield_kernel();
    }
    return result;
}