#include <syscall.h>
#include <stdio.h>
#include <string.h>
#include <file_system/ipo_fs.h>
#include <kernel/process.h>
#include <memory/kmalloc.h>
#include <kernel/terminal.h>
#include <driver/keyboard.h>
#include <driver/input/keymap/keymap.h>

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

typedef struct {
    char *name;
    void *value;
    uint32_t value_len;
} ipo_shared_var_t;

static ipo_shared_var_t *ipo_shared_vars = NULL;
static uint32_t ipo_shared_var_count = 0u;
static uint32_t ipo_shared_var_capacity = 0u;

extern void syscall_isr_entry(void);

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
        if (new_value == NULL && value_len != 0u) {
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
    if (value_copy == NULL && value_len != 0u) {
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

        ipo_syscall_handler_t *new_table =
            (ipo_syscall_handler_t *)kmalloc(
                new_size * sizeof(ipo_syscall_handler_t));

        if (new_table == NULL) {
            return;
        }

        if (ipo_syscall_table != NULL) {
            memcpy(new_table,
                   ipo_syscall_table,
                   ipo_syscall_table_size * sizeof(ipo_syscall_handler_t));
            kfree(ipo_syscall_table);
        }

        memset(new_table + ipo_syscall_table_size,
               0,
               (new_size - ipo_syscall_table_size) *
                   sizeof(ipo_syscall_handler_t));

        ipo_syscall_table = new_table;
        ipo_syscall_table_size = new_size;
    }

    ipo_syscall_table[num] = handler;
}

static uint32_t syscall_builtin_register(uint32_t num,
                                         uint32_t argc,
                                         uint32_t *argv) {
    (void)num;

    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t syscall_num = argv[0];
    uint32_t handler_ptr = argv[1];

    if (syscall_num >= 0x10000000u) {
        return IPO_SYSCALL_ENOSYS;
    }

    ipo_register_syscall(
        syscall_num,
        (ipo_syscall_handler_t)(uintptr_t)handler_ptr);

    return IPO_SYSCALL_OK;
}

static uint32_t syscall_builtin_call(uint32_t num,
                                     uint32_t argc,
                                     uint32_t *argv) {
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

static uint32_t syscall_default_handler(uint32_t num,
                                        uint32_t argc,
                                        uint32_t *argv) {
    (void)argc;
    (void)argv;

    serial_printf("syscall: unregistered 0x%x\n", num);
    return IPO_SYSCALL_ENOSYS;
}

static uint32_t syscall_builtin_print(uint32_t num,
                                      uint32_t argc,
                                      uint32_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)printf((const char *)(uintptr_t)argv[0]);
}

static uint32_t syscall_builtin_write(uint32_t num,
                                      uint32_t argc,
                                      uint32_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *text = (const char *)(uintptr_t)argv[0];

    if (text == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t count = 0;

    while (text[count] != '\0') {
        putchar(text[count]);
        count++;
    }

    return count;
}

static uint32_t syscall_builtin_fs_create(uint32_t num,
                                          uint32_t argc,
                                          uint32_t *argv) {
    (void)num;

    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)ipo_fs_create(
        (const char *)(uintptr_t)argv[0],
        (uint8_t)argv[1]);
}

static uint32_t syscall_builtin_fs_open(uint32_t num,
                                        uint32_t argc,
                                        uint32_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)ipo_fs_open(
        (const char *)(uintptr_t)argv[0]);
}

static uint32_t syscall_builtin_fs_read(uint32_t num,
                                        uint32_t argc,
                                        uint32_t *argv) {
    (void)num;

    if (argc < 4u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)ipo_fs_read(
        (int)argv[0],
        (void *)(uintptr_t)argv[1],
        (uint32_t)argv[2],
        (uint32_t)argv[3]);
}

static uint32_t syscall_builtin_fs_write(uint32_t num,
                                         uint32_t argc,
                                         uint32_t *argv) {
    (void)num;

    if (argc < 4u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)ipo_fs_write(
        (int)argv[0],
        (const void *)(uintptr_t)argv[1],
        (uint32_t)argv[2],
        (uint32_t)argv[3]);
}

static uint32_t syscall_builtin_fs_delete(uint32_t num,
                                          uint32_t argc,
                                          uint32_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)(
        ipo_fs_delete((const char *)(uintptr_t)argv[0]) ? 0u : 1u);
}

static uint32_t syscall_builtin_fs_stat(uint32_t num,
                                        uint32_t argc,
                                        uint32_t *argv) {
    (void)num;

    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    struct ipo_inode *st =
        (struct ipo_inode *)(uintptr_t)argv[1];

    if (st == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)(
        ipo_fs_stat((const char *)(uintptr_t)argv[0], st) ? 0u : 1u);
}

