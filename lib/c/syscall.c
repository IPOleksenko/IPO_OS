#include <syscall.h>
#include <stdio.h>
#include <string.h>
#include <file_system/ipo_fs.h>
#include <kernel/process.h>
#include <memory/kmalloc.h>
#include <kernel/terminal.h>

#define IPO_IDT_ENTRY_FLAGS 0xEEu
#define IPO_KERNEL_CODE_SEG 0x08u

typedef struct {
    uint16_t base_lo;
    uint16_t selector;
    uint8_t zero;
    uint8_t flags;
    uint16_t base_hi;
} __attribute__((packed)) ipo_idt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) ipo_idt_ptr_t;

static ipo_idt_entry_t ipo_idt_table[256];
static ipo_syscall_handler_t *ipo_syscall_table = NULL;
static uint32_t ipo_syscall_table_size = 0;

extern void syscall_isr_entry(void);

static void ipo_idt_set_gate(uint8_t num, uint32_t base, uint16_t selector, uint8_t flags) {
    ipo_idt_table[num].base_lo = (uint16_t)(base & 0xFFFFu);
    ipo_idt_table[num].selector = selector;
    ipo_idt_table[num].zero = 0;
    ipo_idt_table[num].flags = flags;
    ipo_idt_table[num].base_hi = (uint16_t)((base >> 16) & 0xFFFFu);
}

void ipo_register_syscall(uint32_t num, ipo_syscall_handler_t handler) {
    if (ipo_syscall_table == NULL || num >= ipo_syscall_table_size) {
        uint32_t new_size = (num + 1u > 64u) ? (num + 1u) : 64u;
        while (new_size <= num) {
            new_size *= 2u;
        }

        ipo_syscall_handler_t *new_table = (ipo_syscall_handler_t *)kmalloc(new_size * sizeof(ipo_syscall_handler_t));
        if (new_table == NULL) {
            return;
        }

        if (ipo_syscall_table != NULL) {
            memcpy(new_table, ipo_syscall_table, ipo_syscall_table_size * sizeof(ipo_syscall_handler_t));
            kfree(ipo_syscall_table);
        }

        memset(new_table + ipo_syscall_table_size, 0,
               (new_size - ipo_syscall_table_size) * sizeof(ipo_syscall_handler_t));

        ipo_syscall_table = new_table;
        ipo_syscall_table_size = new_size;
    }

    ipo_syscall_table[num] = handler;
}

static uint32_t syscall_builtin_register(uint32_t num, uint32_t arg1, uint32_t arg2,
                                        uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)num; (void)arg3; (void)arg4; (void)arg5;
    if (arg1 >= 0x10000000u) {
        return IPO_SYSCALL_ENOSYS;
    }
    ipo_register_syscall(arg1, (ipo_syscall_handler_t)(uintptr_t)arg2);
    return IPO_SYSCALL_OK;
}

static uint32_t syscall_builtin_call(uint32_t num, uint32_t arg1, uint32_t arg2,
                                    uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)num;
    if (arg1 >= ipo_syscall_table_size) {
        return IPO_SYSCALL_ENOSYS;
    }
    ipo_syscall_handler_t fn = ipo_syscall_table[arg1];
    if (fn == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    return fn(arg1, arg2, arg3, arg4, arg5, 0u);
}

static uint32_t syscall_default_handler(uint32_t num,
                                       uint32_t arg1, uint32_t arg2,
                                       uint32_t arg3, uint32_t arg4,
                                       uint32_t arg5) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    printf("syscall: unregistered 0x%x\n", num);
    return IPO_SYSCALL_ENOSYS;
}

static uint32_t syscall_builtin_print(uint32_t num, uint32_t arg1, uint32_t arg2,
                                     uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)num; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    return (uint32_t)printf((const char *)arg1);
}

static uint32_t syscall_builtin_fs_create(uint32_t num, uint32_t arg1, uint32_t arg2,
                                         uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)num; (void)arg3; (void)arg4; (void)arg5;
    return (uint32_t)ipo_fs_create((const char *)arg1, (uint8_t)arg2);
}

static uint32_t syscall_builtin_fs_open(uint32_t num, uint32_t arg1, uint32_t arg2,
                                       uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)num; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    return (uint32_t)ipo_fs_open((const char *)arg1);
}

static uint32_t syscall_builtin_fs_read(uint32_t num, uint32_t arg1, uint32_t arg2,
                                       uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)num; (void)arg5;
    return (uint32_t)ipo_fs_read((int)arg1, (void *)arg2, (uint32_t)arg3, (uint32_t)arg4);
}

static uint32_t syscall_builtin_fs_write(uint32_t num, uint32_t arg1, uint32_t arg2,
                                        uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)num; (void)arg5;
    return (uint32_t)ipo_fs_write((int)arg1, (const void *)arg2, (uint32_t)arg3, (uint32_t)arg4);
}

static uint32_t syscall_builtin_fs_delete(uint32_t num, uint32_t arg1, uint32_t arg2,
                                         uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)num; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    return (uint32_t)(ipo_fs_delete((const char *)arg1) ? 0u : 1u);
}

