#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include <stdint.h>
#include <stdbool.h>
#include <file_system/ipo_fs.h>

typedef struct address_space address_space_t;

// Dynamic memory allocation for processes
#define PROCESS_HEAP_START       0x00800000u  // Start of process heap area (8 MiB)
#define USER_HEAP_START          0x10000000u  // Start of isolated user process heaps (256 MiB)
#define USER_HEAP_PROCESS_STRIDE 0x10000000u  // Dynamic stride between process heaps (256 MiB)

// Protection flags
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

// Return types and structure
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
    uint32_t raw_eax;
    uint32_t raw_edx;

    /* Self-expanding type name (no fixed 32-byte limit) */
    char *type_name;
    uint32_t type_name_len;
    uint32_t type_name_cap;

    /* Self-expanding formatted universal output string (no fixed 256-byte limit) */
    char *str_val;
    uint32_t str_val_len;
    uint32_t str_val_cap;

    /* Self-expanding raw struct/binary data (no fixed 64-byte limit) */
    uint8_t *struct_data;
    uint32_t struct_size;
    uint32_t struct_data_cap;

    /* Self-expanding argument array for pipeline execution via run */
    char **arg_vals;
    uint32_t arg_count;
    uint32_t arg_cap;
} process_return_value_t;

// Process structure
typedef struct process_thread process_thread_t;

typedef struct process {
    uint32_t pid;
    
    // Process memory
    void *binary_base;      // Base address of the loaded binary
    uint32_t binary_size;   // Binary size
    uint32_t entry_point;   // Absolute entry point address
    void *binary_storage;   // Dedicated storage buffer when swapped out
    uint32_t data_offset;   // Offset where writeable data/bss begins
    uint32_t data_size;     // Size of writeable data/bss
    bool code_mapped;       // True if code segment has been loaded into 0x800000
    
    // Stack
    void *stack_base;       // Allocated process stack
    uint32_t stack_ptr;     // Current stack pointer
    uint32_t stack_start;   // Start of the stack region
    uint32_t stack_size;    // Stack size
    process_thread_t *threads;
    process_thread_t *current_thread;
    uint32_t next_thread_id;
    address_space_t *address_space;
    
    // Arguments
    int argc;               // Number of arguments
    uint32_t argv_addr;     // Address of argv array in process space
    char **argv_kernel;     // Copy of argv in kernel space (for debugging)
    
    // State
    int exit_code;          // Exit code
    process_return_value_t return_val; // Full typed return value
    bool return_val_explicit; // True if process explicitly set custom return value
    uint8_t is_running;     // Running flag
    bool waiting_for_input; // Blocked waiting for user input
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
    bool wants_graphics;    // Process requested VGA graphics mode (Mode 13h)
    uint32_t gfx_overlay_flags; // Flags for raw VGA overlay (VGA_OVERLAY_HIDE_BUTTON, etc.)
    bool is_wm_app;         // Process created a Window Manager window
    uint32_t async_task_count; // number of active async tasks owned by this process
    uint32_t user_heap_base;   // Per-process isolated heap start address
    uint32_t user_heap_break;  // Per-process sbrk break point
    uint32_t user_heap_limit;  // Per-process isolated heap limit address
    int32_t  user_heap_slot;   // Dynamic slot index (-1 if none)

    // Demand Loading
    struct ipo_inode exec_inode;        // Executable inode for on-demand progressive loading
    bool     demand_loading;            // True if executable is being loaded on demand
    uint32_t demand_file_offset;        // Current offset in binary on disk
    uint32_t demand_mem_offset;         // Current offset in process memory
    uint32_t demand_loaded_bytes;       // Bytes loaded into memory so far
    uint32_t demand_total_bytes;        // Total bytes of loadable segments

    uint16_t *text_vram_backup; // Backup of 80x25 text VRAM when swapped to background
    uint8_t *gfx_vram_backup;   // Backup of 320x200 graphics VRAM when swapped to background
    uint8_t *gfx_palette_backup; // Backup of 256-color DAC palette (768 bytes) when swapped to background
    uint16_t text_cursor_pos;   // Saved cursor position in text mode
    bool text_cursor_visible;   // Saved cursor visibility in text mode
    bool completed_and_acknowledged; // Process completed execution and was acknowledged / closed

    uint16_t *scroll_top_buffer;    // Workspace top scrollback buffer
    int scroll_top_capacity;        // Allocated capacity in lines
    uint16_t *scroll_bottom_buffer; // Workspace bottom scrollback buffer
    int scroll_bottom_capacity;     // Allocated capacity in lines
    int scroll_top_count;           // Number of scrolled top lines
    int scroll_bottom_count;        // Number of scrolled bottom lines
    int window_id;                  // Workspace / window group ID in batch mode
    bool text_screen_shared;        // True if text/gfx buffers are shared with another process in same window

    char *text_output_buf;      // Captured stdout text for replay
    uint32_t text_output_len;
    uint32_t text_output_cap;

    // Debugging and Reflection
    char *name;             // Process name (dynamically allocated)
    char *return_format;    // Format descriptor for returned data (int, string, struct, etc.)
    
    // Links
    struct process *next;   // For process list
} process_t;

