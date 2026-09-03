#include <syscall.h>
#include <stdio.h>
#include <string.h>
#include <file_system/ipo_fs.h>
#include <kernel/process.h>
#include <memory/kmalloc.h>
#include <kernel/terminal.h>
#include <driver/input/keyboard.h>
#include <driver/input/keymap/keymap.h>
#include <driver/input/keymap/dynamic_keymap.h>
#include <kernel/driver.h>
#include <system/state.h>
#include <system/timer.h>
#include <vga.h>

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

static uint32_t syscall_builtin_terminal_input(uint32_t num,
                                               uint32_t argc,
                                               uint32_t *argv) {
    (void)num;

    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *text = (const char *)(uintptr_t)argv[0];
    int auto_exec = (int)argv[1];
    terminal_inject_input(text, auto_exec != 0);
    return IPO_SYSCALL_OK;
}

static uint32_t syscall_builtin_keymap_set(uint32_t num,
                                           uint32_t argc,
                                           uint32_t *argv) {
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

static uint32_t syscall_builtin_keymap_get(uint32_t num,
                                           uint32_t argc,
                                           uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)(uintptr_t)dynamic_keymap_get_name();
}

static uint32_t syscall_builtin_font_load(uint32_t num,
                                          uint32_t argc,
                                          uint32_t *argv) {
    (void)num;
    const char *path = (argc >= 1u && argv != NULL) ? (const char *)(uintptr_t)argv[0] : NULL;
    int res = vga_load_cyrillic_font(path);
    return (res == 0) ? IPO_SYSCALL_OK : IPO_SYSCALL_ENOSYS;
}

static uint32_t syscall_builtin_keymap_disable(uint32_t num,
                                              uint32_t argc,
                                              uint32_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return IPO_SYSCALL_ENOSYS;
    const char *target = (const char *)(uintptr_t)argv[0];
    return (uint32_t)dynamic_keymap_disable(target);
}

static uint32_t syscall_builtin_keymap_enable(uint32_t num,
                                             uint32_t argc,
                                             uint32_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return IPO_SYSCALL_ENOSYS;
    const char *target = (const char *)(uintptr_t)argv[0];
    return (uint32_t)dynamic_keymap_enable(target);
}

static uint32_t syscall_builtin_keymap_remove(uint32_t num,
                                             uint32_t argc,
                                             uint32_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return IPO_SYSCALL_ENOSYS;
    const char *target = (const char *)(uintptr_t)argv[0];
    return (uint32_t)dynamic_keymap_remove(target);
}

static uint32_t syscall_builtin_driver_register(uint32_t num,
                                                uint32_t argc,
                                                uint32_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    driver_t *drv = (driver_t *)(uintptr_t)argv[0];
    int res = driver_register(drv);
    return (uint32_t)res;
}

static uint32_t syscall_builtin_driver_unregister(uint32_t num,
                                                  uint32_t argc,
                                                  uint32_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    const char *name = (const char *)(uintptr_t)argv[0];
    int res = driver_unregister(name);
    return (uint32_t)res;
}

static uint32_t syscall_builtin_driver_list(uint32_t num,
                                            uint32_t argc,
                                            uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    driver_print_list();
    return IPO_SYSCALL_OK;
}

static size_t syscall_read_visual_offset(const char *buf, uint32_t len, uint32_t index) {
    size_t vcol = 0;
    for (uint32_t i = 0; i < index && i < len; i++) {
        unsigned char uc = (unsigned char)buf[i];
        if ((uc & 0xC0) == 0x80) {
            continue; /* Skip UTF-8 continuation bytes */
        }
        if (buf[i] == '\t') {
            size_t tab_spaces = 4 - (vcol % 4);
            if (tab_spaces == 0) tab_spaces = 4;
            vcol += tab_spaces;
        } else {
            vcol += 1;
        }
    }
    return vcol;
}

