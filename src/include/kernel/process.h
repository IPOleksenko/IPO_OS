#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include <stdint.h>
#include <stdbool.h>
#include <types.h>
#include <arch/context.h>
#include <file_system/ipo_fs.h>
#include <kernel/utf.h>

typedef struct address_space address_space_t;

#define PROCESS_HEAP_START       0x00800000u  /* 8 MiB */
#define USER_HEAP_START          0x10000000u  /* 256 MiB */
#define USER_HEAP_PROCESS_STRIDE 0x10000000u  /* 256 MiB per-process stride */

#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

typedef enum {
    PROC_RET_INT = 0,
    PROC_RET_CHAR,
    PROC_RET_STRING,
    PROC_RET_STRUCT,
    PROC_RET_POINTER
} process_return_type_t;

typedef struct {
    process_return_type_t type;
    int32_t int_val;
    ipo_reg_t raw_eax;
    ipo_reg_t raw_edx;

    char *type_name;
    uint32_t type_name_len;
    uint32_t type_name_cap;

    char *str_val;
    uint32_t str_val_len;
    uint32_t str_val_cap;

    uint8_t *struct_data;
    uint32_t struct_size;
    uint32_t struct_data_cap;

    /* Pipeline arguments passed via `run` */
    char **arg_vals;
    uint32_t arg_count;
    uint32_t arg_cap;
} process_return_value_t;

typedef struct process_heap_chunk {
    ipo_vaddr_t base;
    ipo_vaddr_t limit;
    ipo_vaddr_t brk;
} process_heap_chunk_t;

typedef struct process_thread process_thread_t;

typedef struct process {
    uint32_t pid;
    
    void *binary_base;
    ipo_size_t binary_size;
    ipo_vaddr_t entry_point;
    void *binary_storage;   /* backing buffer when swapped out */
    ipo_size_t data_offset;   /* writable data/bss offset */
    ipo_size_t data_size;     /* writable data/bss size */
    bool code_mapped;       /* true once mapped at 0x800000 */
    
    void *stack_base;
    ipo_vaddr_t stack_ptr;
    ipo_vaddr_t stack_start;
    ipo_size_t stack_size;
    arch_context_t arch_ctx;
    process_thread_t *threads;
    process_thread_t *current_thread;
    uint32_t next_thread_id;
    address_space_t *address_space;
    
    int argc;
    ipo_vaddr_t argv_addr;
    char **argv_kernel;     /* kernel copy for diagnostics */
    
    int exit_code;
    process_return_value_t return_val;
    bool return_val_explicit;
    uint8_t is_running;
    bool waiting_for_input;
    bool ipc_waiting;
    bool ipc_wait_cancelled;
    uint32_t ipc_wait_endpoint;
    uint8_t ipc_wait_kind;
    uint64_t signal_pending;
    uint64_t signal_mask;
    uintptr_t *signal_handlers;
    uint64_t *signal_action_masks;
    uint32_t signal_action_capacity;
    bool signal_stopped;
    bool signal_delivering;
    bool wants_graphics;    /* VGA Mode 13h requested */
    uint32_t gfx_overlay_flags; /* VGA_OVERLAY_* */
    bool is_wm_app;
    uint32_t async_task_count;
    ipo_vaddr_t user_heap_base;
    ipo_vaddr_t user_heap_break;
    ipo_vaddr_t user_heap_limit;
    int32_t  user_heap_slot;   /* -1 if unassigned */
    uint32_t heap_chunk_count;
    uint32_t heap_chunk_cap;
    process_heap_chunk_t *heap_chunks;

    struct ipo_inode exec_inode;
    bool     demand_loading;
    ipo_size_t demand_file_offset;
    ipo_size_t demand_mem_offset;
    ipo_size_t demand_loaded_bytes;
    ipo_size_t demand_total_bytes;

    uint16_t *text_vram_backup;
    uint8_t *gfx_vram_backup;
    int      gfx_vram_backup_w;
    int      gfx_vram_backup_h;
    uint8_t *gfx_palette_backup; /* 768 B 256-color DAC palette backup */
    uint16_t text_cursor_pos;
    bool text_cursor_visible;
    bool completed_and_acknowledged;

    uint16_t *scroll_top_buffer;
    int scroll_top_capacity;        /* in lines */
    uint16_t *scroll_bottom_buffer;
    int scroll_bottom_capacity;     /* in lines */
    int scroll_top_count;
    int scroll_bottom_count;
    int window_id;                  /* workspace ID in batch mode */
    bool text_screen_shared;        /* buffers shared within same window */

    char *text_output_buf;      /* stdout capture for replay */
    uint32_t text_output_len;
    uint32_t text_output_cap;
    uint32_t text_vga_written_len; /* bytes already written to live VGA text memory */

    utf_stream_t utf_stream;

    char *name;
    char *return_format;    /* "int", "string", "struct", etc. */
    
    struct process *next;
} process_t;