static uint32_t syscall_builtin_fs_list(uint32_t num,
                                        uint32_t argc,
                                        uint32_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)ipo_fs_list_dir(
        (const char *)(uintptr_t)argv[0],
        (char *)(uintptr_t)argv[1],
        (int)argv[2]);
}

static uint32_t syscall_builtin_fs_rename(uint32_t num,
                                          uint32_t argc,
                                          uint32_t *argv) {
    (void)num;

    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)(
        ipo_fs_rename(
            (const char *)(uintptr_t)argv[0],
            (const char *)(uintptr_t)argv[1]) ? 0u : 1u);
}

static uint32_t syscall_builtin_exec(uint32_t num,
                                     uint32_t argc,
                                     uint32_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)process_exec(
        (const char *)(uintptr_t)argv[0],
        (int)argv[1],
        (char **)(uintptr_t)argv[2]);
}

static uint32_t syscall_builtin_read(uint32_t num,
                                     uint32_t argc,
                                     uint32_t *argv) {
    (void)num;

    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    char *buffer = (char *)(uintptr_t)argv[0];
    uint32_t max_len = argv[1];

    if (buffer == NULL || max_len == 0u) {
        return IPO_SYSCALL_ENOSYS;
    }

    keyboard_flush_queue();

    uint32_t len = 0u;

    while (len + 1u < max_len) {
        uint8_t scancode = keyboard_wait_scancode();

        if (scancode == 0x00u) {
            continue;
        }

        if (scancode & 0x80u) {
            continue;
        }

        char ch = get_char(scancode);

        if (ch == 0x00) {
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            putchar('\n');
            buffer[len] = '\0';
            return len;
        }

        if (ch == '\b' || ch == 127) {
            if (len > 0u) {
                len--;
                buffer[len] = '\0';
                putchar('\b');
                putchar(' ');
                putchar('\b');
            }

            continue;
        }

        if (ch >= 32 && ch < 127) {
            buffer[len++] = ch;
            putchar(ch);
        }
    }

    buffer[len] = '\0';

    return len;
}

static uint32_t syscall_builtin_async_start(uint32_t num,
                                            uint32_t argc,
                                            uint32_t *argv) {
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

static uint32_t syscall_builtin_async_stop(uint32_t num,
                                           uint32_t argc,
                                           uint32_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *task_name =
        (const char *)(uintptr_t)argv[0];

    return (uint32_t)async_stop_task(task_name);
}

static uint32_t syscall_builtin_var_set(uint32_t num,
                                       uint32_t argc,
                                       uint32_t *argv) {
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

static uint32_t syscall_builtin_var_get(uint32_t num,
                                       uint32_t argc,
                                       uint32_t *argv) {
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

static uint32_t syscall_builtin_var_delete(uint32_t num,
                                          uint32_t argc,
                                          uint32_t *argv) {
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

static uint32_t syscall_builtin_stack_grow(uint32_t num,
                                          uint32_t argc,
                                          uint32_t *argv) {
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

static uint32_t syscall_builtin_stack_shrink(uint32_t num,
                                            uint32_t argc,
                                            uint32_t *argv) {
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

void syscall_init(void) {
    memset(ipo_idt_table, 0, sizeof(ipo_idt_table));

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
        IPO_SYSCALL_FS_CREATE,
        syscall_builtin_fs_create);

    ipo_register_syscall(
        IPO_SYSCALL_FS_OPEN,
        syscall_builtin_fs_open);

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
        IPO_SYSCALL_VAR_SET,
        syscall_builtin_var_set);

    ipo_register_syscall(
        IPO_SYSCALL_VAR_GET,
        syscall_builtin_var_get);

    ipo_register_syscall(
        IPO_SYSCALL_VAR_DELETE,
        syscall_builtin_var_delete);

    ipo_idt_set_gate(
        0x80,
        (uint32_t)syscall_isr_entry,
        IPO_KERNEL_CODE_SEG,
        IPO_IDT_ENTRY_FLAGS);

    ipo_idt_ptr_t idt_ptr = {
        .limit = (uint16_t)(sizeof(ipo_idt_table) - 1),
        .base = (uint32_t)ipo_idt_table
    };

    __asm__ volatile(
        "lidt %0"
        :
        : "m"(idt_ptr));
}

uint32_t syscall_dispatch(uint32_t num,
                          uint32_t argc,
                          uint32_t *argv)
{
    if (ipo_syscall_table == NULL ||
        num >= ipo_syscall_table_size) {
        return IPO_SYSCALL_ENOSYS;
    }

    ipo_syscall_handler_t fn =
        ipo_syscall_table[num];

    if (fn == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return fn(num, argc, argv);
}

int ipo_syscall(uint32_t num,
                uint32_t argc,
                uint32_t *argv)
{
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