static uint32_t syscall_builtin_fs_stat(uint32_t num, uint32_t arg1, uint32_t arg2,
                                       uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)num; (void)arg3; (void)arg4; (void)arg5;
    struct ipo_inode *st = (struct ipo_inode *)arg2;
    if (st == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    return (uint32_t)(ipo_fs_stat((const char *)arg1, st) ? 0u : 1u);
}

static uint32_t syscall_builtin_fs_list(uint32_t num, uint32_t arg1, uint32_t arg2,
                                       uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)num; (void)arg4; (void)arg5;
    return (uint32_t)ipo_fs_list_dir((const char *)arg1, (char *)arg2, (int)arg3);
}

static uint32_t syscall_builtin_fs_rename(uint32_t num, uint32_t arg1, uint32_t arg2,
                                         uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)num; (void)arg3; (void)arg4; (void)arg5;
    return (uint32_t)(ipo_fs_rename((const char *)arg1, (const char *)arg2) ? 0u : 1u);
}

static uint32_t syscall_builtin_exec(uint32_t num, uint32_t arg1, uint32_t arg2,
                                    uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)num; (void)arg4; (void)arg5;
    return (uint32_t)process_exec((const char *)arg1, (int)arg2, (char **)arg3);
}

static uint32_t syscall_builtin_async_start(uint32_t num, uint32_t arg1, uint32_t arg2,
                                                   uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)num; (void)arg4; (void)arg5;
    const char *task_name = (const char *)arg1;
    uint32_t interval_ms = arg2 ? arg2 : 10000u;
    void (*task_fn)(void) = (void (*)(void))arg3;
    if (task_fn == NULL) {
        printf("[syscall] async start rejected: null fn for '%s'\n", task_name ? task_name : "(null)");
        return IPO_SYSCALL_ENOSYS;
    }

    int result = async_start_task(task_name, interval_ms, task_fn);
    if (result < 0) {
        printf("[syscall] async start failed for '%s'\n", task_name ? task_name : "(null)");
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)result;
}

static uint32_t syscall_builtin_async_stop(uint32_t num, uint32_t arg1, uint32_t arg2,
                                                  uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    (void)num; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    const char *task_name = (const char *)arg1;

    return (uint32_t)async_stop_task(task_name);
}

void syscall_init(void) {
    memset(ipo_idt_table, 0, sizeof(ipo_idt_table));

    ipo_register_syscall(IPO_SYSCALL_REGISTER, syscall_builtin_register);
    ipo_register_syscall(IPO_SYSCALL_CALL, syscall_builtin_call);
    ipo_register_syscall(IPO_SYSCALL_PRINT, syscall_builtin_print);
    ipo_register_syscall(IPO_SYSCALL_FS_CREATE, syscall_builtin_fs_create);
    ipo_register_syscall(IPO_SYSCALL_FS_OPEN, syscall_builtin_fs_open);
    ipo_register_syscall(IPO_SYSCALL_FS_READ, syscall_builtin_fs_read);
    ipo_register_syscall(IPO_SYSCALL_FS_WRITE, syscall_builtin_fs_write);
    ipo_register_syscall(IPO_SYSCALL_FS_DELETE, syscall_builtin_fs_delete);
    ipo_register_syscall(IPO_SYSCALL_FS_STAT, syscall_builtin_fs_stat);
    ipo_register_syscall(IPO_SYSCALL_FS_LIST, syscall_builtin_fs_list);
    ipo_register_syscall(IPO_SYSCALL_FS_RENAME, syscall_builtin_fs_rename);
    ipo_register_syscall(IPO_SYSCALL_EXEC, syscall_builtin_exec);
    ipo_register_syscall(IPO_SYSCALL_ASYNC_START, syscall_builtin_async_start);
    ipo_register_syscall(IPO_SYSCALL_ASYNC_STOP, syscall_builtin_async_stop);

    ipo_idt_set_gate(0x80, (uint32_t)syscall_isr_entry, IPO_KERNEL_CODE_SEG, IPO_IDT_ENTRY_FLAGS);

    ipo_idt_ptr_t idt_ptr = {
        .limit = (uint16_t)(sizeof(ipo_idt_table) - 1),
        .base = (uint32_t)ipo_idt_table
    };

    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
}

uint32_t syscall_dispatch(uint32_t num,
                         uint32_t arg1, uint32_t arg2,
                         uint32_t arg3, uint32_t arg4,
                         uint32_t arg5) {
    if (ipo_syscall_table == NULL || num >= ipo_syscall_table_size) {
        return IPO_SYSCALL_ENOSYS;
    }

    ipo_syscall_handler_t fn = ipo_syscall_table[num];
    if (fn == NULL) {
        return syscall_default_handler(num, arg1, arg2, arg3, arg4, arg5);
    }

    return fn(num, arg1, arg2, arg3, arg4, arg5);
}

int ipo_syscall(uint32_t num, uint32_t arg1, uint32_t arg2, uint32_t arg3,
                uint32_t arg4, uint32_t arg5) {
    int ret = 0;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3), "S"(arg4), "D"(arg5)
    );
    return ret;
}