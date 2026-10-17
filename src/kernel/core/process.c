#include <kernel/process.h>
#include <kernel/async.h>
#include <kernel/terminal.h>
#include <system/state.h>
#include <file_system/ipo_fs.h>
#include <memory/kmalloc.h>
#include <memory/memory_map.h>
#include <driver/input/keymap/keymap.h>
#include <driver/input/keymap/dynamic_keymap.h>
#include <driver/input/keyboard.h>
#include <vga.h>
#include <vga_gfx.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <wm.h>
#include <syscall.h>
#include <driver/input/mouse.h>
#include <driver/audio_core.h>
#include <ioport.h>


/**
 * Memory allocation tracker for process binaries
 */
typedef struct {
    void *base;             // Base address of allocated block
    uint32_t size;          // Size of allocated block
    uint32_t pid;           // Owner process ID (0 = free)
} memory_block_t;

// Global variables
static int last_exit_code = 0;
static process_return_value_t last_return_val;
static process_return_value_t *last_batch_return_vals = NULL;
static int last_batch_return_vals_cap = 0;
static process_t *current_process = NULL;
static process_t *process_list = NULL;

void process_return_val_init(process_return_value_t *val) {
    if (!val) return;
    memset(val, 0, sizeof(process_return_value_t));
}

void process_return_val_free_fields(process_return_value_t *val) {
    if (!val) return;
    if (val->type_name) {
        kfree(val->type_name);
        val->type_name = NULL;
    }
    val->type_name_len = 0;
    val->type_name_cap = 0;

    if (val->str_val) {
        kfree(val->str_val);
        val->str_val = NULL;
    }
    val->str_val_len = 0;
    val->str_val_cap = 0;

    if (val->struct_data) {
        kfree(val->struct_data);
        val->struct_data = NULL;
    }
    val->struct_size = 0;
    val->struct_data_cap = 0;

    if (val->arg_vals) {
        for (uint32_t i = 0; i < val->arg_count; i++) {
            if (val->arg_vals[i]) kfree(val->arg_vals[i]);
        }
        kfree(val->arg_vals);
        val->arg_vals = NULL;
    }
    val->arg_count = 0;
    val->arg_cap = 0;
}

void process_return_val_reset(process_return_value_t *val) {
    if (!val) return;
    process_return_val_free_fields(val);
    memset(val, 0, sizeof(process_return_value_t));
}

void process_return_val_set_type_name(process_return_value_t *val, const char *name) {
    if (!val) return;
    if (!name || name[0] == '\0') {
        if (val->type_name) val->type_name[0] = '\0';
        val->type_name_len = 0;
        return;
    }
    uint32_t len = (uint32_t)strlen(name);
    if (len + 1 > val->type_name_cap) {
        uint32_t new_cap = (val->type_name_cap == 0) ? 32 : (val->type_name_cap * 2);
        while (new_cap < len + 1) new_cap *= 2;
        char *new_buf = (char *)kmalloc(new_cap);
        if (!new_buf) return;
        if (val->type_name) kfree(val->type_name);
        val->type_name = new_buf;
        val->type_name_cap = new_cap;
    }
    memcpy(val->type_name, name, len + 1);
    val->type_name_len = len;
}

void process_return_val_set_str(process_return_value_t *val, const char *str) {
    if (!val) return;
    if (!str) {
        if (val->str_val) val->str_val[0] = '\0';
        val->str_val_len = 0;
        return;
    }
    uint32_t len = (uint32_t)strlen(str);
    if (len + 1 > val->str_val_cap) {
        uint32_t new_cap = (val->str_val_cap == 0) ? 64 : (val->str_val_cap * 2);
        while (new_cap < len + 1) new_cap *= 2;
        char *new_buf = (char *)kmalloc(new_cap);
        if (!new_buf) return;
        if (val->str_val) kfree(val->str_val);
        val->str_val = new_buf;
        val->str_val_cap = new_cap;
    }
    memcpy(val->str_val, str, len + 1);
    val->str_val_len = len;
}

void process_return_val_append_str(process_return_value_t *val, const char *str) {
    if (!val || !str || str[0] == '\0') return;
    uint32_t slen = (uint32_t)strlen(str);
    uint32_t needed = val->str_val_len + slen + 1;
    if (needed > val->str_val_cap) {
        uint32_t new_cap = (val->str_val_cap == 0) ? 64 : (val->str_val_cap * 2);
        while (new_cap < needed) new_cap *= 2;
        char *new_buf = (char *)kmalloc(new_cap);
        if (!new_buf) return;
        if (val->str_val) {
            memcpy(new_buf, val->str_val, val->str_val_len);
            kfree(val->str_val);
        } else {
            val->str_val_len = 0;
        }
        val->str_val = new_buf;
        val->str_val_cap = new_cap;
    }
    memcpy(val->str_val + val->str_val_len, str, slen + 1);
    val->str_val_len += slen;
}

void process_return_val_set_struct_data(process_return_value_t *val, const void *data, uint32_t size) {
    if (!val) return;
    if (!data || size == 0) {
        val->struct_size = 0;
        return;
    }
    if (size > val->struct_data_cap) {
        uint32_t new_cap = (val->struct_data_cap == 0) ? 64 : (val->struct_data_cap * 2);
        while (new_cap < size) new_cap *= 2;
        uint8_t *new_buf = (uint8_t *)kmalloc(new_cap);
        if (!new_buf) return;
        if (val->struct_data) kfree(val->struct_data);
        val->struct_data = new_buf;
        val->struct_data_cap = new_cap;
    }
    memcpy(val->struct_data, data, size);
    val->struct_size = size;
}

void process_return_val_add_arg(process_return_value_t *val, const char *arg) {
    if (!val || !arg) return;
    if (val->arg_count + 1 > val->arg_cap) {
        uint32_t new_cap = (val->arg_cap == 0) ? 4 : (val->arg_cap * 2);
        char **new_arr = (char **)kmalloc(new_cap * sizeof(char *));
        if (!new_arr) return;
        for (uint32_t i = 0; i < val->arg_count; i++) {
            new_arr[i] = val->arg_vals[i];
        }
        if (val->arg_vals) kfree(val->arg_vals);
        val->arg_vals = new_arr;
        val->arg_cap = new_cap;
    }
    uint32_t len = (uint32_t)strlen(arg);
    char *copy = (char *)kmalloc(len + 1);
    if (!copy) return;
    memcpy(copy, arg, len + 1);
    val->arg_vals[val->arg_count++] = copy;
}

void process_return_val_copy(process_return_value_t *dst, const process_return_value_t *src) {
    if (!dst || !src) return;
    process_return_val_free_fields(dst);
    dst->type = src->type;
    dst->int_val = src->int_val;
    dst->raw_eax = src->raw_eax;
    dst->raw_edx = src->raw_edx;
    if (src->type_name && src->type_name_len > 0) {
        process_return_val_set_type_name(dst, src->type_name);
    }
    if (src->str_val && src->str_val_len > 0) {
        process_return_val_set_str(dst, src->str_val);
    }
    if (src->struct_data && src->struct_size > 0) {
        process_return_val_set_struct_data(dst, src->struct_data, src->struct_size);
    }
    for (uint32_t i = 0; i < src->arg_count; i++) {
        process_return_val_add_arg(dst, src->arg_vals[i]);
    }
}

/* Cooperative multitasking scheduler state */
static process_t **batch_procs = NULL;
static int batch_proc_count = 0;
static int batch_current_idx = 0;
static uint32_t scheduler_esp = 0;
static bool batch_active = false;
static bool batch_separate_windows = false;
static process_t *batch_foreground_proc = NULL;
static process_t *batch_displayed_fg = NULL;
static process_t *currently_mapped_app = NULL;

static void process_sync_shared_context(void) {
    process_shared_ctx_t *ctx = process_get_shared_context();
    ctx->magic = PROCESS_SHARED_MAGIC;
    ctx->current_process = current_process;
    ctx->batch_foreground_proc = batch_foreground_proc;
    ctx->batch_displayed_fg = batch_displayed_fg;
    ctx->batch_active = batch_active;
    ctx->batch_separate_windows = batch_separate_windows;
}

void process_map_app(process_t *proc) {
    if (currently_mapped_app == proc) {
        return;
    }

    if (currently_mapped_app != NULL && currently_mapped_app->binary_storage != NULL) {
        // Save previous app's modified data/bss
        uint32_t off = currently_mapped_app->data_offset;
        uint32_t sz = currently_mapped_app->data_size;
        if (sz == 0 || off + sz > currently_mapped_app->binary_size) {
            off = 0; sz = currently_mapped_app->binary_size;
        }
        memcpy((uint8_t *)currently_mapped_app->binary_storage + off,
               (uint8_t *)PROCESS_HEAP_START + off, sz);
    }

    if (proc != NULL && proc->binary_storage != NULL) {
        // If code has not been loaded into 0x800000 or different executable name, load full image
        if (!proc->code_mapped || currently_mapped_app == NULL ||
            (currently_mapped_app->name && proc->name && strcmp(currently_mapped_app->name, proc->name) != 0)) {
            memcpy((void *)PROCESS_HEAP_START, proc->binary_storage, proc->binary_size);
            proc->code_mapped = true;
        } else {
            // Same executable image (e.g. edit && edit or edit & edit): code is already in place, only swap data/bss!
            uint32_t off = proc->data_offset;
            uint32_t sz = proc->data_size;
            if (sz == 0 || off + sz > proc->binary_size) {
                off = 0; sz = proc->binary_size;
            }
            memcpy((uint8_t *)PROCESS_HEAP_START + off,
                   (uint8_t *)proc->binary_storage + off, sz);
        }
    }

    currently_mapped_app = proc;
}

process_t *process_get_mapped_app(void) {
    return currently_mapped_app;
}

__attribute__((naked)) static void process_switch_context(uint32_t *old_esp, uint32_t new_esp) {
    __asm__ volatile(
        "pushl %ebp\n\t"
        "pushl %ebx\n\t"
        "pushl %esi\n\t"
        "pushl %edi\n\t"
        "pushfl\n\t"
        "movl 24(%esp), %eax\n\t"
        "movl 28(%esp), %edx\n\t"
        "movl %esp, (%eax)\n\t"
        "movl %edx, %esp\n\t"
        "popfl\n\t"
        "popl %edi\n\t"
        "popl %esi\n\t"
        "popl %ebx\n\t"
        "popl %ebp\n\t"
        "ret\n\t"
    );
}

static bool in_process_context = false;

bool process_in_process_context(void) {
    return in_process_context;
}

void process_yield_kernel(void) {
    if (!batch_active || !in_process_context || !current_process) {
        return;
    }

    process_t *fg = process_get_foreground();
    if (current_process != fg && !wm_session_active()) {
        io_wait();
    }

    in_process_context = false;
    process_switch_context(&current_process->stack_ptr, scheduler_esp);
    in_process_context = true;
}

bool process_is_active_or_has_windows(process_t *p) {
    if (!p) return false;
    if (p->is_running) return true;
    if (p->is_wm_app || p->wants_graphics) {
        if (p->completed_and_acknowledged) return false;
        if (wm_session_active()) return true;
        return (p->pid != 0 && wm_has_windows_for_pid(p->pid));
    }
    if (batch_separate_windows) {
        if (p->text_vram_backup != NULL && !p->completed_and_acknowledged) return true;
    }
    return false;
}

process_t *process_get_displayed_foreground(void) {
    process_shared_ctx_t *ctx = process_get_shared_context();
    if (ctx && ctx->magic == PROCESS_SHARED_MAGIC && ctx->batch_active) {
        if (ctx->batch_displayed_fg && process_is_active_or_has_windows(ctx->batch_displayed_fg)) {
            return ctx->batch_displayed_fg;
        }
    }
    if (batch_active && batch_displayed_fg && process_is_active_or_has_windows(batch_displayed_fg)) {
        return batch_displayed_fg;
    }
    return process_get_foreground();
}

process_t *process_get_foreground(void) {
    process_shared_ctx_t *ctx = process_get_shared_context();
    if (ctx && ctx->magic == PROCESS_SHARED_MAGIC) {
        if (!ctx->batch_active) {
            return ctx->current_process ? ctx->current_process : current_process;
        }
        if (ctx->batch_foreground_proc && process_is_active_or_has_windows(ctx->batch_foreground_proc)) {
            return ctx->batch_foreground_proc;
        }
    }
    if (!batch_active) {
        return current_process;
    }
    if (batch_foreground_proc && process_is_active_or_has_windows(batch_foreground_proc)) {
        return batch_foreground_proc;
    }
    for (int i = 0; i < batch_proc_count; i++) {
        if (batch_procs && batch_procs[i] && process_is_active_or_has_windows(batch_procs[i])) {
            return batch_procs[i];
        }
    }
    return NULL;
}

static void process_switch_workspace_screen(process_t *old_fg, process_t *new_fg) {
    if (old_fg == new_fg) return;
    if (!process_get_separate_windows()) return;

    /* If both processes belong to the same workspace, they share the same screen.
       Do not switch hardware modes or swap VRAM buffers. */
    if (old_fg && new_fg && old_fg->window_id == new_fg->window_id) {
        batch_displayed_fg = new_fg;
        process_shared_ctx_t *ctx = process_get_shared_context();
        if (ctx && ctx->magic == PROCESS_SHARED_MAGIC) {
            ctx->batch_displayed_fg = new_fg;
        }
        process_sync_shared_context();
        return;
    }

    serial_printf("[proc] switch_workspace_screen: %s -> %s\n",
                  old_fg ? old_fg->name : "none",
                  new_fg ? new_fg->name : "none");

    keyboard_clear_key_state();
    keyboard_flush_hardware();
    keyboard_flush_queue();
    keyboard_flush_app_queue();

    /* 1. Preserve outgoing graphics or text screen state */
    if (old_fg) {
        if (old_fg->wants_graphics || old_fg->is_wm_app || vga_is_graphics_mode()) {
            if (!old_fg->gfx_vram_backup) {
                old_fg->gfx_vram_backup = (uint8_t *)kmalloc(VGA_GFX_SIZE);
            }
            if (old_fg->gfx_vram_backup) {
                memcpy(old_fg->gfx_vram_backup, (const void *)VGA_GFX_VRAM_ADDR, VGA_GFX_SIZE);
            }
            if (!old_fg->gfx_palette_backup) {
                old_fg->gfx_palette_backup = (uint8_t *)kmalloc(768);
            }
            if (old_fg->gfx_palette_backup) {
                for (int i = 0; i < 256; i++) {
                    outb(0x3C7, (uint8_t)i);
                    old_fg->gfx_palette_backup[i * 3 + 0] = inb(0x3C9);
                    old_fg->gfx_palette_backup[i * 3 + 1] = inb(0x3C9);
                    old_fg->gfx_palette_backup[i * 3 + 2] = inb(0x3C9);
                }
            }
        } else {
            if (old_fg->scroll_bottom_count > 0) {
                terminal_return_to_present();
            }
            if (old_fg->text_vram_backup) {
                memcpy(old_fg->text_vram_backup, (const void *)0xB8000, 80 * 25 * sizeof(uint16_t));
                old_fg->text_cursor_pos = vga_get_cursor_position();
                old_fg->text_cursor_visible = vga_is_cursor_visible();
            }
        }
    }

    /* Publish the new workspace before callbacks such as wm_set_focus can
       call process_set_foreground() again. */
    process_shared_ctx_t *ctx = process_get_shared_context();
    if (ctx && ctx->magic == PROCESS_SHARED_MAGIC) {
        ctx->batch_displayed_fg = new_fg;
    }
    batch_displayed_fg = new_fg;
    process_sync_shared_context();

    /* 2. Restore incoming graphics or text screen state */
    if (new_fg == NULL) {
        keyboard_set_app_input_mode(false);
        if (vga_is_graphics_mode()) {
            vga_set_mode_text_hardware();
            dynamic_keymap_reapply_fonts();
            mouse_set_bounds_from_display();
        }
    } else {
        new_fg->waiting_for_input = false;
        keyboard_set_app_input_mode(true);
        if (new_fg->is_wm_app) {
            vga_set_mode_13h_hardware();
            vga_gfx_init_default_palette();
            mouse_set_bounds_from_display();
            wm_focus_window_for_pid(new_fg->pid);
            wm_invalidate_all();
            wm_compositor_tick();
        } else if (new_fg->wants_graphics) {
            vga_set_mode_13h_hardware();
            if (new_fg->gfx_palette_backup) {
                for (int i = 0; i < 256; i++) {
                    outb(0x3C8, (uint8_t)i);
                    outb(0x3C9, new_fg->gfx_palette_backup[i * 3 + 0]);
                    outb(0x3C9, new_fg->gfx_palette_backup[i * 3 + 1]);
                    outb(0x3C9, new_fg->gfx_palette_backup[i * 3 + 2]);
                }
            } else {
                vga_gfx_init_default_palette();
            }
            mouse_set_bounds_from_display();
            if (new_fg->gfx_vram_backup) {
                memcpy((void *)VGA_GFX_VRAM_ADDR, new_fg->gfx_vram_backup, VGA_GFX_SIZE);
            }
        } else {
            if (vga_is_graphics_mode()) {
                vga_set_mode_text_hardware();
                dynamic_keymap_reapply_fonts();
                mouse_set_bounds_from_display();
            }
            if (new_fg->text_vram_backup) {
                memcpy((void *)0xB8000, new_fg->text_vram_backup, 80 * 25 * sizeof(uint16_t));
                vga_set_cursor(new_fg->text_cursor_pos);
                if (new_fg->text_cursor_visible) {
                    vga_show_cursor();
                } else {
                    vga_hide_cursor();
                }
            }
        }
    }

}

void process_set_foreground(process_t *proc) {
    if (!proc) return;
    batch_foreground_proc = proc;
    process_sync_shared_context();
    if (process_is_batch_active()) {
        for (int i = 0; i < batch_proc_count; i++) {
            if (batch_procs && batch_procs[i] == proc) {
                batch_current_idx = i;
                proc->waiting_for_input = false;
                break;
            }
        }
        if (process_get_separate_windows()) {
            process_shared_ctx_t *ctx = process_get_shared_context();
            process_t *cur_disp = (ctx && ctx->magic == PROCESS_SHARED_MAGIC) ? ctx->batch_displayed_fg : batch_displayed_fg;
            if (cur_disp != proc) {
                process_switch_workspace_screen(cur_disp, proc);
            }
        }
    }
}

bool process_is_batch_active(void) {
    process_shared_ctx_t *ctx = process_get_shared_context();
    if (ctx && ctx->magic == PROCESS_SHARED_MAGIC) {
        return ctx->batch_active;
    }
    return batch_active;
}

bool process_get_separate_windows(void) {
    process_shared_ctx_t *ctx = process_get_shared_context();
    if (ctx && ctx->magic == PROCESS_SHARED_MAGIC) {
        return ctx->batch_active ? ctx->batch_separate_windows : false;
    }
    return batch_active ? batch_separate_windows : false;
}