typedef enum {
    PROCESS_THREAD_RUNNABLE = 0,
    PROCESS_THREAD_BLOCKED,
    PROCESS_THREAD_EXITED
} process_thread_state_t;

struct process_thread {
    uint32_t id;
    ipo_vaddr_t context_sp;
    arch_context_t arch_ctx;
    void *stack_base;
    ipo_size_t stack_size;
    void *(*entry)(void *);
    void *argument;
    void *result;
    process_thread_state_t state;
    bool is_main;
    bool join_claimed;
    process_thread_t *joiner;
    process_thread_t *wait_next;
    bool ipc_waiting;
    bool ipc_wait_cancelled;
    uint32_t ipc_wait_endpoint;
    uint8_t ipc_wait_kind;
    uint64_t signal_mask;
    ipo_vaddr_t signal_altstack_base;
    ipo_size_t signal_altstack_size;
    uint32_t signal_altstack_flags;
    bool signal_altstack_active;
    ipo_vaddr_t stack_virtual_top;
    ipo_vaddr_t stack_virtual_bottom;
    ipo_vaddr_t guard_page_addr;
    ipo_size_t stack_allocated_pages;
    process_thread_t *next;
};

#define PROCESS_SHARED_ADDR  0x00001000u
#define PROCESS_SHARED_MAGIC 0x50524F43u

typedef struct {
    uint32_t magic;
    process_t *current_process;
    process_t *batch_foreground_proc;
    process_t *batch_displayed_fg;
    bool batch_active;
    bool batch_separate_windows;
    uint32_t active_count;
} process_shared_ctx_t;

static inline process_shared_ctx_t *process_get_shared_context(void) {
    return (process_shared_ctx_t *)PROCESS_SHARED_ADDR;
}

bool process_is_active_or_has_windows(process_t *p);
void process_workspace_auto_scroll(process_t *proc);
void process_sync_workspace_text_state(process_t *proc);
uintptr_t process_current_context_token(void);

typedef int (*process_entry_t)(int argc, char **argv);

void process_init(void);
int universal_process_execute(const char *path, int argc, char **argv);
int process_exec(const char *path, int argc, char **argv);
int process_get_exit_code(void);
void process_set_last_exit_code(int code);
void process_print_last_return_value(void);
void process_print_batch_return_value(int idx);
const char* process_get_last_return_string(void);
const process_return_value_t* process_get_last_return_value(void);
void process_set_current_return_value(process_return_type_t type, const char *type_name, const char *str_val, int32_t int_val, const void *data, uint32_t size);
void process_return_val_init(process_return_value_t *val);
void process_return_val_free_fields(process_return_value_t *val);
void process_return_val_reset(process_return_value_t *val);
void process_return_val_set_type_name(process_return_value_t *val, const char *name);
void process_return_val_set_str(process_return_value_t *val, const char *str);
void process_return_val_append_str(process_return_value_t *val, const char *str);
void process_return_val_set_struct_data(process_return_value_t *val, const void *data, uint32_t size);
void process_return_val_add_arg(process_return_value_t *val, const char *arg);
void process_return_val_copy(process_return_value_t *dst, const process_return_value_t *src);
process_t *process_get_current(void);
int process_adjust_stack_size(process_t *proc, int32_t delta);
void process_set_keep_alive(process_t *proc, int enabled);
void process_cleanup(process_t *proc);
void process_list_print(void);
int process_kill_by_pid(uint32_t pid);
int process_kill_all(void);
bool process_is_alive(uint32_t pid); /* true while PID is in the process list */
ipo_uarg_t process_signal_syscall_handler(ipo_uarg_t num, ipo_uarg_t argc,
                                          ipo_uarg_t *argv);
