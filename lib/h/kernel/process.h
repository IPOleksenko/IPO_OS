#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include <stdint.h>
#include <stdbool.h>

// Dynamic memory allocation for processes
#define PROCESS_HEAP_START  0x00800000  // Start of process heap area

// Protection flags
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

// Process structure
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
    
    // Arguments
    int argc;               // Number of arguments
    uint32_t argv_addr;     // Address of argv array in process space
    char **argv_kernel;     // Copy of argv in kernel space (for debugging)
    
    // State
    int exit_code;          // Exit code
    uint8_t is_running;     // Running flag
    bool waiting_for_input; // Blocked waiting for user input
    bool wants_graphics;    // Process requested VGA graphics mode (Mode 13h)
    bool is_wm_app;         // Process created a Window Manager window
    uint32_t async_task_count; // number of active async tasks owned by this process
    uint32_t user_heap_break;  // Per-process sbrk break point

    uint16_t *text_vram_backup; // Backup of 80x25 text VRAM when swapped to background
    uint8_t *gfx_vram_backup;   // Backup of 320x200 graphics VRAM when swapped to background
    uint16_t text_cursor_pos;   // Saved cursor position in text mode
    bool text_cursor_visible;   // Saved cursor visibility in text mode
    bool completed_and_acknowledged; // Process completed execution and was acknowledged / closed

    uint16_t *scroll_top_buffer;    // Workspace top scrollback buffer
    uint16_t *scroll_bottom_buffer; // Workspace bottom scrollback buffer
    int scroll_top_count;           // Number of scrolled top lines
    int scroll_bottom_count;        // Number of scrolled bottom lines
    int window_id;                  // Workspace / window group ID in batch mode
    bool text_screen_shared;        // True if text/gfx buffers are shared with another process in same window

    char *text_output_buf;      // Captured stdout text for replay
    uint32_t text_output_len;
    uint32_t text_output_cap;

    // Debugging
    char *name;             // Process name (dynamically allocated)
    
    // Links
    struct process *next;   // For process list
} process_t;

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

// Entry point signature with arguments
typedef int (*process_entry_t)(int argc, char **argv);

// Function prototypes
void process_init(void);
int process_exec(const char *path, int argc, char **argv);
int process_get_exit_code(void);
void process_set_last_exit_code(int code);
process_t *process_get_current(void);
int process_adjust_stack_size(process_t *proc, int32_t delta);
void process_set_keep_alive(process_t *proc, int enabled);
void process_cleanup(process_t *proc);
void process_list_print(void);
int process_kill_by_pid(uint32_t pid);
int process_kill_all(void);
bool process_is_alive(uint32_t pid); /* true while PID is in the process list */

/* Cooperative multitasking */
void process_yield(void);
void process_yield_kernel(void);
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
bool process_in_process_context(void);
void process_map_app(process_t *proc);
process_t *process_get_mapped_app(void);
process_t *process_find_by_pid(uint32_t pid);
void process_set_current(process_t *proc);
bool process_is_valid(process_t *proc);
void process_crash_exit(void);

#endif