void process_init_text_screen(process_t *proc) {
    if (!proc) return;
    if (!proc->text_vram_backup) {
        proc->text_vram_backup = (uint16_t *)kmalloc(80 * 25 * sizeof(uint16_t));
    }
    if (!proc->text_vram_backup) return;

    if (!proc->scroll_top_buffer) {
        proc->scroll_top_buffer = (uint16_t *)kmalloc(TERMINAL_SCROLL_HISTORY_SIZE * 80 * sizeof(uint16_t));
        if (proc->scroll_top_buffer) {
            for (int i = 0; i < TERMINAL_SCROLL_HISTORY_SIZE * 80; i++) {
                proc->scroll_top_buffer[i] = 0x0720;
            }
        }
    }
    if (!proc->scroll_bottom_buffer) {
        proc->scroll_bottom_buffer = (uint16_t *)kmalloc(TERMINAL_SCROLL_HISTORY_SIZE * 80 * sizeof(uint16_t));
        if (proc->scroll_bottom_buffer) {
            for (int i = 0; i < TERMINAL_SCROLL_HISTORY_SIZE * 80; i++) {
                proc->scroll_bottom_buffer[i] = 0x0720;
            }
        }
    }
    proc->scroll_top_count = 0;
    proc->scroll_bottom_count = 0;

    /* Fill entire screen with clean blank spaces 0x0720 */
    for (int i = 0; i < 80 * 25; i++) {
        proc->text_vram_backup[i] = 0x0720;
    }

    /* Top row 0: Header "IPO_OS" on left, "Created by IPOleksenko" on right */
    const char *os_name = "IPO_OS";
    const char *created_by = "Created by IPOleksenko";
    int os_len = strlen(os_name);
    int cr_len = strlen(created_by);
    for (int i = 0; i < os_len; i++) {
        proc->text_vram_backup[i] = vga_entry(os_name[i], VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    }
    for (int i = 0; i < cr_len; i++) {
        proc->text_vram_backup[80 - cr_len + i] = vga_entry(created_by[i], VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    }

    /* Start at beginning of workspace display content (row 2, column 0) */
    proc->text_cursor_pos = (uint16_t)VGA_START_CURSOR_POSITION;
    proc->text_cursor_visible = true;
}

void process_workspace_auto_scroll(process_t *proc) {
    if (!proc || !proc->text_vram_backup) return;

    uint16_t top_row = VGA_START_CURSOR_POSITION / VGA_WIDTH;
    uint16_t terminal_rows = VGA_HEIGHT - top_row;
    uint16_t last_line_start = (top_row + terminal_rows - 1) * VGA_WIDTH;

    if (proc->scroll_top_buffer) {
        if (proc->scroll_top_count >= TERMINAL_SCROLL_HISTORY_SIZE) {
            int keep = TERMINAL_SCROLL_HISTORY_SIZE / 2;
            int remove = proc->scroll_top_count - keep;
            memmove(&proc->scroll_top_buffer[0],
                    &proc->scroll_top_buffer[remove * 80],
                    (size_t)keep * 80 * sizeof(uint16_t));
            proc->scroll_top_count = keep;
        }
        memcpy(&proc->scroll_top_buffer[proc->scroll_top_count * 80],
               &proc->text_vram_backup[top_row * 80],
               80 * sizeof(uint16_t));
        proc->scroll_top_count++;
    }
    proc->scroll_bottom_count = 0;

    for (uint16_t r = top_row; r < top_row + terminal_rows - 1; r++) {
        memcpy((void *)&proc->text_vram_backup[r * 80],
               (const void *)&proc->text_vram_backup[(r + 1) * 80],
               80 * sizeof(uint16_t));
    }
    for (uint16_t col = 0; col < 80; col++) {
        proc->text_vram_backup[last_line_start + col] = 0x0720;
    }
}

void process_switch_foreground_next(void) {
    if (!batch_active || batch_proc_count <= 1) return;
    if (!batch_separate_windows) return;
    process_t *cur_fg = process_get_foreground();

    /* 1. Collect all distinct active workspace IDs (window_id) in order */
    int *unique_wins = (int *)kmalloc((size_t)batch_proc_count * sizeof(int));
    if (!unique_wins) return;
    int unique_count = 0;
    for (int i = 0; i < batch_proc_count; i++) {
        if (batch_procs && batch_procs[i] && process_is_active_or_has_windows(batch_procs[i])) {
            int wid = batch_procs[i]->window_id;
            bool exists = false;
            for (int u = 0; u < unique_count; u++) {
                if (unique_wins[u] == wid) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                unique_wins[unique_count++] = wid;
            }
        }
    }

    if (unique_count <= 1) {
        kfree(unique_wins);
        return;
    }

    /* 2. Find index of current workspace */
    int cur_wid = cur_fg ? cur_fg->window_id : unique_wins[0];
    int cur_win_idx = -1;
    for (int u = 0; u < unique_count; u++) {
        if (unique_wins[u] == cur_wid) {
            cur_win_idx = u;
            break;
        }
    }
    if (cur_win_idx == -1) cur_win_idx = 0;

    /* 3. Determine next workspace */
    int next_win_idx = (cur_win_idx + 1) % unique_count;
    int target_wid = unique_wins[next_win_idx];
    kfree(unique_wins);

    /* 4. Pick canonical foreground process for target workspace */
    process_t *target_proc = NULL;
    for (int i = 0; i < batch_proc_count; i++) {
        if (batch_procs && batch_procs[i] && batch_procs[i]->window_id == target_wid &&
            process_is_active_or_has_windows(batch_procs[i])) {
            target_proc = batch_procs[i];
            break;
        }
    }
    if (!target_proc) return;

    serial_printf("[proc] switch_fg: %s -> %s (win %d -> %d)\n",
        cur_fg ? cur_fg->name : "none",
        target_proc->name,
        cur_fg ? cur_fg->window_id : 0,
        target_wid);

    process_set_foreground(target_proc);
    if (wm_session_active() && (!process_get_separate_windows() || target_proc->is_wm_app)) {
        wm_focus_window_for_pid(target_proc->pid);
        wm_invalidate_all();
    }
    process_yield_kernel();
}

void process_switch_foreground_prev(void) {
    if (!batch_active || batch_proc_count <= 1) return;
    if (!batch_separate_windows) return;
    process_t *cur_fg = process_get_foreground();

    /* 1. Collect all distinct active workspace IDs (window_id) in order */
    int *unique_wins = (int *)kmalloc((size_t)batch_proc_count * sizeof(int));
    if (!unique_wins) return;
    int unique_count = 0;
    for (int i = 0; i < batch_proc_count; i++) {
        if (batch_procs && batch_procs[i] && process_is_active_or_has_windows(batch_procs[i])) {
            int wid = batch_procs[i]->window_id;
            bool exists = false;
            for (int u = 0; u < unique_count; u++) {
                if (unique_wins[u] == wid) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                unique_wins[unique_count++] = wid;
            }
        }
    }

    if (unique_count <= 1) {
        kfree(unique_wins);
        return;
    }

    /* 2. Find index of current workspace */
    int cur_wid = cur_fg ? cur_fg->window_id : unique_wins[0];
    int cur_win_idx = -1;
    for (int u = 0; u < unique_count; u++) {
        if (unique_wins[u] == cur_wid) {
            cur_win_idx = u;
            break;
        }
    }
    if (cur_win_idx == -1) cur_win_idx = 0;

    /* 3. Determine previous workspace */
    int prev_win_idx = (cur_win_idx - 1 + unique_count) % unique_count;
    int target_wid = unique_wins[prev_win_idx];
    kfree(unique_wins);

    /* 4. Pick canonical foreground process for target workspace */
    process_t *target_proc = NULL;
    for (int i = 0; i < batch_proc_count; i++) {
        if (batch_procs && batch_procs[i] && batch_procs[i]->window_id == target_wid &&
            process_is_active_or_has_windows(batch_procs[i])) {
            target_proc = batch_procs[i];
            break;
        }
    }
    if (!target_proc) return;

    serial_printf("[proc] switch_fg_prev: %s -> %s (win %d -> %d)\n",
        cur_fg ? cur_fg->name : "none",
        target_proc->name,
        cur_fg ? cur_fg->window_id : 0,
        target_wid);

    process_set_foreground(target_proc);
    if (wm_session_active() && (!process_get_separate_windows() || target_proc->is_wm_app)) {
        wm_focus_window_for_pid(target_proc->pid);
        wm_invalidate_all();
    }
    process_yield_kernel();
}

bool process_is_foreground(void) {
    if (batch_active) {
        return (process_get_foreground() == current_process);
    }
    int res = ipo_syscall(IPO_SYSCALL_PROCESS_IS_FOREGROUND, 0, NULL);
    if (res == (int)IPO_SYSCALL_ENOSYS) {
        return true;
    }
    return res != 0;
}

void process_yield(void) {
    if (batch_active) {
        process_yield_kernel();
        return;
    }
    ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
}

static char *default_env[] = {
    NULL
};

static char *kstrdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = (char *)kmalloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, s, len + 1);
    return copy;
}

static char *format_uint_dynamic(uint64_t num) {
    uint64_t temp = num;
    size_t len = 0;
    if (temp == 0) len = 1;
    else {
        while (temp > 0) {
            temp /= 10;
            len++;
        }
    }
    char *str = (char*)kmalloc(len + 1);
    if (!str) return NULL;
    str[len] = '\0';
    if (num == 0) {
        str[0] = '0';
        return str;
    }
    for (int j = (int)len - 1; j >= 0; j--) {
        str[j] = '0' + (char)(num % 10);
        num /= 10;
    }
    return str;
}

static char *format_int_dynamic(int64_t val) {
    bool neg = false;
    uint64_t num;
    if (val < 0) {
        neg = true;
        num = (uint64_t)(-val);
    } else {
        num = (uint64_t)val;
    }
    uint64_t temp = num;
    size_t len = 0;
    if (temp == 0) len = 1;
    else {
        while (temp > 0) {
            temp /= 10;
            len++;
        }
    }
    if (neg) len++;
    char *str = (char*)kmalloc(len + 1);
    if (!str) return NULL;
    str[len] = '\0';
    if (num == 0) {
        str[0] = '0';
        return str;
    }
    int j = (int)len - 1;
    while (num > 0) {
        str[j--] = '0' + (char)(num % 10);
        num /= 10;
    }
    if (neg) str[0] = '-';
    return str;
}

static char *format_hex_dynamic(uint64_t val) {
    uint64_t temp = val;
    size_t hex_len = 0;
    if (temp == 0) hex_len = 1;
    else {
        while (temp > 0) {
            temp /= 16;
            hex_len++;
        }
    }
    size_t total_len = 2 + hex_len; /* "0x" + hex digits */
    char *str = (char*)kmalloc(total_len + 1);
    if (!str) return NULL;
    str[0] = '0';
    str[1] = 'x';
    str[total_len] = '\0';
    if (val == 0) {
        str[2] = '0';
        return str;
    }
    for (int j = (int)total_len - 1; j >= 2; j--) {
        uint32_t d = (uint32_t)(val % 16);
        str[j] = (d < 10) ? ('0' + d) : ('a' + (d - 10));
        val /= 16;
    }
    return str;
}

static bool is_valid_string_pointer(const void *ptr, uint32_t *out_len) {
    if (out_len) *out_len = 0;
    if (!ptr) return false;
    uint32_t addr = (uint32_t)(uintptr_t)ptr;
    if (addr < 0x1000) return false;

    /* Verify start of string is within valid RAM (no hardcoded boundaries) */
    if (!memory_map_is_valid_ram(addr, 1)) {
        return false;
    }

    const uint8_t *s = (const uint8_t *)ptr;
    if (s[0] == '\0') {
        return false;
    }

    uint32_t len = 0;
    const uint32_t MAX_STR_LEN = 4096;
    while (len < MAX_STR_LEN) {
        /* On crossing a 4KB page boundary, re-verify RAM validity to prevent kernel page fault */
        if (len > 0 && ((addr + len) & 0xFFF) == 0) {
            if (!memory_map_is_valid_ram(addr + len, 1)) {
                return false;
            }
        }

        uint8_t c = s[len];
        if (c == '\0') {
            break;
        }

        /* Printable ASCII, common whitespace (\t, \r, \n), or UTF-8 multi-byte */
        if ((c >= 0x20 && c <= 0x7E) || c == '\n' || c == '\r' || c == '\t' || c >= 0x80) {
            len++;
            continue;
        }

        return false;
    }

    if (len > 0 && len < MAX_STR_LEN && s[len] == '\0') {
        if (out_len) *out_len = len;
        return true;
    }
    return false;
}

static inline uint8_t struct_read_u8(const uint8_t *p) {
    return *p;
}

static inline int8_t struct_read_i8(const uint8_t *p) {
    return (int8_t)*p;
}

