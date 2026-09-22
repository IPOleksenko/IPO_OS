#include <kernel/process.h>
#include <kernel/async.h>
#include <kernel/terminal.h>
#include <system/state.h>
#include <file_system/ipo_fs.h>
#include <memory/kmalloc.h>
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
static process_t *current_process = NULL;
static process_t *process_list = NULL;

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
            vga_gfx_init_default_palette();
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
    }
    if (!proc->scroll_bottom_buffer) {
        proc->scroll_bottom_buffer = (uint16_t *)kmalloc(TERMINAL_SCROLL_HISTORY_SIZE * 80 * sizeof(uint16_t));
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
            for (int i = 0; i < keep; i++) {
                memcpy(&proc->scroll_top_buffer[i * 80],
                       &proc->scroll_top_buffer[(i + remove) * 80],
                       80 * sizeof(uint16_t));
            }
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
    int unique_wins[16];
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
            if (!exists && unique_count < 16) {
                unique_wins[unique_count++] = wid;
            }
        }
    }

    if (unique_count <= 1) return;

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
    int unique_wins[16];
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
            if (!exists && unique_count < 16) {
                unique_wins[unique_count++] = wid;
            }
        }
    }

    if (unique_count <= 1) return;

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

static void process_trampoline(void) {
    process_t *proc = current_process;
    typedef int (*entry_func_t)(int, char**, char**);
    entry_func_t entry = (entry_func_t)proc->entry_point;
    char **argv = (char**)proc->argv_kernel;

    serial_printf("[process] pid=%u start trampoline\n", proc->pid);

    int exit_code = 0;
    if (entry != NULL) {
        uint32_t cur_esp;
        __asm__ volatile("movl %%esp, %0" : "=r"(cur_esp));
        serial_printf("[process] pid=%u calling entry=0x%x esp=0x%x first4=0x%x\n",
                      proc->pid, (uint32_t)entry, cur_esp, *(uint32_t*)entry);
        exit_code = entry(proc->argc, argv, default_env);
    }

    proc->is_running = 0;
    proc->exit_code = exit_code;
    last_exit_code = exit_code;

    if (!vga_is_graphics_mode()) {
        vga_sanitize_text_vram();
    }

    serial_printf("[process] pid=%u trampoline exit_code=%d\n", proc->pid, exit_code);

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
    
    printf("Process manager initialized\n");
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
        return -1;  // File not found
    }
    
    if ((stat.mode & IPO_INODE_TYPE_DIR) != 0) {
        printf("Path is a directory: %s\n", path);
        return -1;  // Path is a directory
    }
    
    if (stat.size == 0) {
        printf("File is empty: %s\n", path);
        return -1;
    }
    
    serial_printf("Loading binary file: %s, size: %d bytes\n", path, stat.size);
    
    // For large files, we use step-by-step loading.
    void *binary_image = kmalloc(stat.size);
    if (binary_image == NULL) {
        printf("Memory allocation failed for size: %d\n", stat.size);
        return -3;  // Memory allocation failed
    }
    
    // Open the file
    int fd = ipo_fs_open(path);
    if (fd < 0) {
        kfree(binary_image);
        printf("Failed to open file: %s\n", path);
        return -4;  // File open failed
    }
    
    // Read the file in parts if it is large
    uint32_t total_read = 0;
    uint32_t chunk_size = 64 * 1024;  // 64KB
    
    while (total_read < stat.size) {
        uint32_t to_read = stat.size - total_read;
        if (to_read > chunk_size) {
            to_read = chunk_size;
        }
        
        int bytes_read = ipo_fs_read(fd, (uint8_t*)binary_image + total_read, to_read, total_read);
        
        if (bytes_read <= 0) {
            ipo_fs_close(fd);
            kfree(binary_image);
            printf("Read failed at offset %d, read %d bytes\n", total_read, bytes_read);
            return -4;  // Read failed
        }
        
        total_read += bytes_read;
        serial_printf("Read chunk: %d bytes, total: %d/%d\n", bytes_read, total_read, stat.size);
    }

    ipo_fs_close(fd);
    *data_out = binary_image;
    serial_printf("File loaded successfully (%d bytes)\n", stat.size);
    return (int)stat.size;
}