static void syscall_read_render(int32_t *start_cursor_ptr, int32_t start_origin, const char *buf, uint32_t len, uint32_t prev_len, uint32_t cursor_pos) {
    (void)start_origin;
    size_t vis_cursor = syscall_read_visual_offset(buf, len, cursor_pos);
    size_t vis_len = syscall_read_visual_offset(buf, len, len);
    size_t vis_prev = syscall_read_visual_offset(buf, len, prev_len);

    /* If cursor or text tail exceeds bottom of screen, auto-scroll */
    while (*start_cursor_ptr + (int32_t)vis_cursor >= VGA_WIDTH * VGA_HEIGHT) {
        terminal_auto_scroll();
        *start_cursor_ptr -= VGA_WIDTH;
    }
    while (*start_cursor_ptr + (int32_t)vis_len >= VGA_WIDTH * VGA_HEIGHT) {
        terminal_auto_scroll();
        *start_cursor_ptr -= VGA_WIDTH;
    }

    volatile uint16_t *vga = VGA_MEMORY;

    /* Draw text */
    size_t vcol = 0;
    size_t i = 0;
    while (i < len) {
        if (buf[i] == '\t') {
            size_t tab_spaces = 4 - (vcol % 4);
            if (tab_spaces == 0) tab_spaces = 4;
            for (size_t s = 0; s < tab_spaces; s++) {
                int32_t off = *start_cursor_ptr + (int32_t)(vcol + s);
                if (off >= VGA_START_CURSOR_POSITION && off < VGA_WIDTH * VGA_HEIGHT) {
                    vga[off] = vga_entry(' ', VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                }
            }
            vcol += tab_spaces;
            i++;
        } else {
            size_t bytes = 1;
            uint8_t glyph = utf8_to_vga_glyph(&buf[i], len - i, &bytes);

            int32_t off = *start_cursor_ptr + (int32_t)vcol;
            if (off >= VGA_START_CURSOR_POSITION && off < VGA_WIDTH * VGA_HEIGHT) {
                vga[off] = vga_entry(glyph, VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            }
            vcol += 1;
            i += bytes;
        }
    }

    /* Erase old tail with spaces */
    for (size_t i = vcol; i < vis_prev; i++) {
        int32_t off = *start_cursor_ptr + (int32_t)i;
        if (off >= VGA_START_CURSOR_POSITION && off < VGA_WIDTH * VGA_HEIGHT) {
            vga[off] = vga_entry(' ', VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        }
    }

    /* Position cursor */
    int32_t cur = *start_cursor_ptr + (int32_t)vis_cursor;
    if (cur < VGA_START_CURSOR_POSITION) cur = VGA_START_CURSOR_POSITION;
    if (cur >= VGA_WIDTH * VGA_HEIGHT) cur = VGA_WIDTH * VGA_HEIGHT - 1;
    vga_set_cursor((uint16_t)cur);
    vga_show_cursor();
}

static uint32_t syscall_builtin_read(uint32_t num,
                                     uint32_t argc,
                                     uint32_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t max_len = argv[1];
    char **out_ptr = NULL;
    char *buffer = NULL;
    uint32_t capacity = 0u;

    if (max_len == 0u) {
        out_ptr = (char **)(uintptr_t)argv[0];
        if (out_ptr == NULL) {
            return IPO_SYSCALL_ENOSYS;
        }
        capacity = 256u;
        buffer = (char *)kmalloc(capacity);
        if (buffer == NULL) {
            return IPO_SYSCALL_ENOSYS;
        }
    } else {
        buffer = (char *)(uintptr_t)argv[0];
        if (buffer == NULL) {
            return IPO_SYSCALL_ENOSYS;
        }
        capacity = max_len;
    }

    process_t *proc = process_get_current();
    uint32_t pid = proc ? proc->pid : 0u;

    system_state_t prev_state = system_get_state();
    system_set_state(SYSTEM_STATE_TEXT_INPUT);
    keyboard_set_app_input_mode(true);

    serial_printf("[syscall_read] started: pid=%u max_len=%u (state=TEXT_INPUT)\n", pid, max_len);

    uint32_t len = 0u;
    uint32_t cursor_pos = 0u;
    int32_t start_origin = (int32_t)vga_get_cursor_position();
    if (start_origin < VGA_START_CURSOR_POSITION || start_origin >= VGA_WIDTH * VGA_HEIGHT) {
        start_origin = VGA_START_CURSOR_POSITION;
        vga_set_cursor((uint16_t)start_origin);
    }
    int32_t start_cursor = start_origin;
    buffer[0] = '\0';

    for (;;) {
        uint8_t scancode = keyboard_wait_scancode();

        if (scancode == 0x00u) {
            continue;
        }

        update_hot_key_state(scancode);

        /* 1. Dispatch application hotkeys (highest priority) and system hotkeys */
        if (keyboard_dispatch_hotkey(scancode)) {
            continue;
        }

        if (scancode & 0x80u) {
            continue;
        }

        /* 2. Default Ctrl+C: cancel active line input */
        if (keyboard_is_ctrl_pressed() && scancode == 0x2E) {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            size_t vis_len = syscall_read_visual_offset(buffer, len, len);
            int32_t cur = start_cursor + (int32_t)vis_len;
            if (cur < VGA_START_CURSOR_POSITION) cur = VGA_START_CURSOR_POSITION;
            if (cur >= VGA_WIDTH * VGA_HEIGHT) cur = VGA_WIDTH * VGA_HEIGHT - 1;
            vga_set_cursor((uint16_t)cur);
            printf("^C\n");
            /* Signal interrupt to kernel and return special code */
            system_request_interrupt();
            keyboard_set_app_input_mode(false);
            system_set_state(proc ? SYSTEM_STATE_PROCESS_RUNNING : prev_state);
            if (max_len == 0u && out_ptr != NULL) {
                kfree(buffer);
                *out_ptr = NULL;
            }
            return (uint32_t)(-2); /* IPO_SYSCALL_EINTR */
        }

        /* Navigation and Scrolling keys */
        if (scancode == 0x49) { /* Page Up */
            for (int i = 0; i < 5 && terminal_get_top_buffer_count() > 0; i++) {
                terminal_scroll_up();
                start_cursor += VGA_WIDTH;
            }
            continue;
        }
        if (scancode == 0x51) { /* Page Down */
            for (int i = 0; i < 5 && terminal_get_bottom_buffer_count() > 0; i++) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            continue;
        }
        if (scancode == 0x48) { /* Up arrow */
            if (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_up();
                start_cursor += VGA_WIDTH;
                continue;
            }
            if (cursor_pos >= VGA_WIDTH) {
                cursor_pos -= VGA_WIDTH;
                syscall_read_render(&start_cursor, start_origin, buffer, len, len, cursor_pos);
            } else if (cursor_pos > 0) {
                cursor_pos = 0u;
                syscall_read_render(&start_cursor, start_origin, buffer, len, len, cursor_pos);
            }
            continue;
        }
        if (scancode == 0x50) { /* Down arrow */
            if (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
                continue;
            }
            if (cursor_pos + VGA_WIDTH <= len) {
                cursor_pos += VGA_WIDTH;
                syscall_read_render(&start_cursor, start_origin, buffer, len, len, cursor_pos);
            } else if (cursor_pos < len) {
                cursor_pos = len;
                syscall_read_render(&start_cursor, start_origin, buffer, len, len, cursor_pos);
            }
            continue;
        }
        if (scancode == 0x4B) { /* Left arrow */
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            if (cursor_pos > 0u) {
                cursor_pos--;
                syscall_read_render(&start_cursor, start_origin, buffer, len, len, cursor_pos);
            }
            continue;
        }
        if (scancode == 0x4D) { /* Right arrow */
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            if (cursor_pos < len) {
                cursor_pos++;
                syscall_read_render(&start_cursor, start_origin, buffer, len, len, cursor_pos);
            }
            continue;
        }
        if (scancode == 0x47) { /* Home */
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            cursor_pos = 0u;
            syscall_read_render(&start_cursor, start_origin, buffer, len, len, cursor_pos);
            continue;
        }
        if (scancode == 0x4F) { /* End */
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            cursor_pos = len;
            syscall_read_render(&start_cursor, start_origin, buffer, len, len, cursor_pos);
            continue;
        }
        if (scancode == 0x53) { /* Delete */
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            if (cursor_pos < len) {
                uint32_t old_len = len;
                memmove(&buffer[cursor_pos], &buffer[cursor_pos + 1], len - cursor_pos);
                len--;
                buffer[len] = '\0';
                syscall_read_render(&start_cursor, start_origin, buffer, len, old_len, cursor_pos);
            }
            continue;
        }

        char ch = get_char(scancode);

        if (ch == '\r' || ch == '\n') {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            size_t vis_len = syscall_read_visual_offset(buffer, len, len);
            int32_t cur = start_cursor + (int32_t)vis_len;
            if (cur < VGA_START_CURSOR_POSITION) cur = VGA_START_CURSOR_POSITION;
            if (cur >= VGA_WIDTH * VGA_HEIGHT) cur = VGA_WIDTH * VGA_HEIGHT - 1;
            vga_set_cursor((uint16_t)cur);
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
                uint32_t del_bytes = 1;
                while (cursor_pos >= del_bytes + 1u &&
                       ((unsigned char)buffer[cursor_pos - del_bytes] & 0xC0) == 0x80) {
                    del_bytes++;
                }
                uint32_t old_len = len;
                memmove(&buffer[cursor_pos - del_bytes], &buffer[cursor_pos], len - cursor_pos + 1);
                cursor_pos -= del_bytes;
                len -= del_bytes;
                buffer[len] = '\0';
                syscall_read_render(&start_cursor, start_origin, buffer, len, old_len, cursor_pos);
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
                    uint32_t new_cap = (capacity + 16u) * 2u;
                    char *new_buf = (char *)kmalloc(new_cap);
                    if (new_buf != NULL) {
                        memcpy(new_buf, buffer, len + 1);
                        kfree(buffer);
                        buffer = new_buf;
                        capacity = new_cap;
                    }
                }
            } else {
                if (len + 1u >= capacity) continue;
            }

            uint32_t old_len = len;
            memmove(&buffer[cursor_pos + 1], &buffer[cursor_pos], len - cursor_pos + 1);
            buffer[cursor_pos] = '\t';
            cursor_pos++;
            len++;
            buffer[len] = '\0';
            syscall_read_render(&start_cursor, start_origin, buffer, len, old_len, cursor_pos);
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
                if (len + slen >= capacity) {
                    uint32_t new_cap = (capacity + slen + 32u) * 2u;
                    char *new_buf = (char *)kmalloc(new_cap);
                    if (new_buf != NULL) {
                        memcpy(new_buf, buffer, len + 1);
                        kfree(buffer);
                        buffer = new_buf;
                        capacity = new_cap;
                    }
                }
            } else {
                if (len + slen >= capacity) {
                    continue;
                }
            }

            uint32_t old_len = len;
            memmove(&buffer[cursor_pos + slen], &buffer[cursor_pos], len - cursor_pos + 1);
            memcpy(&buffer[cursor_pos], out_str, slen);
            cursor_pos += (uint32_t)slen;
            len += (uint32_t)slen;
            buffer[len] = '\0';
            syscall_read_render(&start_cursor, start_origin, buffer, len, old_len, cursor_pos);
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
        IPO_SYSCALL_TERMINAL_INPUT,
        syscall_builtin_terminal_input);

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
        IPO_SYSCALL_FONT_LOAD,
        syscall_builtin_font_load);

    ipo_register_syscall(
        IPO_SYSCALL_DRIVER_REGISTER,
        syscall_builtin_driver_register);

    ipo_register_syscall(
        IPO_SYSCALL_DRIVER_UNREGISTER,
        syscall_builtin_driver_unregister);

    ipo_register_syscall(
        IPO_SYSCALL_DRIVER_LIST,
        syscall_builtin_driver_list);

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