static inline uint16_t struct_read_u16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline int16_t struct_read_i16(const uint8_t *p) {
    int16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline uint32_t struct_read_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline int32_t struct_read_i32(const uint8_t *p) {
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline uint64_t struct_read_u64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline int64_t struct_read_i64(const uint8_t *p) {
    int64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static char *format_float_dynamic(uint32_t bits) {
    uint32_t s = (bits >> 31) & 1;
    uint32_t raw_exp = (bits >> 23) & 0xFF;
    uint32_t raw_mant = bits & 0x7FFFFF;

    if (raw_exp == 255) {
        if (raw_mant == 0) return kstrdup(s ? "-Inf" : "+Inf");
        return kstrdup("NaN");
    }
    if (raw_exp == 0 && raw_mant == 0) {
        return kstrdup(s ? "-0.0" : "0.0");
    }

    int32_t exp;
    uint32_t mant;
    if (raw_exp == 0) {
        exp = -126;
        mant = raw_mant;
    } else {
        exp = (int32_t)raw_exp - 127;
        mant = raw_mant | 0x800000;
    }

    int32_t shift = 23 - exp;
    uint64_t int_part = 0;
    uint64_t frac_part = 0;

    if (shift <= 0) {
        int_part = (uint64_t)mant << (-shift);
        frac_part = 0;
    } else if (shift < 32) {
        int_part = mant >> shift;
        uint32_t frac_mask = (1u << shift) - 1u;
        uint64_t frac_raw = mant & frac_mask;
        frac_part = (frac_raw * 1000000ULL) >> shift;
    } else if (shift < 64) {
        int_part = 0;
        frac_part = ((uint64_t)mant * 1000000ULL) >> shift;
    } else {
        int_part = 0;
        frac_part = 0;
    }

    char buf[64];
    char frac_str[16];
    snprintf(frac_str, sizeof(frac_str), "%06u", (uint32_t)frac_part);
    int flen = (int)strlen(frac_str);
    while (flen > 1 && frac_str[flen - 1] == '0') {
        frac_str[--flen] = '\0';
    }

    snprintf(buf, sizeof(buf), "%s%u.%s", s ? "-" : "", (uint32_t)int_part, frac_str);
    return kstrdup(buf);
}

static char *format_double_dynamic(uint64_t bits) {
    uint32_t s = (bits >> 63) & 1;
    uint32_t raw_exp = (bits >> 52) & 0x7FF;
    uint64_t raw_mant = bits & 0x000FFFFFFFFFFFFFULL;

    if (raw_exp == 2047) {
        if (raw_mant == 0) return kstrdup(s ? "-Inf" : "+Inf");
        return kstrdup("NaN");
    }
    if (raw_exp == 0 && raw_mant == 0) {
        return kstrdup(s ? "-0.0" : "0.0");
    }

    int32_t exp;
    uint64_t mant;
    if (raw_exp == 0) {
        exp = -1022;
        mant = raw_mant;
    } else {
        exp = (int32_t)raw_exp - 1023;
        mant = raw_mant | 0x0010000000000000ULL;
    }

    int32_t shift = 52 - exp;
    uint64_t int_part = 0;

    char frac_str[16];
    int flen = 0;

    if (shift <= 0) {
        int_part = mant << (-shift);
        frac_str[flen++] = '0';
    } else if (shift < 60) {
        int_part = mant >> shift;
        uint64_t rem = mant & ((1ULL << shift) - 1ULL);
        for (int i = 0; i < 6; i++) {
            rem *= 10ULL;
            uint64_t digit = rem >> shift;
            frac_str[flen++] = '0' + (char)digit;
            rem &= ((1ULL << shift) - 1ULL);
        }
    } else {
        int_part = 0;
        frac_str[flen++] = '0';
    }
    frac_str[flen] = '\0';

    while (flen > 1 && frac_str[flen - 1] == '0') {
        frac_str[--flen] = '\0';
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%s%llu.%s", s ? "-" : "", (unsigned long long)int_part, frac_str);
    return kstrdup(buf);
}

static bool is_entry_struct_return(void *entry_ptr) {
    if (!entry_ptr) return false;
    const uint8_t *code = (const uint8_t *)entry_ptr;
    if (code[0] == 0xE9) {
        int32_t rel = struct_read_i32(code + 1);
        code = code + 5 + rel;
    }
    for (int i = 0; i < 512; i++) {
        if (code[i] == 0xC2 && code[i + 1] == 0x04 && code[i + 2] == 0x00) {
            return true;
        }
        if (code[i] == 0xC3) {
            if (i + 3 < 512 && code[i + 1] == 0x55 && code[i + 2] == 0x89 && code[i + 3] == 0xE5) {
                return false;
            }
        }
    }
    return false;
}

static inline bool is_char_byte(uint8_t c) {
    return (c >= 0x20 && c <= 0x7E) || (c >= 0x80 && c != 0xDD);
}

static char *format_char_dynamic(uint8_t c) {
    if (c >= 0x20 && c <= 0x7E) {
        char *s = (char *)kmalloc(4);
        if (!s) return NULL;
        s[0] = '\'';
        s[1] = (char)c;
        s[2] = '\'';
        s[3] = '\0';
        return s;
    }
    if ((c >= 0x80 && c <= 0x8F) || c == 0x91) {
        char *s = (char *)kmalloc(5);
        if (!s) return NULL;
        s[0] = '\'';
        s[1] = (char)0xD1;
        s[2] = (char)c;
        s[3] = '\'';
        s[4] = '\0';
        return s;
    }
    if ((c >= 0x90 && c <= 0xBF) || c == 0x81) {
        char *s = (char *)kmalloc(5);
        if (!s) return NULL;
        s[0] = '\'';
        s[1] = (char)0xD0;
        s[2] = (char)c;
        s[3] = '\'';
        s[4] = '\0';
        return s;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        char *s = (char *)kmalloc(5);
        if (!s) return NULL;
        s[0] = '\'';
        s[1] = (char)0xD1;
        s[2] = (char)(c - 0xE0 + 0x80);
        s[3] = '\'';
        s[4] = '\0';
        return s;
    }
    if (c >= 0xC0 && c <= 0xDF) {
        char *s = (char *)kmalloc(5);
        if (!s) return NULL;
        s[0] = '\'';
        s[1] = (char)0xD0;
        s[2] = (char)(c - 0xC0 + 0x90);
        s[3] = '\'';
        s[4] = '\0';
        return s;
    }
    if (c >= 0xF0 && c <= 0xFF) {
        char *s = (char *)kmalloc(5);
        if (!s) return NULL;
        s[0] = '\'';
        s[1] = (char)0xD1;
        s[2] = (char)(c - 0xF0 + 0x80);
        s[3] = '\'';
        s[4] = '\0';
        return s;
    }
    if (c == '\0') return kstrdup("'\\0'");
    if (c == '\n') return kstrdup("'\\n'");
    if (c == '\t') return kstrdup("'\\t'");
    if (c == '\r') return kstrdup("'\\r'");
    return format_int_dynamic((int8_t)c);
}

static char *format_char_arg_dynamic(uint8_t c) {
    if (c >= 0x20 && c <= 0x7E) {
        char *s = (char *)kmalloc(2);
        if (!s) return NULL;
        s[0] = (char)c;
        s[1] = '\0';
        return s;
    }
    if ((c >= 0x80 && c <= 0x8F) || c == 0x91) {
        char *s = (char *)kmalloc(3);
        if (!s) return NULL;
        s[0] = (char)0xD1;
        s[1] = (char)c;
        s[2] = '\0';
        return s;
    }
    if ((c >= 0x90 && c <= 0xBF) || c == 0x81) {
        char *s = (char *)kmalloc(3);
        if (!s) return NULL;
        s[0] = (char)0xD0;
        s[1] = (char)c;
        s[2] = '\0';
        return s;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        char *s = (char *)kmalloc(3);
        if (!s) return NULL;
        s[0] = (char)0xD1;
        s[1] = (char)(c - 0xE0 + 0x80);
        s[2] = '\0';
        return s;
    }
    if (c >= 0xC0 && c <= 0xDF) {
        char *s = (char *)kmalloc(3);
        if (!s) return NULL;
        s[0] = (char)0xD0;
        s[1] = (char)(c - 0xC0 + 0x90);
        s[2] = '\0';
        return s;
    }
    if (c >= 0xF0 && c <= 0xFF) {
        char *s = (char *)kmalloc(3);
        if (!s) return NULL;
        s[0] = (char)0xD1;
        s[1] = (char)(c - 0xF0 + 0x80);
        s[2] = '\0';
        return s;
    }
    return format_int_dynamic((int8_t)c);
}

static inline bool match_keyword(const char *p, const char *kw, size_t kw_len) {
    if (strncmp(p, kw, kw_len) != 0) return false;
    char next = p[kw_len];
    if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') || (next >= '0' && next <= '9') || next == '_') {
        return false;
    }
    return true;
}

static void format_struct_fields_internal(process_return_value_t *ret_val,
                                          const uint8_t *buf,
                                          uint32_t size,
                                          const char **fmt_ptr,
                                          uint32_t *off_ptr,
                                          uint32_t *bit_off_ptr,
                                          bool is_packed,
                                          int depth) {
    if (!ret_val || !buf || !fmt_ptr || !*fmt_ptr || !off_ptr || !bit_off_ptr) return;

    const char *p = *fmt_ptr;
    bool first_elem = true;

    while (*p && *p != '}' && *p != ']') {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') p++;
        if (!*p || *p == '}' || *p == ']') break;

        /* Check for packing directive */
        if (match_keyword(p, "packed", 6)) {
            is_packed = true;
            p += 6;
            continue;
        }
        if (*p == '!') {
            is_packed = true;
            p++;
            continue;
        }

        /* Check for nested struct */
        if (*p == '{') {
            p++; /* skip '{' */
            if (*bit_off_ptr > 0) {
                *off_ptr += (*bit_off_ptr + 7) / 8;
                *bit_off_ptr = 0;
            }
            if (!first_elem) {
                process_return_val_append_str(ret_val, ", ");
            }
            process_return_val_append_str(ret_val, "{ ");
            format_struct_fields_internal(ret_val, buf, size, &p, off_ptr, bit_off_ptr, is_packed, depth + 1);
            process_return_val_append_str(ret_val, " }");
            first_elem = false;
            if (*p == '}') p++;
            /* Skip any trailing variable name: e.g. "} inner_var;" */
            while (*p == ' ' || *p == '\t') p++;
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_') {
                while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') p++;
            }
            continue;
        }

        /* Check for field name prefix (e.g. "data: char*" or "len: i")
         * Note: distinguish from bit-field "int a:3" where digit follows colon.
         */
        const char *scan = p;
        while (*scan && *scan != ':' && *scan != ',' && *scan != ';' && *scan != '{' && *scan != '}' && *scan != '[' && *scan != ']') scan++;
        if (*scan == ':') {
            const char *after = scan + 1;
            while (*after == ' ' || *after == '\t') after++;
            if (!(*after >= '0' && *after <= '9')) {
                p = after;
            }
        }

        /* Parse array count prefix: e.g. [4i] or [16c] */
        uint32_t count = 1;
        bool has_count_prefix = false;
        if (*p == '[') {
            p++;
            has_count_prefix = true;
            uint32_t cnt = 0;
            while (*p >= '0' && *p <= '9') {
                cnt = cnt * 10 + (*p++ - '0');
            }
            if (cnt > 0) count = cnt;
            while (*p == ' ' || *p == '\t') p++;
        }

        /* Skip optional "const" */
        if (match_keyword(p, "const", 5)) {
            p += 5;
            while (*p == ' ' || *p == '\t') p++;
        }

        /* Parse type */
        char type_char = '\0';
        uint32_t elem_size = 1;
        uint32_t elem_align = 1;

        if (match_keyword(p, "char", 4)) {
            p += 4;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '*') {
                p++;
                type_char = 's';
                elem_size = 4; elem_align = 4;
            } else {
                type_char = 'c';
                elem_size = 1; elem_align = 1;
            }
        } else if (match_keyword(p, "string", 6) || strncmp(p, "char*", 5) == 0) {
            type_char = 's';
            elem_size = 4; elem_align = 4;
            p += (strncmp(p, "char*", 5) == 0) ? 5 : 6;
        } else if (match_keyword(p, "void", 4) || strncmp(p, "void*", 5) == 0 || match_keyword(p, "ptr", 3) || match_keyword(p, "pointer", 7)) {
            type_char = 'p';
            elem_size = 4; elem_align = 4;
            if (strncmp(p, "void*", 5) == 0) p += 5;
            else if (match_keyword(p, "void", 4)) {
                p += 4;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '*') p++;
            } else if (match_keyword(p, "pointer", 7)) p += 7;
            else p += 3;
        } else if (match_keyword(p, "int8", 4) || match_keyword(p, "int8_t", 6) || match_keyword(p, "signed char", 11)) {
            type_char = 'b';
            elem_size = 1; elem_align = 1;
            p += match_keyword(p, "int8_t", 6) ? 6 : (match_keyword(p, "int8", 4) ? 4 : 11);
        } else if (match_keyword(p, "uint8", 5) || match_keyword(p, "uint8_t", 7) || match_keyword(p, "unsigned char", 13) || match_keyword(p, "byte", 4)) {
            type_char = 'B';
            elem_size = 1; elem_align = 1;
            p += match_keyword(p, "uint8_t", 7) ? 7 : (match_keyword(p, "uint8", 5) ? 5 : (match_keyword(p, "byte", 4) ? 4 : 13));
        } else if (match_keyword(p, "short", 5) || match_keyword(p, "int16", 5) || match_keyword(p, "int16_t", 7) || match_keyword(p, "signed short", 12)) {
            type_char = 'h';
            elem_size = 2; elem_align = 2;
            p += match_keyword(p, "int16_t", 7) ? 7 : (match_keyword(p, "signed short", 12) ? 12 : 5);
        } else if (match_keyword(p, "ushort", 6) || match_keyword(p, "uint16", 6) || match_keyword(p, "uint16_t", 8) || match_keyword(p, "unsigned short", 14)) {
            type_char = 'H';
            elem_size = 2; elem_align = 2;
            p += match_keyword(p, "unsigned short", 14) ? 14 : (match_keyword(p, "uint16_t", 8) ? 8 : 6);
        } else if (match_keyword(p, "long long", 9) || match_keyword(p, "int64", 5) || match_keyword(p, "int64_t", 7) || match_keyword(p, "signed long long", 16)) {
            type_char = 'q';
            elem_size = 8; elem_align = 4;
            p += match_keyword(p, "signed long long", 16) ? 16 : (match_keyword(p, "long long", 9) ? 9 : (match_keyword(p, "int64_t", 7) ? 7 : 5));
        } else if (match_keyword(p, "ulong long", 10) || match_keyword(p, "unsigned long long", 18) || match_keyword(p, "uint64", 6) || match_keyword(p, "uint64_t", 8)) {
            type_char = 'Q';
            elem_size = 8; elem_align = 4;
            p += match_keyword(p, "unsigned long long", 18) ? 18 : (match_keyword(p, "ulong long", 10) ? 10 : (match_keyword(p, "uint64_t", 8) ? 8 : 6));
        } else if (match_keyword(p, "size_t", 6) || match_keyword(p, "uint32", 6) || match_keyword(p, "uint32_t", 8) || match_keyword(p, "uint", 4) ||
                   match_keyword(p, "unsigned int", 12) || match_keyword(p, "ulong", 5) || match_keyword(p, "unsigned long", 13) || match_keyword(p, "uintptr_t", 9)) {
            type_char = 'u';
            elem_size = 4; elem_align = 4;
            if (match_keyword(p, "unsigned long", 13)) p += 13;
            else if (match_keyword(p, "unsigned int", 12)) p += 12;
            else if (match_keyword(p, "uintptr_t", 9)) p += 9;
            else if (match_keyword(p, "uint32_t", 8)) p += 8;
            else if (match_keyword(p, "size_t", 6) || match_keyword(p, "uint32", 6)) p += 6;
            else if (match_keyword(p, "ulong", 5)) p += 5;
            else p += 4;
        } else if (match_keyword(p, "int32", 5) || match_keyword(p, "int32_t", 7) || match_keyword(p, "int", 3) || match_keyword(p, "signed int", 10) ||
                   match_keyword(p, "long", 4) || match_keyword(p, "signed long", 11) || match_keyword(p, "ptrdiff_t", 9) || match_keyword(p, "intptr_t", 8) || match_keyword(p, "enum", 4)) {
            type_char = 'i';
            elem_size = 4; elem_align = 4;
            if (match_keyword(p, "signed long", 11)) p += 11;
            else if (match_keyword(p, "signed int", 10)) p += 10;
            else if (match_keyword(p, "ptrdiff_t", 9)) p += 9;
            else if (match_keyword(p, "intptr_t", 8)) p += 8;
            else if (match_keyword(p, "int32_t", 7)) p += 7;
            else if (match_keyword(p, "int32", 5)) p += 5;
            else if (match_keyword(p, "long", 4) || match_keyword(p, "enum", 4)) p += 4;
            else p += 3;
        } else if (match_keyword(p, "float", 5)) {
            type_char = 'f';
            elem_size = 4; elem_align = 4;
            p += 5;
        } else if (match_keyword(p, "double", 6)) {
            type_char = 'd';
            elem_size = 8; elem_align = 4;
            p += 6;
        } else if (match_keyword(p, "bool", 4) || match_keyword(p, "_Bool", 5)) {
            type_char = '?';
            elem_size = 1; elem_align = 1;
            p += match_keyword(p, "_Bool", 5) ? 5 : 4;
        } else {
            /* Single character code or custom identifier */
            char ch = *p;
            switch (ch) {
                case 'c': type_char = 'c'; elem_size = 1; elem_align = 1; p++; break;
                case 'b': type_char = 'b'; elem_size = 1; elem_align = 1; p++; break;
                case 'B': type_char = 'B'; elem_size = 1; elem_align = 1; p++; break;
                case '?': type_char = '?'; elem_size = 1; elem_align = 1; p++; break;
                case 'h': type_char = 'h'; elem_size = 2; elem_align = 2; p++; break;
                case 'H': type_char = 'H'; elem_size = 2; elem_align = 2; p++; break;
                case 'i': case 'l': type_char = 'i'; elem_size = 4; elem_align = 4; p++; break;
                case 'u': case 'I': case 'L': type_char = 'u'; elem_size = 4; elem_align = 4; p++; break;
                case 'q': type_char = 'q'; elem_size = 8; elem_align = 4; p++; break;
                case 'Q': type_char = 'Q'; elem_size = 8; elem_align = 4; p++; break;
                case 'f': type_char = 'f'; elem_size = 4; elem_align = 4; p++; break;
                case 'd': type_char = 'd'; elem_size = 8; elem_align = 4; p++; break;
                case 'p': type_char = 'p'; elem_size = 4; elem_align = 4; p++; break;
                case 's': type_char = 's'; elem_size = 4; elem_align = 4; p++; break;
                default: {
                    /* Custom type name: e.g. struct name or typedef */
                    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') p++;
                    while (*p == ' ' || *p == '\t') p++;
                    if (*p == '*') {
                        p++;
                        type_char = 'p';
                        elem_size = 4; elem_align = 4;
                    } else {
                        type_char = 'i';
                        elem_size = 4; elem_align = 4;
                    }
                    break;
                }
            }
        }

        /* Check for pointer suffix (e.g. "int *ptr" or "Foo*") */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '*') {
            p++;
            if (type_char != 's') {
                type_char = 'p';
                elem_size = 4;
                elem_align = 4;
            }
        }

        /* Skip field variable name */
        while (*p == ' ' || *p == '\t') p++;
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_') {
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') p++;
        }

        /* Parse array suffix: e.g. c[16] or [4] */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            p++;
            uint32_t cnt = 0;
            while (*p >= '0' && *p <= '9') {
                cnt = cnt * 10 + (*p++ - '0');
            }
            if (cnt > 0) count = cnt;
            while (*p && *p != ']') p++;
            if (*p == ']') p++;
        } else if (has_count_prefix) {
            while (*p && *p != ']') p++;
            if (*p == ']') p++;
        }

        /* Check for bit-field width: e.g. ": 3" */
        while (*p == ' ' || *p == '\t') p++;
        uint32_t bit_width = 0;
        if (*p == ':') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            while (*p >= '0' && *p <= '9') {
                bit_width = bit_width * 10 + (*p++ - '0');
            }
        }

        /* Skip remaining delimiters in this field declaration */
        while (*p && *p != ',' && *p != ';' && *p != '}' && *p != ']') p++;
        if (*p == ',' || *p == ';') p++;

        if (is_packed) {
            elem_align = 1;
        }

        /* Bit-field handling */
        if (bit_width > 0) {
            uint32_t bit_off = *bit_off_ptr;
            uint32_t cur_off = *off_ptr;
            if (bit_off == 0 && elem_align > 1 && !is_packed) {
                cur_off = (cur_off + elem_align - 1) & ~(elem_align - 1);
                *off_ptr = cur_off;
            }
            if (cur_off + elem_size <= size) {
                uint64_t container = 0;
                if (elem_size == 1) container = buf[cur_off];
                else if (elem_size == 2) container = struct_read_u16(buf + cur_off);
                else if (elem_size == 8) container = struct_read_u64(buf + cur_off);
                else container = struct_read_u32(buf + cur_off);

                uint64_t val = (container >> bit_off) & ((bit_width >= 64) ? ~0ULL : ((1ULL << bit_width) - 1ULL));
                char *field_str = format_uint_dynamic(val);
                if (field_str) {
                    if (!first_elem) process_return_val_append_str(ret_val, ", ");
                    process_return_val_append_str(ret_val, field_str);
                    process_return_val_add_arg(ret_val, field_str);
                    first_elem = false;
                    kfree(field_str);
                }
                bit_off += bit_width;
                if (bit_off >= elem_size * 8) {
                    *off_ptr = cur_off + elem_size;
                    bit_off = 0;
                }
                *bit_off_ptr = bit_off;
            }
            continue;
        }

        /* If previous fields used bit-fields, align to byte boundary */
        if (*bit_off_ptr > 0) {
            *off_ptr += (*bit_off_ptr + 7) / 8;
            *bit_off_ptr = 0;
        }

        /* If it is an array of char (e.g. c[32]) and contains a null-terminated string */
        if (type_char == 'c' && count > 1) {
            uint32_t cur_off = *off_ptr;
            if (elem_align > 1) {
                cur_off = (cur_off + elem_align - 1) & ~(elem_align - 1);
            }
            if (cur_off + count <= size) {
                const char *arr_str = (const char *)(buf + cur_off);
                bool is_str = false;
                for (size_t s_idx = 0; s_idx < count; s_idx++) {
                    if (arr_str[s_idx] == '\0') {
                        is_str = true;
                        break;
                    }
                }
                if (is_str) {
                    if (!first_elem) process_return_val_append_str(ret_val, ", ");
                    process_return_val_append_str(ret_val, "\"");
                    process_return_val_append_str(ret_val, arr_str);
                    process_return_val_append_str(ret_val, "\"");
                    process_return_val_add_arg(ret_val, arr_str);
                    first_elem = false;
                    *off_ptr = cur_off + count;
                    continue;
                }
            }
        }

        /* Format each element */
        bool is_array = (count > 1);
        if (is_array) {
            if (!first_elem) process_return_val_append_str(ret_val, ", ");
            process_return_val_append_str(ret_val, "[ ");
        }

        for (uint32_t idx = 0; idx < count; idx++) {
            uint32_t cur_off = *off_ptr;
            if (elem_align > 1) {
                cur_off = (cur_off + elem_align - 1) & ~(elem_align - 1);
            }
            if (cur_off + elem_size > size) {
                break;
            }

            char *field_str = NULL;
            char *field_arg = NULL;

            switch (type_char) {
                case 'c': {
                    uint8_t c = buf[cur_off];
                    field_str = format_char_dynamic(c);
                    field_arg = format_char_arg_dynamic(c);
                    break;
                }
                case 'b': {
                    int8_t v = (int8_t)buf[cur_off];
                    field_str = format_int_dynamic((int64_t)v);
                    break;
                }
                case 'B': {
                    uint8_t v = buf[cur_off];
                    field_str = format_uint_dynamic((uint64_t)v);
                    break;
                }
                case '?': {
                    uint8_t v = buf[cur_off];
                    field_str = kstrdup(v ? "true" : "false");
                    break;
                }
                case 'h': {
                    int16_t v = struct_read_i16(buf + cur_off);
                    field_str = format_int_dynamic((int64_t)v);
                    break;
                }
                case 'H': {
                    uint16_t v = struct_read_u16(buf + cur_off);
                    field_str = format_uint_dynamic((uint64_t)v);
                    break;
                }
                case 'i': case 'l': {
                    int32_t v = struct_read_i32(buf + cur_off);
                    field_str = format_int_dynamic((int64_t)v);
                    break;
                }
                case 'u': case 'I': case 'L': {
                    uint32_t v = struct_read_u32(buf + cur_off);
                    field_str = format_uint_dynamic((uint64_t)v);
                    break;
                }
                case 'q': {
                    int64_t v = struct_read_i64(buf + cur_off);
                    field_str = format_int_dynamic(v);
                    break;
                }
                case 'Q': {
                    uint64_t v = struct_read_u64(buf + cur_off);
                    field_str = format_uint_dynamic(v);
                    break;
                }
                case 'f': {
                    uint32_t bits = struct_read_u32(buf + cur_off);
                    field_str = format_float_dynamic(bits);
                    break;
                }
                case 'd': {
                    uint64_t bits = struct_read_u64(buf + cur_off);
                    field_str = format_double_dynamic(bits);
                    break;
                }
                case 'p': {
                    uint32_t ptr = struct_read_u32(buf + cur_off);
                    if (ptr == 0) field_str = kstrdup("NULL");
                    else field_str = format_hex_dynamic((uint64_t)ptr);
                    break;
                }
                case 's': {
                    uint32_t ptr = struct_read_u32(buf + cur_off);
                    uint32_t slen = 0;
                    if (ptr == 0) {
                        field_str = kstrdup("NULL");
                        field_arg = kstrdup("NULL");
                    } else if (is_valid_string_pointer((const void*)ptr, &slen)) {
                        const char *src_str = (const char*)ptr;
                        field_str = (char*)kmalloc(slen + 3);
                        if (field_str) {
                            field_str[0] = '"';
                            memcpy(field_str + 1, src_str, slen);
                            field_str[slen + 1] = '"';
                            field_str[slen + 2] = '\0';
                        }
                        field_arg = kstrdup(src_str);
                    } else {
                        field_str = format_hex_dynamic((uint64_t)ptr);
                        field_arg = kstrdup(field_str);
                    }
                    break;
                }
                default: {
                    int32_t v = struct_read_i32(buf + cur_off);
                    field_str = format_int_dynamic((int64_t)v);
                    break;
                }
            }

            if (field_str) {
                if (is_array) {
                    if (idx > 0) process_return_val_append_str(ret_val, ", ");
                } else {
                    if (!first_elem) process_return_val_append_str(ret_val, ", ");
                }
                process_return_val_append_str(ret_val, field_str);
                process_return_val_add_arg(ret_val, field_arg ? field_arg : field_str);
                if (!is_array) first_elem = false;
            }

            if (field_str) kfree(field_str);
            if (field_arg) kfree(field_arg);

            *off_ptr = cur_off + elem_size;
        }

        if (is_array) {
            process_return_val_append_str(ret_val, " ]");
            first_elem = false;
        }
    }

    *fmt_ptr = p;
}

static void format_struct_fields(process_return_value_t *ret_val,
                                 const uint8_t *buf,
                                 uint32_t size,
                                 const char *fmt) {
    if (!ret_val) return;
    if (!buf || size == 0 || !fmt || fmt[0] == '\0') {
        process_return_val_set_str(ret_val, "{ }");
        return;
    }

    process_return_val_set_str(ret_val, "{ ");
    const char *p = fmt;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '{') p++; /* skip leading '{' if present */

    uint32_t off = 0;
    uint32_t bit_off = 0;
    format_struct_fields_internal(ret_val, buf, size, &p, &off, &bit_off, false, 0);
    process_return_val_append_str(ret_val, " }");
}