void process_signal_deliver_pending(void);
void process_signal_cleanup(uint32_t pid);

/* Cooperative and timer-preemptive thread scheduling */
void process_yield(void);
void process_yield_kernel(void);
bool process_thread_init(process_t *proc);
bool process_thread_has_runnable(process_t *proc);
process_thread_t *process_thread_pick_next(process_t *proc);
void process_thread_cleanup(process_t *proc);
bool process_update_memory_mappings(process_t *proc, ipo_vaddr_t address,
                                    ipo_size_t length, bool allocated);
void process_thread_main_exit(process_t *proc);
ipo_uarg_t process_thread_syscall_handler(ipo_uarg_t num, ipo_uarg_t argc,
                                          ipo_uarg_t *argv);
bool process_thread_handle_stack_fault(process_t *proc,
                                       process_thread_t *thread,
                                       ipo_vaddr_t fault_address,
                                       ipo_reg_t error_code,
                                       ipo_vaddr_t saved_sp);
void process_ipc_set_waiting(process_t *proc, uint32_t endpoint_id, uint8_t wait_kind);
void process_ipc_wake_waiters(uint32_t endpoint_id, uint8_t wait_kind);
void process_ipc_cancel_batch_waiters(void);
bool process_ipc_consume_wait_cancelled(void);
process_t *process_spawn(const char *path, int argc, char **argv);
int process_run_batch(process_t **procs, int count);
int process_run_batch_ex(process_t **procs, int count, bool separate_windows);
int process_run_batch_windows(process_t **procs, int count, const int *window_ids);
int process_get_batch_exit_code(int idx);
bool process_is_batch_active(void);
bool process_get_separate_windows(void);
void process_init_text_screen(process_t *proc);
bool process_is_foreground(void);
process_t *process_get_foreground(void);
process_t *process_get_displayed_foreground(void);
void process_set_foreground(process_t *proc);
void process_switch_foreground_next(void);
void process_switch_foreground_prev(void);
void process_switch_foreground_to_workspace(int target_wid);
int process_workspace_get_proc_count(int wid);
bool process_heap_ensure_capacity(process_t *proc, uint32_t min_chunks);
bool process_in_process_context(void);
bool process_batch_has_terminal_procs(void);
bool process_wm_has_focus(void);
bool process_get_batch_wm_was_active(void);
bool process_close_current_workspace(void);
void process_on_wm_window_created(process_t *owner);
void process_on_wm_windows_empty(void);
void process_append_text_output(process_t *proc, const char *str, uint32_t len);
void process_map_app(process_t *proc);
process_t *process_get_mapped_app(void);
process_t *process_find_by_pid(uint32_t pid);
void process_set_current(process_t *proc);
bool process_is_valid(process_t *proc);
void process_crash_exit(void);
void process_emergency_crash_exit(void);

/* Discontinuous on-demand user heap chunks */
bool process_heap_init(process_t *proc);
void process_heap_cleanup(process_t *proc);
bool process_heap_can_expand(process_t *proc, ipo_vaddr_t new_limit);
bool process_heap_chunk_can_expand(process_t *proc, uint32_t chunk_idx, ipo_vaddr_t new_limit);
ipo_vaddr_t process_heap_allocate_new_chunk(process_t *proc, ipo_size_t needed_size);

void process_demand_load_step(process_t *proc);
void process_demand_load_finish(process_t *proc);

void process_set_return_format(process_t *proc, const char *fmt);
const char *process_get_return_format(const process_t *proc);

int binfmt_register(const char *name, const uint8_t *magic, uint32_t magic_len, uint32_t offset, const char *runner_path);
int binfmt_unregister(const char *name);
void binfmt_list(void);

#endif