typedef enum {
    PROCESS_THREAD_RUNNABLE = 0,
    PROCESS_THREAD_BLOCKED,
    PROCESS_THREAD_EXITED
} process_thread_state_t;

struct process_thread {
    uint32_t id;
    uint32_t context_esp;
    void *stack_base;
    uint32_t stack_size;
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
    uint32_t signal_altstack_base;
    uint32_t signal_altstack_size;
    uint32_t signal_altstack_flags;
    bool signal_altstack_active;
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
uintptr_t process_current_context_token(void);

// Entry point signature with arguments
typedef int (*process_entry_t)(int argc, char **argv);

// Function prototypes
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
uint32_t process_signal_syscall_handler(uint32_t num, uint32_t argc,
                                        uint32_t *argv);
void process_signal_deliver_pending(void);

/* Cooperative and timer-preemptive thread scheduling */
void process_yield(void);
void process_yield_kernel(void);
bool process_thread_init(process_t *proc);
bool process_thread_has_runnable(process_t *proc);
process_thread_t *process_thread_pick_next(process_t *proc);
void process_thread_cleanup(process_t *proc);
bool process_update_memory_mappings(process_t *proc, uint32_t address,
                                    uint32_t length, bool allocated);
void process_thread_main_exit(process_t *proc);
uint32_t process_thread_syscall_handler(uint32_t num, uint32_t argc,
                                        uint32_t *argv);
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
bool process_is_foreground(void);
process_t *process_get_foreground(void);
process_t *process_get_displayed_foreground(void);
void process_set_foreground(process_t *proc);
void process_switch_foreground_next(void);
void process_switch_foreground_prev(void);
void process_switch_foreground_to_workspace(int target_wid);
bool process_in_process_context(void);
void process_map_app(process_t *proc);
process_t *process_get_mapped_app(void);
process_t *process_find_by_pid(uint32_t pid);
void process_set_current(process_t *proc);
bool process_is_valid(process_t *proc);
void process_crash_exit(void);
void process_emergency_crash_exit(void);

/* Process isolated heap management (dynamic on-demand, no static limits) */
bool process_heap_init(process_t *proc);
void process_heap_cleanup(process_t *proc);
bool process_heap_can_expand(process_t *proc, uint32_t new_limit);

/* Demand / Lazy Loading */
void process_demand_load_step(process_t *proc);
void process_demand_load_finish(process_t *proc);

/* Return Reflection */
void process_set_return_format(process_t *proc, const char *fmt);
const char *process_get_return_format(const process_t *proc);

/* Custom Binary Format Registry */
int binfmt_register(const char *name, const uint8_t *magic, uint32_t magic_len, uint32_t offset, const char *runner_path);
int binfmt_unregister(const char *name);
void binfmt_list(void);

#endif