static void format_struct_fields_universal(process_return_value_t *ret_val, const uint8_t *struct_buf, uint32_t written_size) {
    if (!ret_val || !struct_buf || written_size == 0) {
        if (ret_val) process_return_val_set_str(ret_val, "{ }");
        return;
    }

    process_return_val_set_str(ret_val, "{ ");
    uint32_t off = 0;
    bool first_field = true;

    while (off < written_size) {
        /* Skip any unwritten poison padding (0xDD) or intra-struct alignment padding */
        while (off < written_size && (struct_buf[off] == 0xDD || ((off % 4 != 0) && struct_buf[off] == 0))) {
            off++;
        }
        if (off >= written_size) {
            break;
        }

        uint32_t rem = written_size - off;
        char *field_str = NULL;
        char *field_arg = NULL;
        uint32_t advance = 0;

        /* Case 1: Unaligned offset (off % 4 != 0)
         * In 32-bit x86 C ABI, int and pointer fields are always 4-byte aligned.
         * Therefore, any field at an unaligned offset CANNOT be an int or pointer.
         * It must be a 1-byte char (or int8), or 2-byte short (if off % 2 == 0).
         */
        if (off % 4 != 0) {
            if ((off % 2 == 0) && rem >= 2 && struct_buf[off + 1] != 0xDD &&
                !is_char_byte(struct_buf[off])) {
                /* 16-bit short integer */
                int16_t sval = struct_read_i16(struct_buf + off);
                field_str = format_int_dynamic((int32_t)sval);
                if (field_str) field_arg = kstrdup(field_str);
                advance = 2;
            } else {
                /* 1-byte char, bool or int8 */
                uint8_t c = struct_buf[off];
                if (c == 1 && (off + 1 >= written_size || struct_buf[off + 1] == 0 || struct_buf[off + 1] == 0xDD)) {
                    field_str = kstrdup("true");
                    if (field_str) field_arg = kstrdup(field_str);
                } else if (is_char_byte(c)) {
                    field_str = format_char_dynamic(c);
                    field_arg = format_char_arg_dynamic(c);
                } else {
                    field_str = format_int_dynamic((int8_t)c);
                    if (field_str) field_arg = kstrdup(field_str);
                }
                advance = 1;
            }
        }
        /* Case 2: 1-byte remaining (or explicit 0xDD poison immediately after) */
        else if (rem == 1 || (off + 1 < written_size && struct_buf[off + 1] == 0xDD)) {
            uint8_t c = struct_buf[off];
            if (is_char_byte(c)) {
                field_str = format_char_dynamic(c);
                field_arg = format_char_arg_dynamic(c);
            } else {
                field_str = format_int_dynamic((int8_t)c);
                if (field_str) field_arg = kstrdup(field_str);
            }
            advance = 1;
        }
        /* Case 3: 4 or more bytes aligned at 4-byte boundary */
        else if (rem >= 4) {
            uint32_t val32 = struct_read_u32(struct_buf + off);
            uint32_t str_len = 0;

            /* Check if valid string pointer */
            if (is_valid_string_pointer((const void*)val32, &str_len)) {
                const char *src_str = (const char*)val32;
                field_str = (char*)kmalloc(str_len + 3);
                if (field_str) {
                    field_str[0] = '"';
                    memcpy(field_str + 1, src_str, str_len);
                    field_str[str_len + 1] = '"';
                    field_str[str_len + 2] = '\0';
                }
                field_arg = kstrdup(src_str);
                advance = 4;
            }
            /* Check if multiple packed chars/bytes (e.g. char c1, c2, ... or char + bool) */
            else if (rem >= 2 &&
                     is_char_byte(struct_buf[off]) &&
                     is_char_byte(struct_buf[off + 1])) {
                uint8_t c = struct_buf[off];
                field_str = format_char_dynamic(c);
                field_arg = format_char_arg_dynamic(c);
                advance = 1;
            }
            /* 64-bit integer check (negative sign extension or 64-bit architecture) */
            else if (rem >= 8 && (off % 8 == 0) &&
                     #if UINTPTR_MAX > 0xFFFFFFFFU
                     true
                     #else
                     ((int32_t)struct_read_u32(struct_buf + off + 4) == -1 && (int32_t)val32 < 0)
                     #endif
            ) {
                int64_t val64 = struct_read_i64(struct_buf + off);
                field_str = format_int_dynamic(val64);
                if (field_str) field_arg = kstrdup(field_str);
                advance = 8;
            }
            /* Valid RAM pointer (non-string) */
            else if (val32 >= 0x10000 && memory_map_is_valid_ram(val32, 1) && (val32 % 4 == 0) &&
                     !is_valid_string_pointer((const void*)val32, NULL) &&
                     (val32 >= 0xC0000000 || val32 >= 0x01000000)) {
                field_str = format_hex_dynamic(val32);
                if (field_str) field_arg = kstrdup(field_str);
                advance = 4;
            }
            /* Standard 32-bit integer */
            else {
                field_str = format_int_dynamic((int32_t)val32);
                if (field_str) field_arg = kstrdup(field_str);
                advance = 4;
            }
        }
        /* Case 4: 2 or 3 bytes remaining */
        else {
            uint8_t c = struct_buf[off];
            if (is_char_byte(c)) {
                field_str = format_char_dynamic(c);
                field_arg = format_char_arg_dynamic(c);
            } else {
                field_str = format_int_dynamic((int8_t)c);
                if (field_str) field_arg = kstrdup(field_str);
            }
            advance = 1;
        }

        /* Only append comma if field_str allocation succeeded */
        if (field_str) {
            if (!first_field) {
                process_return_val_append_str(ret_val, ", ");
            }
            process_return_val_append_str(ret_val, field_str);
            if (field_arg) {
                process_return_val_add_arg(ret_val, field_arg);
            }
            first_field = false;
        }

        if (field_str) kfree(field_str);
        if (field_arg) kfree(field_arg);

        if (advance == 0) advance = 1;
        off += advance;
    }

    process_return_val_append_str(ret_val, " }");
}

static void process_trampoline(void) {
    process_t *proc = current_process;
    char **argv = (char**)proc->argv_kernel;

    serial_printf("[process] pid=%u start trampoline\n", proc->pid);

    size_t struct_buf_cap = 512;
    uint8_t *struct_buf = (uint8_t*)kmalloc(struct_buf_cap);
    if (struct_buf) memset(struct_buf, 0xDD, struct_buf_cap);

    bool is_struct = is_entry_struct_return((void*)proc->entry_point);
    uint32_t ret_eax = 0;
    uint32_t ret_edx = 0;

    if (proc->entry_point != 0) {
        uint32_t cur_esp;
        __asm__ volatile("movl %%esp, %0" : "=r"(cur_esp));
        serial_printf("[process] pid=%u calling entry=0x%x esp=0x%x is_struct=%d\n",
                      proc->pid, proc->entry_point, cur_esp, (int)is_struct);

        uint32_t saved_esp;
        __asm__ volatile("movl %%esp, %0" : "=r"(saved_esp));

        if (is_struct && struct_buf) {
            typedef void* (*struct_entry_func_t)(void *ret_buf, int argc, char **argv, char **envp);
            struct_entry_func_t struct_entry = (struct_entry_func_t)proc->entry_point;
            struct_entry(struct_buf, proc->argc, argv, default_env);
            ret_eax = (uint32_t)(uintptr_t)struct_buf;
        } else {
            typedef int (*entry_func_t)(int, char**, char**);
            entry_func_t entry = (entry_func_t)proc->entry_point;
            __asm__ volatile(
                "pushl %3\n\t"
                "pushl %4\n\t"
                "pushl %5\n\t"
                "call *%2\n\t"
                "addl $12, %%esp\n\t"
                : "=a"(ret_eax), "=d"(ret_edx)
                : "r"(entry), "r"(default_env), "r"(argv), "r"(proc->argc)
                : "ecx", "memory", "cc"
            );
        }

        __asm__ volatile("movl %0, %%esp" : : "r"(saved_esp));
    }

    if (!proc->return_val_explicit) {
        process_return_val_reset(&proc->return_val);

        /* Check if process has an auto-detected or embedded return format descriptor */
        const char *embedded_fmt = proc->return_format;
        if (!embedded_fmt && proc->binary_storage && proc->binary_size >= 8) {
            const uint8_t *bin = (const uint8_t *)proc->binary_storage;
            uint32_t limit = proc->binary_size - 8;
            for (uint32_t i = 0; i <= limit; i++) {
                if (bin[i] == 0x49 && bin[i+1] == 0x50 && bin[i+2] == 0x4F && bin[i+3] == 0x52) {
                    uint32_t str_ptr = struct_read_u32(bin + i + 4);
                    const char *candidate = NULL;
                    uint32_t base = (uint32_t)(uintptr_t)proc->binary_base;
                    if (str_ptr >= base && str_ptr < base + proc->binary_size) {
                        candidate = (const char *)(bin + (str_ptr - base));
                    } else if (is_valid_string_pointer((const void*)str_ptr, NULL)) {
                        candidate = (const char *)str_ptr;
                    }
                    if (candidate && candidate[0] != '\0') {
                        embedded_fmt = candidate;
                        break;
                    }
                }
            }
        }

        if (is_struct && struct_buf) {
            /* Find exact written size in poisoned buffer */
            int last_written = -1;
            for (int i = (int)struct_buf_cap - 1; i >= 0; i--) {
                if (struct_buf[i] != 0xDD) {
                    last_written = i;
                    break;
                }
            }
            uint32_t written_size = (last_written >= 0) ? (uint32_t)(last_written + 1) : 0;

            proc->return_val.type = PROC_RET_STRUCT;
            if (embedded_fmt && embedded_fmt[0] != '\0') {
                process_return_val_set_type_name(&proc->return_val, embedded_fmt);
            } else {
                process_return_val_set_type_name(&proc->return_val, "struct");
            }
            process_return_val_set_struct_data(&proc->return_val, struct_buf, written_size);
            proc->return_val.raw_eax = (uint32_t)(uintptr_t)struct_buf;
            proc->return_val.raw_edx = 0;
            proc->return_val.int_val = (written_size >= 4) ? *(int32_t*)struct_buf : (written_size > 0 ? (int32_t)struct_buf[0] : 0);
            proc->exit_code = proc->return_val.int_val;

            /* Output formatting: use explicit descriptor if provided, else universal */
            if (embedded_fmt && embedded_fmt[0] != '\0') {
                format_struct_fields(&proc->return_val, struct_buf, written_size, embedded_fmt);
            } else {
                format_struct_fields_universal(&proc->return_val, struct_buf, written_size);
            }
        } else {
            uint32_t str_len = 0;
            if (embedded_fmt && strcmp(embedded_fmt, "void") == 0) {
                proc->return_val.type = PROC_RET_INT;
                process_return_val_set_type_name(&proc->return_val, "void");
                proc->return_val.int_val = 0;
                proc->return_val.raw_eax = 0;
                proc->return_val.raw_edx = 0;
                proc->exit_code = 0;
            } else if (embedded_fmt && (strcmp(embedded_fmt, "bool") == 0 || strcmp(embedded_fmt, "_Bool") == 0)) {
                proc->return_val.type = PROC_RET_INT;
                process_return_val_set_type_name(&proc->return_val, "bool");
                proc->return_val.int_val = (ret_eax != 0) ? 1 : 0;
                proc->return_val.raw_eax = ret_eax;
                proc->return_val.raw_edx = ret_edx;
                proc->exit_code = (int32_t)ret_eax;
                process_return_val_set_str(&proc->return_val, (ret_eax != 0) ? "true" : "false");
                process_return_val_add_arg(&proc->return_val, (ret_eax != 0) ? "true" : "false");
            } else if (embedded_fmt && (strcmp(embedded_fmt, "char") == 0 || strcmp(embedded_fmt, "c") == 0)) {
                uint8_t cb = (uint8_t)(ret_eax & 0xFF);
                char *c_str = format_char_dynamic(cb);
                char *c_arg = format_char_arg_dynamic(cb);
                proc->return_val.type = PROC_RET_INT;
                process_return_val_set_type_name(&proc->return_val, "char");
                proc->return_val.int_val = (int32_t)ret_eax;
                proc->return_val.raw_eax = ret_eax;
                proc->return_val.raw_edx = ret_edx;
                proc->exit_code = (int32_t)ret_eax;
                if (c_str) {
                    process_return_val_set_str(&proc->return_val, c_str);
                    kfree(c_str);
                }
                if (c_arg) {
                    process_return_val_add_arg(&proc->return_val, c_arg);
                    kfree(c_arg);
                }
            } else if (embedded_fmt && (strcmp(embedded_fmt, "float") == 0 || strcmp(embedded_fmt, "f") == 0)) {
                proc->return_val.type = PROC_RET_INT;
                process_return_val_set_type_name(&proc->return_val, "float");
                proc->return_val.int_val = (int32_t)ret_eax;
                proc->return_val.raw_eax = ret_eax;
                proc->return_val.raw_edx = ret_edx;
                proc->exit_code = (int32_t)ret_eax;
                char *f_str = format_float_dynamic(ret_eax);
                if (f_str) {
                    process_return_val_set_str(&proc->return_val, f_str);
                    process_return_val_add_arg(&proc->return_val, f_str);
                    kfree(f_str);
                }
            } else if (embedded_fmt && (strcmp(embedded_fmt, "double") == 0 || strcmp(embedded_fmt, "d") == 0)) {
                uint64_t d_bits = ((uint64_t)ret_edx << 32) | ret_eax;
                proc->return_val.type = PROC_RET_INT;
                process_return_val_set_type_name(&proc->return_val, "double");
                proc->return_val.int_val = (int32_t)ret_eax;
                proc->return_val.raw_eax = ret_eax;
                proc->return_val.raw_edx = ret_edx;
                proc->exit_code = (int32_t)ret_eax;
                char *d_str = format_double_dynamic(d_bits);
                if (d_str) {
                    process_return_val_set_str(&proc->return_val, d_str);
                    process_return_val_add_arg(&proc->return_val, d_str);
                    kfree(d_str);
                }
            } else if (embedded_fmt && (strcmp(embedded_fmt, "char*") == 0 || strcmp(embedded_fmt, "string") == 0)) {
                proc->return_val.type = PROC_RET_STRING;
                proc->return_val.int_val = 0;
                proc->return_val.raw_eax = ret_eax;
                proc->return_val.raw_edx = ret_edx;
                proc->exit_code = 0;
                process_return_val_set_type_name(&proc->return_val, "char*");
                if (ret_eax == 0) {
                    process_return_val_set_str(&proc->return_val, "NULL");
                    process_return_val_add_arg(&proc->return_val, "NULL");
                } else {
                    process_return_val_set_str(&proc->return_val, "\"");
                    process_return_val_append_str(&proc->return_val, (const char*)ret_eax);
                    process_return_val_append_str(&proc->return_val, "\"");
                    process_return_val_add_arg(&proc->return_val, (const char*)ret_eax);
                }
            } else if (embedded_fmt && embedded_fmt[0] == '{') {
                uint8_t reg_buf[8];
                memcpy(reg_buf, &ret_eax, 4);
                memcpy(reg_buf + 4, &ret_edx, 4);
                proc->return_val.type = PROC_RET_STRUCT;
                process_return_val_set_type_name(&proc->return_val, embedded_fmt);
                process_return_val_set_struct_data(&proc->return_val, reg_buf, 8);
                proc->return_val.raw_eax = ret_eax;
                proc->return_val.raw_edx = ret_edx;
                proc->return_val.int_val = (int32_t)ret_eax;
                proc->exit_code = (int32_t)ret_eax;
                format_struct_fields(&proc->return_val, reg_buf, 8, embedded_fmt);
            } else if (ret_eax == 0) {
                proc->return_val.type = PROC_RET_INT;
                proc->return_val.int_val = 0;
                proc->return_val.raw_eax = 0;
                proc->return_val.raw_edx = ret_edx;
                proc->exit_code = 0;
                process_return_val_set_type_name(&proc->return_val, "int");
                process_return_val_set_str(&proc->return_val, "0");
                process_return_val_add_arg(&proc->return_val, "0");
            } else if (is_valid_string_pointer((const void*)ret_eax, &str_len)) {
                proc->return_val.type = PROC_RET_STRING;
                proc->return_val.int_val = 0;
                proc->return_val.raw_eax = ret_eax;
                proc->return_val.raw_edx = ret_edx;
                proc->exit_code = 0;
                process_return_val_set_type_name(&proc->return_val, "char*");
                process_return_val_set_str(&proc->return_val, "\"");
                process_return_val_append_str(&proc->return_val, (const char*)ret_eax);
                process_return_val_append_str(&proc->return_val, "\"");
                process_return_val_add_arg(&proc->return_val, (const char*)ret_eax);
            } else if (ret_eax >= 0x10000 && memory_map_is_valid_ram(ret_eax, 1) && (ret_eax % 4 == 0) &&
                       (ret_eax >= 0xC0000000 || ret_eax >= 0x01000000)) {
                proc->return_val.type = PROC_RET_POINTER;
                process_return_val_set_type_name(&proc->return_val, "void*");
                proc->return_val.raw_eax = ret_eax;
                proc->return_val.raw_edx = ret_edx;
                proc->return_val.int_val = 0;
                proc->exit_code = 0;
                char *hex_str = format_hex_dynamic(ret_eax);
                if (hex_str) {
                    process_return_val_set_str(&proc->return_val, hex_str);
                    process_return_val_add_arg(&proc->return_val, hex_str);
                    kfree(hex_str);
                }
            } else {
                /* Check if scalar return is a Cyrillic character constant in UTF-8 (e.g. 'ы' = 0xD18B = 53643) */
                if ((ret_eax & 0xFF00) == 0xD000 || (ret_eax & 0xFF00) == 0xD100) {
                    char c_str[5];
                    c_str[0] = '\'';
                    c_str[1] = (char)((ret_eax >> 8) & 0xFF);
                    c_str[2] = (char)(ret_eax & 0xFF);
                    c_str[3] = '\'';
                    c_str[4] = '\0';
                    char c_arg[3];
                    c_arg[0] = (char)((ret_eax >> 8) & 0xFF);
                    c_arg[1] = (char)(ret_eax & 0xFF);
                    c_arg[2] = '\0';
                    proc->return_val.type = PROC_RET_INT;
                    process_return_val_set_type_name(&proc->return_val, "char");
                    proc->return_val.int_val = (int32_t)ret_eax;
                    proc->return_val.raw_eax = ret_eax;
                    proc->return_val.raw_edx = ret_edx;
                    proc->exit_code = (int32_t)ret_eax;
                    process_return_val_set_str(&proc->return_val, c_str);
                    process_return_val_add_arg(&proc->return_val, c_arg);
                } else if ((int32_t)ret_eax >= -128 && (int32_t)ret_eax < 0 && is_char_byte((uint8_t)ret_eax)) {
                    /* Sign-extended 8-bit character return */
                    uint8_t cb = (uint8_t)(ret_eax & 0xFF);
                    char *c_str = format_char_dynamic(cb);
                    char *c_arg = format_char_arg_dynamic(cb);
                    proc->return_val.type = PROC_RET_INT;
                    process_return_val_set_type_name(&proc->return_val, "char");
                    proc->return_val.int_val = (int32_t)ret_eax;
                    proc->return_val.raw_eax = ret_eax;
                    proc->return_val.raw_edx = ret_edx;
                    proc->exit_code = (int32_t)ret_eax;
                    if (c_str) {
                        process_return_val_set_str(&proc->return_val, c_str);
                        kfree(c_str);
                    }
                    if (c_arg) {
                        process_return_val_add_arg(&proc->return_val, c_arg);
                        kfree(c_arg);
                    }
                } else {
                    proc->return_val.type = PROC_RET_INT;
                    process_return_val_set_type_name(&proc->return_val, "int");
                    proc->return_val.int_val = (int32_t)ret_eax;
                    proc->return_val.raw_eax = ret_eax;
                    proc->return_val.raw_edx = ret_edx;
                    proc->exit_code = (int32_t)ret_eax;
                    char *int_str = format_int_dynamic((int32_t)ret_eax);
                    if (int_str) {
                        process_return_val_set_str(&proc->return_val, int_str);
                        process_return_val_add_arg(&proc->return_val, int_str);
                        kfree(int_str);
                    }
                }
            }
        }
    } else {
        proc->exit_code = proc->return_val.int_val;
    }

    if (struct_buf) {
        kfree(struct_buf);
        struct_buf = NULL;
    }

    proc->is_running = 0;
    proc->completed_and_acknowledged = true;
    last_exit_code = proc->exit_code;
    process_return_val_copy(&last_return_val, &proc->return_val);

    if (!vga_is_graphics_mode()) {
        vga_sanitize_text_vram();
    }

    serial_printf("[process] pid=%u trampoline type=%d exit_code=%d str='%s'\n",
                  proc->pid, (int)proc->return_val.type, proc->exit_code,
                  proc->return_val.str_val ? proc->return_val.str_val : "");

    process_yield();

    while (1) {
        process_yield();
    }
}