/**
 * relocate_binary - Relocates the binary if necessary
 */
static int relocate_binary(void *binary, uint32_t load_address, uint32_t size) {
    return 0;
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

static char *kstrdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = (char *)kmalloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, s, len + 1);
    return copy;
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
static int universal_exec_load(process_t *proc, const uint8_t *file_data, uint32_t file_size) {
    if (!proc || !file_data || file_size == 0) {
        return -1;
    }

    uint32_t total_memsz = file_size;
    uint32_t entry_point = (uint32_t)PROCESS_HEAP_START;
    bool is_elf = (file_size >= sizeof(elf32_header_t) &&
                   file_data[0] == 0x7F && file_data[1] == 'E' &&
                   file_data[2] == 'L'  && file_data[3] == 'F');

    if (is_elf) {
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
    proc->user_heap_break = 0x08000000u + (proc->pid > 0 ? (proc->pid - 1) : 0) * 0x04000000u; /* Isolated 64MB slice per PID */

    proc->next = process_list;
    process_list = proc;

    // Load executable via the Universal Executable Loader
    int load_res = universal_exec_load(proc, (const uint8_t *)binary_image, (uint32_t)size);
    kfree(binary_image);
    if (resolved != NULL) kfree(resolved);

    if (load_res < 0) {
        printf("Cannot execute '%s': failed to load image\n", actual_path);
        process_cleanup(proc);
        return NULL;
    }

    // Setting up arguments - argv is allocated in kernel memory
    uint32_t argv_addr = 0;
    if (setup_arguments(proc, argc, argv, &argv_addr) < 0) {
        printf("Failed to setup arguments\n");
        process_cleanup(proc);
        return NULL;
    }

    // Setting up the stack for calling main()
    if (setup_stack(proc) == 0) {
        printf("Failed to allocate process stack\n");
        process_cleanup(proc);
        return NULL;
    }

    // Defensive validation: a broken application must never crash the kernel.
    if (proc->binary_base == NULL || proc->binary_size == 0 ||
        proc->entry_point < PROCESS_HEAP_START) {
        printf("[process] pid=%u: invalid app image, closing process safely\n", proc->pid);
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
                }
                fg->text_vram_backup = NULL;
                fg->scroll_top_buffer = NULL;
                fg->scroll_bottom_buffer = NULL;
                fg->scroll_top_count = 0;
                fg->scroll_bottom_count = 0;
                fg->gfx_vram_backup = NULL;
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

        if (separate_windows && fg && !fg->is_running && !fg->is_wm_app && !fg->wants_graphics) {
            if (!fg->completed_and_acknowledged) {
                uint8_t sc = keyboard_get_scancode();
                if (process_get_foreground() != fg) {
                    continue;
                }
                if ((sc == 0x2E || sc == 0xAE) && keyboard_is_ctrl_pressed()) {
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
                }
                if (separate_windows) {
                    fg->text_vram_backup = NULL;
                    fg->scroll_top_buffer = NULL;
                    fg->scroll_bottom_buffer = NULL;
                    fg->scroll_top_count = 0;
                    fg->scroll_bottom_count = 0;
                    fg->gfx_vram_backup = NULL;
                }
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
                    }
                    procs[i]->text_vram_backup = NULL;
                    procs[i]->scroll_top_buffer = NULL;
                    procs[i]->scroll_bottom_buffer = NULL;
                    procs[i]->scroll_top_count = 0;
                    procs[i]->scroll_bottom_count = 0;
                    procs[i]->gfx_vram_backup = NULL;
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
                if (p->waiting_for_input && p != fg && !wm_session_active()) {
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
 * process_exec - The main function for executing a process with arguments
 */
int process_exec(const char *path, int argc, char **argv) {
    process_t *proc = process_spawn(path, argc, argv);
    if (!proc) {
        return -1;
    }
    uint32_t pid = proc->pid;
    process_t *batch[1] = { proc };
    process_run_batch(batch, 1);
    return (int)pid;
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