static uint32_t allocate_pid(void) {
    uint32_t candidate = 1;
    while (1) {
        bool in_use = false;
        process_t *curr = process_list;
        while (curr) {
            if (curr->pid == candidate) {
                in_use = true;
                break;
            }
            curr = curr->next;
        }
        if (!in_use) {
            return candidate;
        }
        candidate++;
    }
}

// Memory allocation tracking
static memory_block_t *allocated_blocks = NULL;
static int block_count = 0;
static int max_blocks = 0;
static uint64_t global_process_heap_used = 0;

static uint64_t get_process_heap_capacity(void) {
    uint64_t total = memory_map_get_total_ram();
    if (total > 0) {
        return total;
    }
    volatile uint32_t *ram_size_ptr = (volatile uint32_t *)0x8FF0;
    uint32_t detected = *ram_size_ptr;
    return (uint64_t)detected;
}

static void log_process_heap_state(const char *stage) {
    uint64_t total = get_process_heap_capacity();
    uint64_t used = global_process_heap_used;
    if (used > total) {
        used = total;
    }

    uint64_t remaining = (used < total) ? (total - used) : 0;
    uint64_t used_pct = (total == 0) ? 0 : ((used * 100ull) / total);

    serial_printf("[process] %s: used=%llu total=%llu used_pct=%llu%% remaining=%llu\n",
                 stage, used, total, used_pct, remaining);
}

/**
 * Add a memory block to tracking
 */
static int add_memory_block(void *base, uint32_t size, uint32_t pid) {
    // Expand array if needed
    if (block_count >= max_blocks) {
        int new_max = max_blocks + 16;
        memory_block_t *new_blocks = kmalloc(new_max * sizeof(memory_block_t));
        if (!new_blocks) return -1;
        
        if (allocated_blocks) {
            memcpy(new_blocks, allocated_blocks, block_count * sizeof(memory_block_t));
            kfree(allocated_blocks);
        }
        allocated_blocks = new_blocks;
        max_blocks = new_max;
    }
    
    allocated_blocks[block_count].base = base;
    allocated_blocks[block_count].size = size;
    allocated_blocks[block_count].pid = pid;
    block_count++;
    
    return 0;
}

/**
 * Find and remove a memory block
 */
static int remove_memory_block(void *base) {
    for (int i = 0; i < block_count; i++) {
        if (allocated_blocks[i].base == base) {
            allocated_blocks[i] = allocated_blocks[block_count - 1];
            block_count--;
            return 0;
        }
    }
    return -1;
}

void process_set_keep_alive(process_t *proc, int enabled) {
    if (proc == NULL) {
        return;
    }

    if (enabled) {
        proc->async_task_count++;
        serial_printf("[process] pid=%u keep_alive++ -> %u\n",
                      proc->pid, proc->async_task_count);
        return;
    }

    if (proc->async_task_count > 0) {
        proc->async_task_count--;
        serial_printf("[process] pid=%u keep_alive-- -> %u\n",
                      proc->pid, proc->async_task_count);
    }

    if (proc->async_task_count == 0) {
        serial_printf("[process] pid=%u final async cleanup\n", proc->pid);
        proc->is_running = 0;
        if (!process_is_batch_active()) {
            process_cleanup(proc);
            log_process_heap_state("after cleanup");
        }
    }
}

/**
 * process_init - Initialize process manager
 */
void process_init(void) {
    // Initialize process memory tracking
    global_process_heap_used = 0;
    block_count = 0;
    max_blocks = 0;
    allocated_blocks = NULL;
    process_sync_shared_context();
    
    serial_printf("[process] Process manager initialized (capacity: %llu MB).\n", get_process_heap_capacity() / (1024 * 1024));
}

/**
 * allocate_process_memory - Allocates memory for a process dynamically
 */
static void *allocate_process_memory(process_t *proc, uint32_t size, uint32_t prot_flags) {
    (void)prot_flags;

    if (proc == NULL || size == 0) {
        return NULL;
    }

    uint64_t heap_capacity = get_process_heap_capacity();
    
    // First, try to find a free block that fits
    for (int i = 0; i < block_count; i++) {
        if (allocated_blocks[i].pid == 0 && allocated_blocks[i].size >= size) {
            // Found a free block that fits
            void *addr = allocated_blocks[i].base;
            uint32_t old_size = allocated_blocks[i].size;
            
            // Mark as allocated
            allocated_blocks[i].pid = proc->pid;
            
            // If the block is larger than needed, split it
            if (old_size > size) {
                // Create a new free block for the remaining space
                if (add_memory_block((uint8_t*)addr + size, old_size - size, 0) < 0) {
                    // Failed to create free block, use the whole block
                    allocated_blocks[i].size = old_size;
                } else {
                    allocated_blocks[i].size = size;
                }
            }
            
            serial_printf("Reused free block at 0x%x, size=%u (was %u)\n", 
                         (uint32_t)addr, size, old_size);
            return addr;
        }
    }
    
    // No suitable free block found, allocate at the end
    if (global_process_heap_used + size > heap_capacity) {
        serial_printf("Process heap exhausted: need %llu, have %llu\n",
                     global_process_heap_used + size, heap_capacity);
        return NULL;
    }

    uint32_t base = PROCESS_HEAP_START + (uint32_t)global_process_heap_used;
    void *addr = (void *)base;

    // Track this allocation
    if (add_memory_block(addr, size, proc->pid) < 0) {
        return NULL;
    }

    global_process_heap_used += size;

    serial_printf("Allocated process memory at end: base=0x%x, size=%u, used=%llu\n",
                 base, size, global_process_heap_used);

    return addr;
}

static void coalesce_free_blocks(void) {
    bool merged = true;
    while (merged) {
        merged = false;
        for (int i = 0; i < block_count; i++) {
            if (allocated_blocks[i].pid != 0) continue;
            uint8_t *end_i = (uint8_t *)allocated_blocks[i].base + allocated_blocks[i].size;

            for (int j = 0; j < block_count; j++) {
                if (i == j || allocated_blocks[j].pid != 0) continue;
                if (end_i == (uint8_t *)allocated_blocks[j].base) {
                    allocated_blocks[i].size += allocated_blocks[j].size;
                    for (int k = j; k < block_count - 1; k++) {
                        allocated_blocks[k] = allocated_blocks[k + 1];
                    }
                    block_count--;
                    merged = true;
                    break;
                }
            }
            if (merged) break;
        }
    }

    // Shrink heap usage if free blocks exist at the top of heap
    for (int i = 0; i < block_count; i++) {
        if (allocated_blocks[i].pid == 0) {
            uint32_t block_top = (uint32_t)allocated_blocks[i].base + allocated_blocks[i].size;
            if (block_top == PROCESS_HEAP_START + (uint32_t)global_process_heap_used) {
                global_process_heap_used -= allocated_blocks[i].size;
                for (int k = i; k < block_count - 1; k++) {
                    allocated_blocks[k] = allocated_blocks[k + 1];
                }
                block_count--;
                i--;
            }
        }
    }
}

/**
 * free_process_memory - Frees up process memory
 * Marks block as free (pid=0) so it can be reused
 */
static void free_process_memory(process_t *proc, void *addr, uint32_t size) {
    (void)proc;
    (void)size;

    // Find the block and mark it as free
    for (int i = 0; i < block_count; i++) {
        if (allocated_blocks[i].base == addr) {
            allocated_blocks[i].pid = 0;  // Mark as free
            serial_printf("Marked memory as free: base=0x%x, size=%u\n", 
                         (uint32_t)addr, allocated_blocks[i].size);
            coalesce_free_blocks();
            return;
        }
    }
    
    serial_printf("Warning: tried to free unknown block at 0x%x\n", (uint32_t)addr);
}

/**
 * setup_arguments - Sets the command line arguments and returns the address of argv
 */
static int setup_arguments(process_t *proc, int argc, char **argv, uint32_t *argv_addr_out) {
    if (argc == 0 || argv == NULL) {
        proc->argc = 0;
        *argv_addr_out = 0;
        return 0;
    }
    
    // No hard argument-count cap: argv is sized to the caller-provided count.
    char **argv_array = kmalloc((argc + 1) * sizeof(char *));
    if (!argv_array) {
        return -1;
    }
    
    // Copy the arguments and save their addresses
    for (int i = 0; i < argc; i++) {
        if (argv[i] == NULL) {
            argc = i;
            break;
        }
        // Allocate memory for the argument string
        size_t arg_len = strlen(argv[i]) + 1;
        char *arg_copy = kmalloc(arg_len);
        if (!arg_copy) {
            return -1;
        }
        strcpy(arg_copy, argv[i]);
        argv_array[i] = arg_copy;
    }
    argv_array[argc] = 0;
    
    // Saving information
    proc->argc = argc;
    *argv_addr_out = (uint32_t)argv_array;  // The address of the argv array in kernel memory
    
    // We save a copy in the process for cleaning
    proc->argv_kernel = argv_array;
    
    return 0;
}

/**
 * setup_stack - Configures the process stack for startup.
 * Start with a modest reserve and let the app request more when it actually needs it.
 */
static uint32_t setup_stack(process_t *proc) {
    const uint32_t stack_size = 256u * 1024u;

    proc->stack_base = kmalloc(stack_size);
    if (proc->stack_base == NULL) {
        return 0;
    }

    proc->stack_start = (uint32_t)proc->stack_base;
    proc->stack_ptr = proc->stack_start + stack_size;
    proc->stack_size = stack_size;
    
    return proc->stack_ptr;
}

int process_adjust_stack_size(process_t *proc, int32_t delta) {
    if (proc == NULL || proc->stack_base == NULL) {
        return -1;
    }

    if (delta == 0) {
        return (int32_t)proc->stack_size;
    }

    if (delta > 0) {
        uint32_t grow = (uint32_t)delta;
        uint64_t heap_capacity = get_process_heap_capacity();
        uint64_t new_used = (uint64_t)global_process_heap_used + grow;

        if (new_used > heap_capacity) {
            serial_printf("[process] stack grow rejected: need %llu, have %llu\n",
                         new_used, heap_capacity);
            return -1;
        }

        void *extra = allocate_process_memory(proc, grow, PROT_READ | PROT_WRITE);
        if (extra == NULL) {
            return -1;
        }

        proc->stack_size += grow;
        proc->stack_ptr = proc->stack_start + proc->stack_size;
        serial_printf("[process] stack grown: pid=%u size=%u top=0x%x\n",
                     proc->pid, proc->stack_size, proc->stack_ptr);
        return (int32_t)proc->stack_size;
    }

    uint32_t shrink = (uint32_t)(-delta);
    if (shrink > proc->stack_size) {
        serial_printf("[process] stack shrink rejected: pid=%u try=%u current=%u\n",
                     proc->pid, shrink, proc->stack_size);
        return -1;
    }

    proc->stack_size -= shrink;
    proc->stack_ptr = proc->stack_start + proc->stack_size;
    if (global_process_heap_used >= shrink) {
        global_process_heap_used -= shrink;
    }

    serial_printf("[process] stack shrunk: pid=%u size=%u top=0x%x\n",
                 proc->pid, proc->stack_size, proc->stack_ptr);
    return (int32_t)proc->stack_size;
}

static int process_call_entry(process_entry_t entry, int argc, char **argv,
                              uint32_t stack_top) {
    int result;
    uint32_t old_stack;

    __asm__ volatile(
        "movl %%esp, %0\n"
        "movl %5, %%esp\n"
        "pushl %3\n"   /* argv */
        "pushl %2\n"   /* argc */
        "call *%4\n"
        "addl $8, %%esp\n"
        "movl %0, %%esp\n"
        : "=m"(old_stack), "=a"(result)
        : "r"(argc), "r"(argv), "r"(entry), "r"(stack_top)
        : "ecx", "edx", "memory");

    return result;
}

/**
 * load_binary_file - Reads an executable file image into memory
 */
static int load_binary_file(const char *path, void **data_out) {
    if (data_out == NULL) {
        return -1;
    }
    
    *data_out = NULL;
    
    // Checking the existence of a file
    struct ipo_inode stat;
    if (!ipo_fs_stat(path, &stat)) {
        printf("File not found: %s\n", path);
        process_set_last_exit_code(127);
        return -1;  // File not found
    }
    
    if ((stat.mode & IPO_INODE_TYPE_DIR) != 0) {
        printf("Cannot execute '%s': is a directory\n", path);
        process_set_last_exit_code(126);
        return -1;  // Path is a directory
    }
    
    if (stat.size == 0) {
        printf("Cannot execute '%s': file is empty\n", path);
        process_set_last_exit_code(126);
        return -1;
    }
    
    serial_printf("Loading binary file: %s, size: %d bytes\n", path, stat.size);
    
    // For large files, we use step-by-step loading.
    void *binary_image = kmalloc(stat.size);
    if (binary_image == NULL) {
        printf("Memory allocation failed for size: %d\n", stat.size);
        process_set_last_exit_code(126);
        return -3;  // Memory allocation failed
    }
    
    // Read the file in 64KB chunks directly using inode metadata
    uint32_t total_read = 0;
    uint32_t chunk_size = 64 * 1024;  // 64KB
    
    while (total_read < stat.size) {
        uint32_t to_read = stat.size - total_read;
        if (to_read > chunk_size) {
            to_read = chunk_size;
        }
        
        int bytes_read = inode_read_bytes(&stat, (uint8_t*)binary_image + total_read, to_read, total_read);
        
        if (bytes_read <= 0) {
            kfree(binary_image);
            printf("Read failed at offset %d, read %d bytes\n", total_read, bytes_read);
            return -4;  // Read failed
        }
        
        total_read += (uint32_t)bytes_read;
        serial_printf("Read chunk: %d bytes, total: %d/%d\n", bytes_read, total_read, stat.size);
    }

    *data_out = binary_image;
    serial_printf("File loaded successfully (%d bytes)\n", stat.size);
    return (int)stat.size;
}


/**
 * process_cleanup - Frees up process resources
 */
void process_cleanup(process_t *proc) {
    if (!proc) return;
    
    serial_printf("Cleaning up process %d\n", proc->pid);
    audio_stop_all();

    if (proc->async_task_count > 0) {
        proc->is_running = 0;
        return;
    }
    
    if (currently_mapped_app == proc) {
        currently_mapped_app = NULL;
    }

    if (batch_displayed_fg == proc) {
        batch_displayed_fg = NULL;
    }

    if (proc->binary_storage) {
        kfree(proc->binary_storage);
        proc->binary_storage = NULL;
    }

    if (proc->return_format) {
        kfree(proc->return_format);
        proc->return_format = NULL;
    }

    if (proc->binary_base) {
        proc->binary_base = NULL;
    }

    if (proc->stack_base) {
        kfree(proc->stack_base);
        proc->stack_base = NULL;
    }

    if (proc->name) {
        kfree(proc->name);
        proc->name = NULL;
    }

    if (proc->text_vram_backup) {
        kfree(proc->text_vram_backup);
        proc->text_vram_backup = NULL;
    }

    if (proc->scroll_top_buffer) {
        kfree(proc->scroll_top_buffer);
        proc->scroll_top_buffer = NULL;
    }

    if (proc->scroll_bottom_buffer) {
        kfree(proc->scroll_bottom_buffer);
        proc->scroll_bottom_buffer = NULL;
    }
    proc->scroll_top_count = 0;
    proc->scroll_bottom_count = 0;

    if (proc->gfx_vram_backup) {
        kfree(proc->gfx_vram_backup);
        proc->gfx_vram_backup = NULL;
    }

    if (proc->gfx_palette_backup) {
        kfree(proc->gfx_palette_backup);
        proc->gfx_palette_backup = NULL;
    }

    if (proc->text_output_buf) {
        kfree(proc->text_output_buf);
        proc->text_output_buf = NULL;
        proc->text_output_len = 0;
        proc->text_output_cap = 0;
    }
    
    // Freeing arguments
    if (proc->argv_kernel) {
        // argv_kernel contains a pointer to the argv array
        uint32_t *argv_array = (uint32_t *)proc->argv_kernel;
        
        // Freeing up argument structures
        for (int i = 0; i < proc->argc; i++) {
            if (argv_array[i]) {
                kfree((void*)argv_array[i]);
            }
        }
        
        // Freeing the array itself
        kfree(argv_array);
        proc->argv_kernel = NULL;
    }
    
    // Remove from the list of processes
    if (process_list == proc) {
        process_list = proc->next;
    } else {
        process_t *prev = process_list;
        while (prev && prev->next != proc) {
            prev = prev->next;
        }
        if (prev) {
            prev->next = proc->next;
        }
    }

    if (current_process == proc) {
        current_process = NULL;
    }

    process_return_val_free_fields(&proc->return_val);
    kfree(proc);

    if (process_list == NULL) {
        global_process_heap_used = 0;
        block_count = 0;
        syscall_reset_user_heap();
        keyboard_set_app_input_mode(false);
        terminal_unlock_input();
        serial_printf("[process] All processes terminated, heap reset to 0\n");
    }
}

void process_crash_exit(void) {
    process_t *proc = current_process;
    if (proc != NULL) {
        proc->is_running = 0;
        proc->completed_and_acknowledged = true;
        proc->exit_code = 139;
        last_exit_code = 139;

        if (!vga_is_graphics_mode()) {
            vga_sanitize_text_vram();
        }

        serial_printf("[process] pid=%u terminated by CPU exception\n", proc->pid);
    }

    while (1) {
        process_yield();
    }
}


typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf32_header_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} elf32_program_header_t;

/**
 * universal_exec_load - Universally loads any file as an executable image.
 *
 * Supports flat binary files and 32-bit ELF executables, properly allocating
 * and zeroing .bss sections so that memory swaps between processes preserve
 * complete process state and global data.
 */
static bool is_text_file(const uint8_t *data, uint32_t size) {
    if (!data || size == 0) return true;
    uint32_t check_len = size < 512 ? size : 512;
    uint32_t i = 0;
    while (i < check_len) {
        uint8_t c = data[i];
        if (c == 0x00) {
            return false;
        }
        if (c == '\r' || c == '\n' || c == '\t' || c == '\f') {
            i++;
            continue;
        }
        if (c >= 0x20 && c <= 0x7E) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 < check_len && (data[i + 1] & 0xC0) == 0x80) {
                i += 2;
                continue;
            }
            return false;
        }
        if (c >= 0xE0 && c <= 0xEF) {
            if (i + 2 < check_len && (data[i + 1] & 0xC0) == 0x80 && (data[i + 2] & 0xC0) == 0x80) {
                i += 3;
                continue;
            }
            return false;
        }
        if (c >= 0xF0 && c <= 0xF4) {
            if (i + 3 < check_len && (data[i + 1] & 0xC0) == 0x80 && (data[i + 2] & 0xC0) == 0x80 && (data[i + 3] & 0xC0) == 0x80) {
                i += 4;
                continue;
            }
            return false;
        }
        return false;
    }
    return true;
}

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} elf32_section_header_t;

#define DW_TAG_array_type       0x01
#define DW_TAG_enumeration_type 0x04
#define DW_TAG_structure_type   0x13
#define DW_TAG_typedef          0x16
#define DW_TAG_subprogram       0x2e
#define DW_TAG_base_type        0x24
#define DW_TAG_pointer_type     0x0f
#define DW_TAG_reference_type   0x10
#define DW_TAG_member           0x0d
#define DW_TAG_subrange_type    0x21
#define DW_TAG_const_type       0x26
#define DW_TAG_volatile_type    0x35

#define DW_AT_name              0x03
#define DW_AT_type              0x49
#define DW_AT_upper_bound       0x2f

#define DW_FORM_addr            0x01
#define DW_FORM_block2          0x03
#define DW_FORM_block4          0x04
#define DW_FORM_data2           0x05
#define DW_FORM_data4           0x06
#define DW_FORM_data8           0x07
#define DW_FORM_string          0x08
#define DW_FORM_block           0x09
#define DW_FORM_block1          0x0a
#define DW_FORM_data1           0x0b
#define DW_FORM_flag            0x0c
#define DW_FORM_sdata           0x0d
#define DW_FORM_strp            0x0e
#define DW_FORM_udata           0x0f
#define DW_FORM_ref_addr        0x10
#define DW_FORM_ref1            0x11
#define DW_FORM_ref2            0x12
#define DW_FORM_ref4            0x13
#define DW_FORM_ref8            0x14
#define DW_FORM_ref_udata       0x15
#define DW_FORM_indirect        0x16
#define DW_FORM_sec_offset      0x17
#define DW_FORM_exprloc         0x18
#define DW_FORM_flag_present    0x19
#define DW_FORM_strx            0x1a
#define DW_FORM_addrx           0x1b
#define DW_FORM_line_strp       0x1f
#define DW_FORM_implicit_const  0x21

static uint64_t dwarf_read_uleb128(const uint8_t **p, const uint8_t *end) {
    uint64_t res = 0;
    int shift = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        res |= (uint64_t)(b & 0x7f) << shift;
        if ((b & 0x80) == 0) break;
        shift += 7;
        if (shift >= 64) break;
    }
    return res;
}

static int64_t dwarf_read_sleb128(const uint8_t **p, const uint8_t *end) {
    int64_t res = 0;
    int shift = 0;
    uint8_t b = 0;
    while (*p < end) {
        b = *(*p)++;
        res |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if ((b & 0x80) == 0) break;
        if (shift >= 64) break;
    }
    if ((shift < 64) && (b & 0x40)) {
        res |= -((int64_t)1 << shift);
    }
    return res;
}

typedef struct {
    uint64_t name;
    uint64_t form;
    int64_t implicit_val;
} dwarf_attr_t;

typedef struct {
    uint64_t code;
    uint64_t tag;
    uint8_t has_children;
    dwarf_attr_t attrs[32];
    int attr_count;
} dwarf_abbrev_t;

typedef struct dwarf_die {
    uint32_t offset;
    uint32_t tag;
    const char *name;
    uint32_t type_ref;
    uint32_t upper_bound;
    struct dwarf_die *first_child;
    struct dwarf_die *next_sibling;
} dwarf_die_t;

static dwarf_abbrev_t *dwarf_find_abbrev(dwarf_abbrev_t *abbrevs, int count, uint64_t code) {
    for (int i = 0; i < count; i++) {
        if (abbrevs[i].code == code) return &abbrevs[i];
    }
    return NULL;
}

static dwarf_die_t *dwarf_find_die(dwarf_die_t *dies, int count, uint32_t offset) {
    for (int i = 0; i < count; i++) {
        if (dies[i].offset == offset) return &dies[i];
    }
    return NULL;
}

static inline void dwarf_append_str(char *out, const char *s, size_t out_sz) {
    size_t len = strlen(out);
    size_t slen = strlen(s);
    if (len + slen + 1 < out_sz) {
        memcpy(out + len, s, slen + 1);
    }
}

static void dwarf_die_to_fmt(dwarf_die_t *dies, int die_count, dwarf_die_t *die, char *out, size_t out_sz) {
    if (!die) {
        dwarf_append_str(out, "void", out_sz);
        return;
    }
    if (die->tag == DW_TAG_typedef || die->tag == DW_TAG_const_type || die->tag == DW_TAG_volatile_type) {
        dwarf_die_to_fmt(dies, die_count, dwarf_find_die(dies, die_count, die->type_ref), out, out_sz);
    } else if (die->tag == DW_TAG_base_type) {
        const char *n = die->name ? die->name : "int";
        if (strcmp(n, "char") == 0 || strcmp(n, "signed char") == 0 || strcmp(n, "unsigned char") == 0) dwarf_append_str(out, "char", out_sz);
        else if (strcmp(n, "short") == 0 || strcmp(n, "short int") == 0 || strcmp(n, "signed short") == 0) dwarf_append_str(out, "short", out_sz);
        else if (strcmp(n, "unsigned short") == 0 || strcmp(n, "short unsigned int") == 0) dwarf_append_str(out, "ushort", out_sz);
        else if (strcmp(n, "int") == 0 || strcmp(n, "signed int") == 0) dwarf_append_str(out, "int", out_sz);
        else if (strcmp(n, "unsigned int") == 0 || strcmp(n, "uint") == 0) dwarf_append_str(out, "uint", out_sz);
        else if (strcmp(n, "long") == 0 || strcmp(n, "long int") == 0 || strcmp(n, "signed long") == 0) dwarf_append_str(out, "long", out_sz);
        else if (strcmp(n, "unsigned long") == 0 || strcmp(n, "long unsigned int") == 0) dwarf_append_str(out, "ulong", out_sz);
        else if (strcmp(n, "long long") == 0 || strcmp(n, "long long int") == 0 || strcmp(n, "signed long long") == 0) dwarf_append_str(out, "long long", out_sz);
        else if (strcmp(n, "unsigned long long") == 0 || strcmp(n, "long long unsigned int") == 0) dwarf_append_str(out, "ulong long", out_sz);
        else if (strcmp(n, "float") == 0) dwarf_append_str(out, "float", out_sz);
        else if (strcmp(n, "double") == 0) dwarf_append_str(out, "double", out_sz);
        else if (strcmp(n, "_Bool") == 0 || strcmp(n, "bool") == 0) dwarf_append_str(out, "bool", out_sz);
        else dwarf_append_str(out, n, out_sz);
    } else if (die->tag == DW_TAG_pointer_type) {
        dwarf_die_t *sub = dwarf_find_die(dies, die_count, die->type_ref);
        if (sub && sub->tag == DW_TAG_base_type && sub->name && strstr(sub->name, "char")) {
            dwarf_append_str(out, "char*", out_sz);
        } else {
            dwarf_append_str(out, "void*", out_sz);
        }
    } else if (die->tag == DW_TAG_array_type) {
        dwarf_die_t *sub = dwarf_find_die(dies, die_count, die->type_ref);
        uint32_t cnt = die->first_child ? (die->first_child->upper_bound + 1) : 1;
        char cnt_str[16];
        snprintf(cnt_str, sizeof(cnt_str), "[%u", cnt);
        dwarf_append_str(out, cnt_str, out_sz);
        dwarf_die_to_fmt(dies, die_count, sub, out, out_sz);
        dwarf_append_str(out, "]", out_sz);
    } else if (die->tag == DW_TAG_structure_type) {
        dwarf_append_str(out, "{ ", out_sz);
        bool first = true;
        for (dwarf_die_t *m = die->first_child; m; m = m->next_sibling) {
            if (m->tag == DW_TAG_member) {
                if (!first) dwarf_append_str(out, ", ", out_sz);
                first = false;
                dwarf_die_to_fmt(dies, die_count, dwarf_find_die(dies, die_count, m->type_ref), out, out_sz);
                if (m->name) {
                    dwarf_append_str(out, " ", out_sz);
                    dwarf_append_str(out, m->name, out_sz);
                }
            }
        }
        dwarf_append_str(out, " }", out_sz);
    } else if (die->tag == DW_TAG_enumeration_type) {
        dwarf_append_str(out, "int", out_sz);
    } else if (die->tag == DW_TAG_reference_type) {
        dwarf_append_str(out, "void*", out_sz);
    } else {
        dwarf_append_str(out, "int", out_sz);
    }
}

static char *dwarf_extract_main_return_format(const uint8_t *file_data, uint32_t file_size) {
    if (!file_data || file_size < sizeof(elf32_header_t)) return NULL;
    const elf32_header_t *eh = (const elf32_header_t *)file_data;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
        return NULL;
    }
    if (eh->e_shoff == 0 || eh->e_shnum == 0 || eh->e_shstrndx >= eh->e_shnum) return NULL;
    if (eh->e_shoff + (uint32_t)eh->e_shnum * sizeof(elf32_section_header_t) > file_size) return NULL;

    const elf32_section_header_t *shdr = (const elf32_section_header_t *)(file_data + eh->e_shoff);
    uint32_t shstr_off = shdr[eh->e_shstrndx].sh_offset;
    uint32_t shstr_sz = shdr[eh->e_shstrndx].sh_size;
    if (shstr_off + shstr_sz > file_size) return NULL;
    const char *shstrtab = (const char *)(file_data + shstr_off);

    const uint8_t *debug_info = NULL; uint32_t debug_info_sz = 0;
    const uint8_t *debug_abbrev = NULL; uint32_t debug_abbrev_sz = 0;
    const char *debug_str = NULL; uint32_t debug_str_sz = 0;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (shdr[i].sh_name >= shstr_sz) continue;
        const char *name = shstrtab + shdr[i].sh_name;
        if (shdr[i].sh_offset + shdr[i].sh_size > file_size) continue;

        if (strcmp(name, ".debug_info") == 0) {
            debug_info = file_data + shdr[i].sh_offset;
            debug_info_sz = shdr[i].sh_size;
        } else if (strcmp(name, ".debug_abbrev") == 0) {
            debug_abbrev = file_data + shdr[i].sh_offset;
            debug_abbrev_sz = shdr[i].sh_size;
        } else if (strcmp(name, ".debug_str") == 0) {
            debug_str = (const char *)(file_data + shdr[i].sh_offset);
            debug_str_sz = shdr[i].sh_size;
        }
    }

    if (!debug_info || !debug_abbrev || debug_info_sz < 11 || debug_abbrev_sz == 0) {
        return NULL;
    }

    int max_abbrevs = 128;
    dwarf_abbrev_t *abbrevs = (dwarf_abbrev_t *)kmalloc(max_abbrevs * sizeof(dwarf_abbrev_t));
    int max_dies = 512;
    dwarf_die_t *dies = (dwarf_die_t *)kmalloc(max_dies * sizeof(dwarf_die_t));

    if (!abbrevs || !dies) {
        if (abbrevs) kfree(abbrevs);
        if (dies) kfree(dies);
        return NULL;
    }

    int abbrev_count = 0;
    const uint8_t *ab_ptr = debug_abbrev;
    const uint8_t *ab_end = debug_abbrev + debug_abbrev_sz;

    while (ab_ptr < ab_end && abbrev_count < max_abbrevs) {
        uint64_t code = dwarf_read_uleb128(&ab_ptr, ab_end);
        if (code == 0) continue;
        dwarf_abbrev_t *ab = &abbrevs[abbrev_count++];
        ab->code = code;
        ab->tag = dwarf_read_uleb128(&ab_ptr, ab_end);
        if (ab_ptr >= ab_end) break;
        ab->has_children = *ab_ptr++;
        ab->attr_count = 0;
        while (ab_ptr < ab_end) {
            uint64_t aname = dwarf_read_uleb128(&ab_ptr, ab_end);
            uint64_t aform = dwarf_read_uleb128(&ab_ptr, ab_end);
            if (aname == 0 && aform == 0) break;
            int64_t ival = 0;
            if (aform == DW_FORM_implicit_const) {
                ival = dwarf_read_sleb128(&ab_ptr, ab_end);
            }
            if (ab->attr_count < 32) {
                ab->attrs[ab->attr_count].name = aname;
                ab->attrs[ab->attr_count].form = aform;
                ab->attrs[ab->attr_count].implicit_val = ival;
                ab->attr_count++;
            }
        }
    }

    const uint8_t *cu_ptr = debug_info;
    const uint8_t *info_end = debug_info + debug_info_sz;
    int die_count = 0;
    dwarf_die_t *main_die = NULL;

    while (cu_ptr < info_end && die_count < max_dies) {
        if (cu_ptr + 4 > info_end) break;
        uint32_t unit_len = *(const uint32_t *)cu_ptr; cu_ptr += 4;
        if (unit_len == 0 || unit_len == 0xFFFFFFFF) break;
        const uint8_t *cu_end = cu_ptr + unit_len;
        if (cu_end > info_end) cu_end = info_end;

        if (cu_ptr + 2 > cu_end) break;
        uint16_t version = *(const uint16_t *)cu_ptr; cu_ptr += 2;
        if (version == 5) {
            if (cu_ptr + 6 > cu_end) break;
            cu_ptr += 2;
            cu_ptr += 4;
        } else {
            if (cu_ptr + 5 > cu_end) break;
            cu_ptr += 4;
            cu_ptr += 1;
        }

        dwarf_die_t *parent_stack[32];
        int depth = 0;

        while (cu_ptr < cu_end && die_count < max_dies) {
            uint32_t die_off = (uint32_t)(cu_ptr - debug_info);
            uint64_t code = dwarf_read_uleb128(&cu_ptr, cu_end);
            if (code == 0) {
                if (depth > 0) depth--;
                continue;
            }

            dwarf_abbrev_t *ab = dwarf_find_abbrev(abbrevs, abbrev_count, code);
            if (!ab) break;

            dwarf_die_t *die = &dies[die_count++];
            die->offset = die_off;
            die->tag = ab->tag;
            die->name = NULL;
            die->type_ref = 0;
            die->upper_bound = 0;
            die->first_child = NULL;
            die->next_sibling = NULL;

            if (depth > 0) {
                dwarf_die_t *p = parent_stack[depth - 1];
                if (!p->first_child) p->first_child = die;
                else {
                    dwarf_die_t *s = p->first_child;
                    while (s->next_sibling) s = s->next_sibling;
                    s->next_sibling = die;
                }
            }

            for (int i = 0; i < ab->attr_count && cu_ptr < cu_end; i++) {
                uint64_t name = ab->attrs[i].name;
                uint64_t form = ab->attrs[i].form;

                const char *val_str = NULL;
                uint32_t val_ref = 0;
                uint32_t val_u32 = 0;

                if (form == DW_FORM_strp) {
                    if (cu_ptr + 4 <= cu_end) {
                        uint32_t off = *(const uint32_t *)cu_ptr; cu_ptr += 4;
                        if (debug_str && off < debug_str_sz) val_str = debug_str + off;
                    }
                } else if (form == DW_FORM_string) {
                    val_str = (const char *)cu_ptr;
                    cu_ptr += strlen(val_str) + 1;
                } else if (form == DW_FORM_ref4) {
                    if (cu_ptr + 4 <= cu_end) { val_ref = *(const uint32_t *)cu_ptr; cu_ptr += 4; }
                } else if (form == DW_FORM_data1 || form == DW_FORM_flag || form == DW_FORM_ref1) {
                    if (cu_ptr < cu_end) val_u32 = *cu_ptr++;
                } else if (form == DW_FORM_data2 || form == DW_FORM_ref2) {
                    if (cu_ptr + 2 <= cu_end) { val_u32 = *(const uint16_t *)cu_ptr; cu_ptr += 2; }
                } else if (form == DW_FORM_data4 || form == DW_FORM_sec_offset || form == DW_FORM_addr || form == DW_FORM_line_strp) {
                    if (cu_ptr + 4 <= cu_end) { val_u32 = *(const uint32_t *)cu_ptr; cu_ptr += 4; }
                } else if (form == DW_FORM_data8 || form == DW_FORM_ref8) {
                    if (cu_ptr + 8 <= cu_end) cu_ptr += 8;
                } else if (form == DW_FORM_exprloc || form == DW_FORM_block1) {
                    if (cu_ptr < cu_end) { uint8_t len = *cu_ptr++; cu_ptr += len; }
                } else if (form == DW_FORM_block) {
                    uint64_t len = dwarf_read_uleb128(&cu_ptr, cu_end);
                    cu_ptr += len;
                } else if (form == DW_FORM_udata || form == DW_FORM_ref_udata) {
                    val_u32 = (uint32_t)dwarf_read_uleb128(&cu_ptr, cu_end);
                } else if (form == DW_FORM_sdata) {
                    dwarf_read_sleb128(&cu_ptr, cu_end);
                } else if (form == DW_FORM_flag_present || form == DW_FORM_implicit_const) {
                }

                if (name == DW_AT_name && val_str) die->name = val_str;
                if (name == DW_AT_type && val_ref) die->type_ref = val_ref;
                if (name == DW_AT_upper_bound) die->upper_bound = val_u32;
            }

            if (ab->has_children) {
                if (depth < 32) parent_stack[depth++] = die;
            }

            if (die->tag == DW_TAG_subprogram && die->name && strcmp(die->name, "main") == 0) {
                main_die = die;
            }
        }
        cu_ptr = cu_end;
    }

    char *result = NULL;
    if (main_die) {
        if (main_die->type_ref == 0) {
            result = kstrdup("void");
        } else {
            dwarf_die_t *ret_type = dwarf_find_die(dies, die_count, main_die->type_ref);
            if (ret_type) {
                char fmt_buf[512] = {0};
                dwarf_die_to_fmt(dies, die_count, ret_type, fmt_buf, sizeof(fmt_buf));
                if (fmt_buf[0] != '\0') {
                    result = kstrdup(fmt_buf);
                }
            }
        }
    }

    kfree(abbrevs);
    kfree(dies);
    return result;
}

static int universal_exec_load(process_t *proc, const uint8_t *file_data, uint32_t file_size) {
    if (!proc || !file_data || file_size == 0) {
        return -1;
    }

    uint32_t total_memsz = file_size;
    uint32_t entry_point = (uint32_t)PROCESS_HEAP_START;
    bool is_elf = (file_size >= sizeof(elf32_header_t) &&
                   file_data[0] == 0x7F && file_data[1] == 'E' &&
                   file_data[2] == 'L'  && file_data[3] == 'F');

    if (!is_elf && is_text_file(file_data, file_size)) {
        serial_printf("[universal_exec_load] Rejected text file '%s' (not an executable binary)\n",
                      proc->name ? proc->name : "unknown");
        return -1;
    }

    if (is_elf) {
        proc->return_format = dwarf_extract_main_return_format(file_data, file_size);
        if (proc->return_format) {
            serial_printf("[universal_exec_load] DWARF return format: '%s'\n", proc->return_format);
        }
        const elf32_header_t *elf = (const elf32_header_t *)file_data;
        if (elf->e_phoff > 0 && elf->e_phnum > 0 &&
            elf->e_phoff + (uint32_t)elf->e_phnum * sizeof(elf32_program_header_t) <= file_size) {
            uint32_t max_vaddr = PROCESS_HEAP_START;
            const elf32_program_header_t *ph = (const elf32_program_header_t *)(file_data + elf->e_phoff);
            for (uint16_t i = 0; i < elf->e_phnum; i++) {
                if (ph[i].p_type == 1 /* PT_LOAD */) {
                    uint32_t seg_end = ph[i].p_vaddr + ph[i].p_memsz;
                    if (seg_end > max_vaddr) {
                        max_vaddr = seg_end;
                    }
                }
            }
            if (max_vaddr > PROCESS_HEAP_START) {
                total_memsz = max_vaddr - PROCESS_HEAP_START;
            }
            if (elf->e_entry >= PROCESS_HEAP_START) {
                entry_point = elf->e_entry;
            }
        }
    }

    /* 4KB page align total_memsz */
    total_memsz = (total_memsz + 4095u) & ~4095u;
    if (total_memsz > UINT32_MAX - PROCESS_HEAP_START) {
        return -1;
    }

    proc->binary_storage = kmalloc(total_memsz);
    if (!proc->binary_storage) {
        return -1;
    }
    memset(proc->binary_storage, 0, total_memsz);

    uint32_t data_off = 0;
    uint32_t data_sz = total_memsz;
    if (is_elf) {
        const elf32_header_t *elf = (const elf32_header_t *)file_data;
        const elf32_program_header_t *ph = (const elf32_program_header_t *)(file_data + elf->e_phoff);
        for (uint16_t i = 0; i < elf->e_phnum; i++) {
            if (ph[i].p_type == 1 /* PT_LOAD */ && ph[i].p_filesz > 0) {
                if (ph[i].p_vaddr >= PROCESS_HEAP_START) {
                    uint32_t off = ph[i].p_vaddr - PROCESS_HEAP_START;
                    if (ph[i].p_offset + ph[i].p_filesz <= file_size &&
                        off + ph[i].p_filesz <= total_memsz) {
                        memcpy((uint8_t *)proc->binary_storage + off, file_data + ph[i].p_offset, ph[i].p_filesz);
                    }
                    if (ph[i].p_flags & 2 /* PF_W: writeable data */) {
                        data_off = off;
                        data_sz = ph[i].p_memsz;
                    }
                }
            }
        }
    } else {
        memcpy(proc->binary_storage, file_data, file_size);
    }

    proc->binary_base = (void *)PROCESS_HEAP_START;
    proc->binary_size = total_memsz;
    proc->entry_point = entry_point;
    proc->data_offset = data_off;
    proc->data_size = data_sz;
    proc->code_mapped = false;

    serial_printf("[universal_exec_load] Universally loaded: file_size=%u, mem_size=%u, data_off=%u, data_sz=%u, entry=0x%x\n",
                  file_size, total_memsz, proc->data_offset, proc->data_size, proc->entry_point);

    return 0;
}

/**
 * process_spawn - Creates, loads, and initializes a process ready to run
 */
process_t *process_spawn(const char *path, int argc, char **argv) {
    if (path == NULL) {
        return NULL;
    }

    char *resolved = NULL;
    const char *actual_path = path;
    if (path[0] != '/') {
        resolved = resolve_command_path(path);
        if (resolved != NULL) {
            actual_path = resolved;
        }
    }

    serial_printf("process_spawn: %s (actual=%s), argc=%d\n", path, actual_path, argc);

    // Read file contents into memory
    void *binary_image = NULL;
    int size = load_binary_file(actual_path, &binary_image);
    if (size < 0) {
        if (resolved != NULL) kfree(resolved);
        return NULL;
    }

    // Check for shebang (#!) script
    static int shebang_depth = 0;
    if (size >= 2 && ((const uint8_t *)binary_image)[0] == '#' && ((const uint8_t *)binary_image)[1] == '!') {
        if (shebang_depth >= 4) {
            printf("Cannot execute '%s': too many levels of interpreter recursion\n", actual_path);
            kfree(binary_image);
            if (resolved != NULL) kfree(resolved);
            return NULL;
        }

        const char *data = (const char *)binary_image;
        size_t line_len = 0;
        while (2 + line_len < (size_t)size && data[2 + line_len] != '\r' && data[2 + line_len] != '\n') {
            line_len++;
        }
        char *line = kmalloc(line_len + 1);
        if (!line) {
            kfree(binary_image);
            if (resolved != NULL) kfree(resolved);
            return NULL;
        }
        memcpy(line, data + 2, line_len);
        line[line_len] = '\0';
        kfree(binary_image);

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') {
            printf("Cannot execute '%s': empty interpreter in shebang\n", actual_path);
            kfree(line);
            if (resolved != NULL) kfree(resolved);
            return NULL;
        }

        char *interp = p;
        char *interp_arg = NULL;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) {
            *p++ = '\0';
            while (*p == ' ' || *p == '\t') p++;
            if (*p) {
                interp_arg = p;
                char *end = interp_arg + strlen(interp_arg) - 1;
                while (end >= interp_arg && (*end == ' ' || *end == '\t')) {
                    *end-- = '\0';
                }
            }
        }

        char *interp_path = resolve_command_path(interp);
        if (!interp_path) {
            printf("Interpreter '%s' not found for '%s'\n", interp, actual_path);
            kfree(line);
            if (resolved != NULL) kfree(resolved);
            return NULL;
        }

        int new_cap = argc + 4;
        char **new_argv = kmalloc((size_t)new_cap * sizeof(char *));
        if (!new_argv) {
            kfree(interp_path);
            kfree(line);
            if (resolved != NULL) kfree(resolved);
            return NULL;
        }

        int new_argc = 0;
        new_argv[new_argc++] = interp;
        if (interp_arg && *interp_arg) {
            new_argv[new_argc++] = interp_arg;
        }
        new_argv[new_argc++] = (char *)actual_path;
        for (int i = 1; i < argc; i++) {
            new_argv[new_argc++] = argv[i];
        }
        new_argv[new_argc] = NULL;

        shebang_depth++;
        process_t *proc = process_spawn(interp_path, new_argc, new_argv);
        shebang_depth--;

        kfree(interp_path);
        kfree(new_argv);
        kfree(line);
        if (resolved != NULL) kfree(resolved);
        return proc;
    }

    if (process_list == NULL) {
        global_process_heap_used = 0;
        block_count = 0;
    }

    log_process_heap_state("before spawn");

    // Creating process structure
    process_t *proc = kmalloc(sizeof(process_t));
    if (!proc) {
        printf("Failed to allocate process structure\n");
        kfree(binary_image);
        if (resolved != NULL) kfree(resolved);
        return NULL;
    }

    memset(proc, 0, sizeof(process_t));
    proc->pid = allocate_pid();
    proc->is_running = 1;
    proc->name = kstrdup(actual_path);
    proc->wants_graphics = false;
    uint32_t heap_slot = (proc->pid > 0) ? ((proc->pid - 1) % 7) : 0;
    proc->user_heap_break = 0x08000000u + heap_slot * 0x01000000u; /* Isolated 16MB slice per PID, fits within 256MB RAM */

    proc->next = process_list;
    process_list = proc;

    // Load executable via the Universal Executable Loader
    int load_res = universal_exec_load(proc, (const uint8_t *)binary_image, (uint32_t)size);
    kfree(binary_image);
    if (resolved != NULL) kfree(resolved);

    if (load_res < 0) {
        printf("Cannot execute '%s': not an executable format\n", actual_path);
        process_set_last_exit_code(126);
        process_cleanup(proc);
        return NULL;
    }

    // Setting up arguments - argv is allocated in kernel memory
    uint32_t argv_addr = 0;
    if (setup_arguments(proc, argc, argv, &argv_addr) < 0) {
        printf("Failed to setup arguments\n");
        process_set_last_exit_code(126);
        process_cleanup(proc);
        return NULL;
    }

    // Setting up the stack for calling main()
    if (setup_stack(proc) == 0) {
        printf("Failed to allocate process stack\n");
        process_set_last_exit_code(126);
        process_cleanup(proc);
        return NULL;
    }

    // Defensive validation: a broken application must never crash the kernel.
    if (proc->binary_base == NULL || proc->binary_size == 0 ||
        proc->entry_point < PROCESS_HEAP_START) {
        printf("[process] pid=%u: invalid app image, closing process safely\n", proc->pid);
        process_set_last_exit_code(126);
        process_cleanup(proc);
        return NULL;
    }

    // Set up trampoline stack frame for cooperative multitasking
    uint32_t *sp = (uint32_t *)proc->stack_ptr;
    sp = (uint32_t *)((uint32_t)sp & ~15u);
    sp[-1] = (uint32_t)process_trampoline;
    sp[-2] = 0;       // ebp
    sp[-3] = 0;       // ebx
    sp[-4] = 0;       // esi
    sp[-5] = 0;       // edi
    sp[-6] = 0x02;    // eflags (reserved bit 1=1, IF=0)
    proc->stack_ptr = (uint32_t)&sp[-6];

    serial_printf("Process %d ready: entry=0x%x, argc=%d\n",
           proc->pid, proc->entry_point, proc->argc);

    return proc;
}

static int *last_batch_exit_codes = NULL;
static int last_batch_exit_codes_cap = 0;
static int last_batch_count = 0;

int process_get_batch_exit_code(int idx) {
    if (idx >= 0 && idx < last_batch_count && last_batch_exit_codes) {
        return last_batch_exit_codes[idx];
    }
    return 0;
}

/**
 * process_run_batch_windows - Runs all processes in one batch with window grouping.
 * Processes with the same window_id share a single virtual text screen (workspace).
 * Processes with unique window_ids each get their own workspace.
 * All processes execute truly in parallel in one batch loop.
 */
int process_run_batch_windows(process_t **procs, int count, const int *window_ids) {
    if (!procs || count <= 0 || !window_ids) return -1;

    /* Assign window_id to every process */
    for (int i = 0; i < count; i++) {
        if (procs[i]) {
            procs[i]->window_id = window_ids[i];
            procs[i]->text_screen_shared = false;
        }
    }

    /* Initialize text screens: first process in each window group owns the buffers,
       subsequent processes in the same group share them. */
    for (int i = 0; i < count; i++) {
        if (!procs[i]) continue;
        int wid = procs[i]->window_id;

        /* Find the first process in this window group */
        process_t *owner = NULL;
        for (int k = 0; k < i; k++) {
            if (procs[k] && procs[k]->window_id == wid) {
                owner = procs[k];
                break;
            }
        }

        if (owner == NULL) {
            /* First process in this group — allocate its own text screen */
            process_init_text_screen(procs[i]);
        } else {
            /* Share buffers from the owner */
            procs[i]->text_vram_backup = owner->text_vram_backup;
            procs[i]->gfx_vram_backup = owner->gfx_vram_backup;
            procs[i]->gfx_palette_backup = owner->gfx_palette_backup;
            procs[i]->text_cursor_pos = owner->text_cursor_pos;
            procs[i]->text_cursor_visible = owner->text_cursor_visible;
            procs[i]->scroll_top_buffer = owner->scroll_top_buffer;
            procs[i]->scroll_bottom_buffer = owner->scroll_bottom_buffer;
            procs[i]->scroll_top_count = owner->scroll_top_count;
            procs[i]->scroll_bottom_count = owner->scroll_bottom_count;
            procs[i]->text_screen_shared = true;
        }
    }

    /* Delegate to batch_ex with separate_windows=true but skip its init_text_screen
       since we already set up all screens above. We call it directly — it checks
       the separate_windows flag and the screens are already allocated. */
    return process_run_batch_ex(procs, count, true);
}

/**
 * process_run_batch - Executes one or more processes concurrently using cooperative multitasking
 */
int process_run_batch_ex(process_t **procs, int count, bool separate_windows) {
    if (!procs || count <= 0) {
        return -1;
    }

    /* If separate_windows is requested, initialize virtual text screens for all processes.
       Skip processes with text_screen_shared=true — their buffers are already set up
       by process_run_batch_windows(). */
    if (separate_windows) {
        for (int i = 0; i < count; i++) {
            if (procs[i] && !procs[i]->text_screen_shared) {
                process_init_text_screen(procs[i]);
            }
        }
    }

    process_t *old_process = current_process;
    process_t **old_batch_procs = batch_procs;
    int old_batch_proc_count = batch_proc_count;
    int old_batch_current_idx = batch_current_idx;
    bool old_batch_active = batch_active;
    bool old_batch_separate_windows = batch_separate_windows;
    process_t *old_batch_foreground_proc = batch_foreground_proc;
    uint32_t old_scheduler_esp = scheduler_esp;
    bool old_in_process_context = in_process_context;

    batch_procs = procs;
    batch_proc_count = count;
    batch_current_idx = 0;
    batch_active = true;
    batch_separate_windows = separate_windows;
    batch_foreground_proc = NULL;
    batch_displayed_fg = NULL;
    process_sync_shared_context();
    bool had_graphics = false;
    for (int i = 0; i < count; i++) {
        if (procs[i] && procs[i]->wants_graphics) {
            had_graphics = true;
            break;
        }
    }
    if (vga_is_graphics_mode() || wm_session_active()) {
        had_graphics = true;
    }

    /* Capture pristine text screen and cursor before any batch process alters them */
    uint16_t *batch_saved_screen = (uint16_t *)kmalloc(80 * 25 * sizeof(uint16_t));
    uint16_t batch_saved_cursor = vga_get_cursor_position();
    bool batch_saved_cursor_visible = vga_is_cursor_visible();
    if (batch_saved_screen) {
        if (vga_is_graphics_mode()) {
            for (int i = 0; i < 80 * 25; i++) batch_saved_screen[i] = 0x0720;
        } else {
            memcpy(batch_saved_screen, (const void *)0xB8000, 80 * 25 * sizeof(uint16_t));
            for (int i = 0; i < 80 * 25; i++) {
                uint16_t ent = batch_saved_screen[i];
                uint8_t attr = (uint8_t)(ent >> 8);
                if (ent == 0xFFFF || attr == 0xFF || attr == 0x20) {
                    batch_saved_screen[i] = 0x0720;
                }
            }
        }
    }

    terminal_lock_input();
    system_set_state(SYSTEM_STATE_PROCESS_RUNNING);
    keyboard_set_app_input_mode(true);

    batch_displayed_fg = NULL;
    process_t *fg_init = process_get_foreground();
    if (fg_init != NULL) {
        for (int i = 0; i < count; i++) {
            if (procs[i] == fg_init) {
                batch_current_idx = i;
                break;
            }
        }
        process_set_foreground(fg_init);
    }

    process_t *last_fg = NULL;
    system_clear_interrupt();

    while (1) {
        if (vga_is_graphics_mode() || wm_session_active()) {
            had_graphics = true;
        }

        process_t *fg = process_get_foreground();
        if (fg && fg->wants_graphics) {
            had_graphics = true;
        }

        if (system_is_interrupted()) {
            system_clear_interrupt();
            if (separate_windows && fg) {
                if (fg->is_running) {
                    fg->is_running = 0;
                    fg->exit_code = 130;
                }
                async_stop_tasks_by_owner(fg);
                fg->completed_and_acknowledged = true;
                if (!fg->text_screen_shared) {
                    if (fg->text_vram_backup) {
                        kfree(fg->text_vram_backup);
                    }
                    if (fg->scroll_top_buffer) {
                        kfree(fg->scroll_top_buffer);
                    }
                    if (fg->scroll_bottom_buffer) {
                        kfree(fg->scroll_bottom_buffer);
                    }
                    if (fg->gfx_vram_backup) {
                        kfree(fg->gfx_vram_backup);
                    }
                    if (fg->gfx_palette_backup) {
                        kfree(fg->gfx_palette_backup);
                    }
                }
                fg->text_vram_backup = NULL;
                fg->scroll_top_buffer = NULL;
                fg->scroll_bottom_buffer = NULL;
                fg->scroll_top_count = 0;
                fg->scroll_bottom_count = 0;
                fg->gfx_vram_backup = NULL;
                fg->gfx_palette_backup = NULL;
                process_t *next_fg = NULL;
                for (int i = 0; i < count; i++) {
                    if (procs[i] && procs[i] != fg && process_is_active_or_has_windows(procs[i])) {
                        next_fg = procs[i];
                        break;
                    }
                }
                if (next_fg) {
                    serial_printf("[proc] fg %s closed by Ctrl+C, auto-switching to %s\n", fg->name, next_fg->name);
                    process_set_foreground(next_fg);
                    continue;
                } else {
                    break;
                }
            } else {
                break;
            }
        }

        if (fg != last_fg) {
            serial_printf("[proc] batch fg changed: %s -> %s (wants_gfx=%d)\n",
                          last_fg ? last_fg->name : "none",
                          fg ? fg->name : "none",
                          fg ? (int)fg->wants_graphics : -1);
            if (fg && fg != batch_displayed_fg) {
                process_set_foreground(fg);
            }

            if (fg == NULL) {
                keyboard_set_app_input_mode(false);
                if (wm_session_active()) {
                    vga_set_mode_13h_hardware();
                    vga_gfx_init_default_palette();
                    mouse_set_bounds_from_display();
                    wm_invalidate_all();
                } else if (vga_is_graphics_mode()) {
                    vga_set_mode_text_hardware();
                    mouse_set_bounds_from_display();
                }
            } else {
                fg->waiting_for_input = false;
                keyboard_set_app_input_mode(true);
                if (wm_session_active()) {
                    if (!process_get_separate_windows() || (fg && (fg->wants_graphics || fg->is_wm_app))) {
                        if (fg->wants_graphics || fg->is_wm_app) {
                            vga_set_mode_13h_hardware();
                            vga_gfx_init_default_palette();
                            mouse_set_bounds_from_display();
                        }
                        wm_focus_window_for_pid(fg->pid);
                        wm_invalidate_all();
                        wm_compositor_tick();
                    }
                }
                for (int i = 0; i < count; i++) {
                    if (procs[i] == fg) {
                        batch_current_idx = i;
                        break;
                    }
                }
            }
            last_fg = fg;
        }

        /* Check if any process is still active (running, open WM windows, or unacknowledged separate workspace) */
        int active_count = 0;
        for (int i = 0; i < count; i++) {
            if (procs[i] && process_is_active_or_has_windows(procs[i])) {
                active_count++;
            }
        }

        /* If all processes have finished and all separate workspaces closed:
           the entire batch is finished -> return to console! */
        if (active_count == 0) {
            if (wm_session_active()) {
                wm_session_stop();
            }
            break;
        }

        if (separate_windows && fg && !fg->is_running) {
            /* Check if any other process in the batch is still running */
            process_t *running_proc = NULL;
            for (int i = 0; i < count; i++) {
                if (procs[i] && procs[i] != fg && procs[i]->is_running) {
                    running_proc = procs[i];
                    break;
                }
            }

            if (running_proc) {
                /* Immediately acknowledge and auto-switch to the living process */
                fg->completed_and_acknowledged = true;
                async_stop_tasks_by_owner(fg);
                if (separate_windows && !fg->text_screen_shared) {
                    if (fg->text_vram_backup) { kfree(fg->text_vram_backup); fg->text_vram_backup = NULL; }
                    if (fg->scroll_top_buffer) { kfree(fg->scroll_top_buffer); fg->scroll_top_buffer = NULL; }
                    if (fg->scroll_bottom_buffer) { kfree(fg->scroll_bottom_buffer); fg->scroll_bottom_buffer = NULL; }
                    if (fg->gfx_vram_backup) { kfree(fg->gfx_vram_backup); fg->gfx_vram_backup = NULL; }
                    if (fg->gfx_palette_backup) { kfree(fg->gfx_palette_backup); fg->gfx_palette_backup = NULL; }
                }
                fg->scroll_top_count = 0;
                fg->scroll_bottom_count = 0;
                serial_printf("[proc] fg %s exited, auto-switching to running %s\n", fg->name, running_proc->name);
                process_set_foreground(running_proc);
                continue;
            } else if (!separate_windows || fg->wants_graphics || fg->is_wm_app || fg->completed_and_acknowledged || fg->exit_code == 139) {
                /* A graphics app, WM app, crashed app, single-workspace CLI app, or acknowledged app closes immediately */
                fg->completed_and_acknowledged = true;
                async_stop_tasks_by_owner(fg);
                if (separate_windows && !fg->text_screen_shared) {
                    if (fg->text_vram_backup) { kfree(fg->text_vram_backup); fg->text_vram_backup = NULL; }
                    if (fg->scroll_top_buffer) { kfree(fg->scroll_top_buffer); fg->scroll_top_buffer = NULL; }
                    if (fg->scroll_bottom_buffer) { kfree(fg->scroll_bottom_buffer); fg->scroll_bottom_buffer = NULL; }
                    if (fg->gfx_vram_backup) { kfree(fg->gfx_vram_backup); fg->gfx_vram_backup = NULL; }
                    if (fg->gfx_palette_backup) { kfree(fg->gfx_palette_backup); fg->gfx_palette_backup = NULL; }
                }
                fg->scroll_top_count = 0;
                fg->scroll_bottom_count = 0;
                serial_printf("[proc] fg %s exited (exit_code=%d), batch finished\n", fg->name, fg->exit_code);
                break;
            } else {
                /* CLI program in separate workspace with no other running processes: allow reading text or scrolling before Ctrl+C */
                keyboard_set_app_input_mode(true);
                if (!fg->completed_and_acknowledged) {
                    if (system_is_interrupted()) {
                        system_clear_interrupt();
                        fg->completed_and_acknowledged = true;
                    } else {
                        uint8_t sc = keyboard_get_scancode();
                        if (process_get_foreground() != fg) {
                            continue;
                        }
                        /* Accept Ctrl+C or Escape to close finished workspace */
                        if (sc == 0x01 || ((sc == 0x2E || sc == 0xAE) && keyboard_is_ctrl_pressed())) {
                            fg->completed_and_acknowledged = true;
                        } else if (sc == 0x49) { // Page Up
                            if (terminal_get_top_buffer_count() > 0) {
                                terminal_scroll_up();
                            }
                        } else if (sc == 0x51) { // Page Down
                            if (terminal_get_bottom_buffer_count() > 0) {
                                terminal_scroll_down();
                            }
                        } else if (sc == 0x48) { // Up arrow
                            if (terminal_get_top_buffer_count() > 0) {
                                terminal_scroll_up();
                            }
                        } else if (sc == 0x50) { // Down arrow
                            if (terminal_get_bottom_buffer_count() > 0) {
                                terminal_scroll_down();
                            }
                        }
                    }
                }
                if (fg->completed_and_acknowledged) {
                    async_stop_tasks_by_owner(fg);
                    if (separate_windows && !fg->text_screen_shared) {
                        if (fg->text_vram_backup) {
                            kfree(fg->text_vram_backup);
                        }
                        if (fg->scroll_top_buffer) {
                            kfree(fg->scroll_top_buffer);
                        }
                        if (fg->scroll_bottom_buffer) {
                            kfree(fg->scroll_bottom_buffer);
                        }
                        if (fg->gfx_vram_backup) {
                            kfree(fg->gfx_vram_backup);
                        }
                        if (fg->gfx_palette_backup) {
                            kfree(fg->gfx_palette_backup);
                        }
                    }
                    fg->text_vram_backup = NULL;
                    fg->scroll_top_buffer = NULL;
                    fg->scroll_bottom_buffer = NULL;
                    fg->scroll_top_count = 0;
                    fg->scroll_bottom_count = 0;
                    fg->gfx_vram_backup = NULL;
                    fg->gfx_palette_backup = NULL;
                    process_t *next_fg = NULL;
                    for (int i = 0; i < count; i++) {
                        if (procs[i] && process_is_active_or_has_windows(procs[i])) {
                            next_fg = procs[i];
                            break;
                        }
                    }
                    if (next_fg) {
                        serial_printf("[proc] fg %s closed, auto-switching to %s\n", fg->name, next_fg->name);
                        process_set_foreground(next_fg);
                        continue;
                    } else {
                        break;
                    }
                }
            }
        }

        /* Clean up any terminated and acknowledged process windows */
        if (separate_windows) {
            for (int i = 0; i < count; i++) {
                if (procs[i] && !process_is_active_or_has_windows(procs[i])) {
                    if (!procs[i]->text_screen_shared) {
                        if (procs[i]->text_vram_backup) {
                            kfree(procs[i]->text_vram_backup);
                        }
                        if (procs[i]->scroll_top_buffer) {
                            kfree(procs[i]->scroll_top_buffer);
                        }
                        if (procs[i]->scroll_bottom_buffer) {
                            kfree(procs[i]->scroll_bottom_buffer);
                        }
                        if (procs[i]->gfx_vram_backup) {
                            kfree(procs[i]->gfx_vram_backup);
                        }
                        if (procs[i]->gfx_palette_backup) {
                            kfree(procs[i]->gfx_palette_backup);
                        }
                    }
                    procs[i]->text_vram_backup = NULL;
                    procs[i]->scroll_top_buffer = NULL;
                    procs[i]->scroll_bottom_buffer = NULL;
                    procs[i]->scroll_top_count = 0;
                    procs[i]->scroll_bottom_count = 0;
                    procs[i]->gfx_vram_backup = NULL;
                    procs[i]->gfx_palette_backup = NULL;
                }
            }
        }

        // Drive async tasks and WM compositor if active, in graphics mode, and not in raw fullscreen VGA graphics
        async_scheduler_tick();
        bool should_tick_wm = false;
        if (wm_session_active() && vga_is_graphics_mode()) {
            if (process_get_separate_windows()) {
                should_tick_wm = (fg == NULL || fg->is_wm_app);
            } else {
                should_tick_wm = (!fg || fg->is_wm_app || !fg->wants_graphics);
            }
        }
        if (should_tick_wm) {
            wm_compositor_tick();
        }

        // Find next runnable process
        int found = -1;
        for (int step = 0; step < count; step++) {
            int idx = (batch_current_idx + step) % count;
            process_t *p = procs[idx];
            if (p && p->is_running) {
                if (p->waiting_for_input && p != fg && !wm_session_active() && fg && fg->is_running) {
                    continue;
                }
                found = idx;
                break;
            }
        }

        if (found == -1) {
            if (fg && fg->is_running) {
                for (int i = 0; i < count; i++) {
                    if (procs[i] == fg) {
                        found = i;
                        break;
                    }
                }
                io_wait();
            }
            if (found == -1) {
                for (int i = 0; i < count; i++) {
                    if (procs[i] && procs[i]->is_running) {
                        found = i;
                        break;
                    }
                }
            }
            if (found == -1) {
                if (fg) {
                    current_process = fg;
                    process_sync_shared_context();
                }
                io_wait();
                continue;
            }
        }

        batch_current_idx = (found + 1) % count;
        current_process = procs[found];
        process_sync_shared_context();

        // Map process binary to 0x800000
        process_map_app(current_process);

        in_process_context = true;
        // Switch to process!
        process_switch_context(&scheduler_esp, current_process->stack_ptr);
        in_process_context = false;
        // Process yields back here
    }

    if (old_process) {
        process_map_app(old_process);
    } else {
        process_map_app(NULL);
    }
    batch_active = old_batch_active;
    batch_separate_windows = old_batch_separate_windows;
    batch_foreground_proc = old_batch_foreground_proc;
    batch_procs = old_batch_procs;
    batch_proc_count = old_batch_proc_count;
    batch_current_idx = old_batch_current_idx;
    scheduler_esp = old_scheduler_esp;
    in_process_context = old_in_process_context;
    current_process = old_process;
    batch_displayed_fg = NULL;
    process_sync_shared_context();

    if (!old_batch_active) {
        keyboard_set_app_input_mode(false);
        keyboard_clear_key_state();
        terminal_unlock_input();
        system_set_state(SYSTEM_STATE_TERMINAL_IDLE);
    }

    if (system_is_interrupted()) {
        if (wm_session_active()) {
            wm_session_stop();
        }
        if (vga_is_graphics_mode()) {
            vga_set_mode_text_hardware();
        }
        if ((had_graphics || separate_windows) && batch_saved_screen && !wm_session_active()) {
            memcpy((void *)0xB8000, batch_saved_screen, 80 * 25 * sizeof(uint16_t));
            vga_set_cursor(batch_saved_cursor);
            if (batch_saved_cursor_visible) {
                vga_show_cursor();
            } else {
                vga_hide_cursor();
            }
        }
        vga_font_set_app_mode(false);
        dynamic_keymap_reapply_fonts();
        vga_cursor_reset();
        vga_sanitize_text_vram();
        vga_show_cursor();
        printf("Application stopped.\n");
        system_clear_interrupt();
    } else if (!wm_session_active()) {
        if (vga_is_graphics_mode()) {
            vga_set_mode_text_hardware();
        }
        if ((had_graphics || separate_windows) && batch_saved_screen) {
            memcpy((void *)0xB8000, batch_saved_screen, 80 * 25 * sizeof(uint16_t));
            vga_set_cursor(batch_saved_cursor);
            if (batch_saved_cursor_visible) {
                vga_show_cursor();
            } else {
                vga_hide_cursor();
            }

            /* Replay stdout produced by text processes only for single-workspace batches */
            if (!separate_windows) {
                for (int i = 0; i < count; i++) {
                    if (procs[i] && procs[i]->text_output_buf && procs[i]->text_output_len > 0) {
                        printf("%s", procs[i]->text_output_buf);
                    }
                }
            }
        }
        vga_font_set_app_mode(false);
        dynamic_keymap_reapply_fonts();
        vga_cursor_reset();
        vga_sanitize_text_vram();
        vga_show_cursor();
    }

    if (batch_saved_screen) {
        kfree(batch_saved_screen);
        batch_saved_screen = NULL;
    }

    if (count > last_batch_exit_codes_cap) {
        if (last_batch_exit_codes) kfree(last_batch_exit_codes);
        last_batch_exit_codes = kmalloc((size_t)count * sizeof(int));
        last_batch_exit_codes_cap = last_batch_exit_codes ? count : 0;
    }
    last_batch_count = (count <= last_batch_exit_codes_cap) ? count : last_batch_exit_codes_cap;
    for (int i = 0; i < last_batch_count; i++) {
        if (procs[i] && process_is_valid(procs[i])) {
            last_batch_exit_codes[i] = procs[i]->exit_code;
        } else {
            last_batch_exit_codes[i] = 0;
        }
    }

    if (count > last_batch_return_vals_cap) {
        if (last_batch_return_vals) {
            for (int i = 0; i < last_batch_return_vals_cap; i++) {
                process_return_val_free_fields(&last_batch_return_vals[i]);
            }
            kfree(last_batch_return_vals);
        }
        last_batch_return_vals = kmalloc((size_t)count * sizeof(process_return_value_t));
        last_batch_return_vals_cap = last_batch_return_vals ? count : 0;
        if (last_batch_return_vals) {
            for (int i = 0; i < last_batch_return_vals_cap; i++) {
                process_return_val_init(&last_batch_return_vals[i]);
            }
        }
    }
    for (int i = 0; i < count; i++) {
        if (procs[i] && process_is_valid(procs[i])) {
            if (last_batch_return_vals && i < last_batch_return_vals_cap) {
                process_return_val_copy(&last_batch_return_vals[i], &procs[i]->return_val);
            }
            process_return_val_copy(&last_return_val, &procs[i]->return_val);
        }
    }

    for (int i = 0; i < count; i++) {
        process_t *proc = procs[i];
        if (!proc) continue;
        if (!process_is_valid(proc)) continue;
        if (proc->async_task_count > 0) {
            proc->is_running = 0;
            serial_printf("[process] keeping process %u alive in background (owns %u async task(s))\n",
                   proc->pid, proc->async_task_count);
        } else {
            process_cleanup(proc);
        }
    }
    log_process_heap_state("after cleanup");

    return 0;
}

int process_run_batch(process_t **procs, int count) {
    return process_run_batch_ex(procs, count, false);
}

/**
 * universal_process_execute - The single universal function for launching ANY program
 * regardless of file format, headers, or lack of headers.
 * If the file is not an executable file (plain text, empty, directory, invalid),
 * exits cleanly at the launch moment without hanging or crashing.
 */
int universal_process_execute(const char *path, int argc, char **argv) {
    process_t *proc = process_spawn(path, argc, argv);
    if (!proc) {
        return -1;
    }
    uint32_t pid = proc->pid;
    process_t *batch[1] = { proc };
    process_run_batch(batch, 1);
    return (int)pid;
}

int process_exec(const char *path, int argc, char **argv) {
    return universal_process_execute(path, argc, argv);
}

/**
 * process_get_exit_code - Returns the exit code of the last process.
 */
int process_get_exit_code(void) {
    return last_exit_code;
}

void process_set_last_exit_code(int code) {
    last_exit_code = code;
}

void process_print_last_return_value(void) {
    if (last_return_val.str_val && last_return_val.str_val[0] != '\0') {
        printf("Return value: %s\n", last_return_val.str_val);
        serial_printf("Return value: %s\n", last_return_val.str_val);
    } else {
        printf("Return value: %d\n", last_return_val.int_val);
        serial_printf("Return value: %d\n", last_return_val.int_val);
    }
}

void process_print_batch_return_value(int idx) {
    if (idx >= 0 && idx < last_batch_count && last_batch_return_vals) {
        const process_return_value_t *val = &last_batch_return_vals[idx];
        if (val->str_val && val->str_val[0] != '\0') {
            printf("Return value: %s\n", val->str_val);
            return;
        }
        printf("Return value: %d\n", val->int_val);
    } else {
        printf("Return value: 0\n");
    }
}

const char* process_get_last_return_string(void) {
    return last_return_val.str_val;
}

const process_return_value_t* process_get_last_return_value(void) {
    return &last_return_val;
}

void process_set_current_return_value(process_return_type_t type, const char *type_name, const char *str_val, int32_t int_val, const void *data, uint32_t size) {
    process_t *proc = current_process;
    if (!proc) return;

    process_return_val_reset(&proc->return_val);
    proc->return_val.type = type;
    if (type_name) {
        process_return_val_set_type_name(&proc->return_val, type_name);
    }

    if (type == PROC_RET_INT) {
        proc->return_val.int_val = int_val;
        proc->return_val.raw_eax = (uint32_t)int_val;
        char *num_str = format_int_dynamic(int_val);
        if (num_str) {
            process_return_val_set_str(&proc->return_val, num_str);
            process_return_val_add_arg(&proc->return_val, num_str);
            kfree(num_str);
        }
        proc->exit_code = int_val;
    } else if (type == PROC_RET_CHAR) {
        proc->return_val.int_val = int_val;
        proc->return_val.raw_eax = (uint32_t)int_val;
        char ch_str[4] = { '\'', (char)int_val, '\'', '\0' };
        char ch_arg[2] = { (char)int_val, '\0' };
        process_return_val_set_str(&proc->return_val, ch_str);
        process_return_val_add_arg(&proc->return_val, ch_arg);
        proc->exit_code = int_val;
    } else if (type == PROC_RET_STRING) {
        proc->return_val.int_val = 0;
        proc->return_val.raw_eax = (uint32_t)(uintptr_t)str_val;
        if (str_val) {
            if (type_name && (strcmp(type_name, "char*") == 0 || strcmp(type_name, "string") == 0 || strcmp(type_name, "s") == 0)) {
                process_return_val_set_str(&proc->return_val, "\"");
                process_return_val_append_str(&proc->return_val, str_val);
                process_return_val_append_str(&proc->return_val, "\"");
            } else {
                process_return_val_set_str(&proc->return_val, str_val);
            }
            process_return_val_add_arg(&proc->return_val, str_val);
        } else {
            process_return_val_set_str(&proc->return_val, "\"\"");
            process_return_val_add_arg(&proc->return_val, "");
        }
        proc->exit_code = 0;
    } else if (type == PROC_RET_STRUCT) {
        process_return_val_set_struct_data(&proc->return_val, (const uint8_t *)data, size);
        if (data && size >= 4) {
            proc->return_val.int_val = struct_read_i32((const uint8_t *)data);
        } else if (data && size > 0) {
            proc->return_val.int_val = (int32_t)((const uint8_t *)data)[0];
        }
        proc->return_val.raw_eax = (uint32_t)(uintptr_t)data;
        if (str_val && str_val[0] != '\0') {
            process_return_val_set_str(&proc->return_val, str_val);
        } else if (data && size > 0) {
            if (type_name && type_name[0] != '\0' && strcmp(type_name, "struct") != 0) {
                format_struct_fields(&proc->return_val, (const uint8_t *)data, size, type_name);
            } else {
                format_struct_fields_universal(&proc->return_val, (const uint8_t *)data, size);
            }
        } else {
            process_return_val_set_str(&proc->return_val, "{ }");
        }
        proc->exit_code = proc->return_val.int_val;
    } else if (type == PROC_RET_POINTER) {
        proc->return_val.raw_eax = (uint32_t)(uintptr_t)data;
        char *ptr_str = format_hex_dynamic((uint32_t)(uintptr_t)data);
        if (ptr_str) {
            process_return_val_set_str(&proc->return_val, ptr_str);
            process_return_val_add_arg(&proc->return_val, ptr_str);
            kfree(ptr_str);
        }
        proc->exit_code = 0;
    }

    proc->return_val_explicit = true;
    last_exit_code = proc->exit_code;
    process_return_val_copy(&last_return_val, &proc->return_val);
}

/**
 * process_get_current - Returns the current process
 */
process_t *process_get_current(void) {
    process_shared_ctx_t *ctx = process_get_shared_context();
    if (ctx && ctx->magic == PROCESS_SHARED_MAGIC && ctx->current_process != NULL) {
        return ctx->current_process;
    }
    return current_process;
}

void process_set_current(process_t *proc) {
    current_process = proc;
    process_sync_shared_context();
}

bool process_is_valid(process_t *proc) {
    if (!proc) return false;
    process_t *curr = process_list;
    while (curr != NULL) {
        if (curr == proc) return true;
        curr = curr->next;
    }
    return false;
}

/**
 * process_list_print - Lists all running and background processes
 */
void process_list_print(void) {
    if (process_list == NULL) {
        printf("No active or background processes running.\n");
        return;
    }

    printf("  PID  STATE         ASYNC    MEMORY      NAME\n");
    printf("  ------------------------------------------------------------\n");

    process_t *curr = process_list;
    uint32_t count = 0;
    while (curr != NULL) {
        const char *state = curr->is_running ? "RUNNING" : "BACKGROUND";
        uint32_t mem_kb = (curr->binary_size + curr->stack_size + 1023) / 1024;

        printf("  %u", curr->pid);
        if (curr->pid < 10) printf("   ");
        else if (curr->pid < 100) printf("  ");
        else printf(" ");

        printf("%s", state);
        size_t slen = strlen(state);
        for (size_t s = slen; s < 14; s++) putchar(' ');

        printf("%u", curr->async_task_count);
        if (curr->async_task_count < 10) printf("        ");
        else if (curr->async_task_count < 100) printf("       ");
        else printf("      ");

        printf("%u KB", mem_kb);
        if (mem_kb < 10) printf("       ");
        else if (mem_kb < 100) printf("      ");
        else if (mem_kb < 1000) printf("     ");
        else if (mem_kb < 10000) printf("    ");
        else printf("   ");

        printf("%s\n", (curr->name && curr->name[0]) ? curr->name : "unnamed");

        count++;
        curr = curr->next;
    }
    printf("  ------------------------------------------------------------\n");
    printf("  Total processes: %u\n", count);
}

/**
 * process_kill_by_pid - Terminates a process and its async tasks by PID
 */
int process_kill_by_pid(uint32_t pid) {
    process_t *curr = process_list;
    while (curr != NULL) {
        if (curr->pid == pid) {
            char *name_copy = curr->name ? kstrdup(curr->name) : NULL;

            /* Stop all async tasks owned by this process */
            async_stop_tasks_by_owner(curr);
            curr->async_task_count = 0;

            /* Cleanup memory and unlink from list */
            process_cleanup(curr);
            printf("[process] pid=%u (%s) killed and memory freed.\n",
                   pid, name_copy ? name_copy : "unnamed");
            if (name_copy) kfree(name_copy);
            return 0;
        }
        curr = curr->next;
    }

    printf("kill: no process found with PID %u\n", pid);
    return -1;
}

/**
 * process_find_by_pid - Find process struct by PID
 */
process_t *process_find_by_pid(uint32_t pid) {
    process_t *curr = process_list;
    while (curr != NULL) {
        if (curr->pid == pid) return curr;
        curr = curr->next;
    }
    return NULL;
}

/**
 * process_is_alive - Check if a process with @pid is still in the process list.
 * Returns true if the process exists (even in background/keep-alive state).
 */
bool process_is_alive(uint32_t pid) {
    return process_find_by_pid(pid) != NULL;
}

/**
 * process_kill_all - Terminates all active and background processes
 */
int process_kill_all(void) {
    if (process_list == NULL) {
        printf("No active processes to kill.\n");
        return 0;
    }

    async_stop_all_tasks();

    uint32_t killed = 0;
    while (process_list != NULL) {
        process_t *proc = process_list;
        proc->async_task_count = 0;
        char *name_copy = proc->name ? kstrdup(proc->name) : NULL;
        uint32_t pid = proc->pid;

        process_cleanup(proc);
        printf("[process] killed pid=%u (%s)\n", pid, name_copy ? name_copy : "unnamed");
        if (name_copy) kfree(name_copy);
        killed++;
    }

    printf("[process] Terminated %u process(es).\n", killed);
    return (int)killed;
}
