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
    uint32_t async_task_count; // number of active async tasks owned by this process
    
    // Debugging
    char name[256];         // Process name
    
    // Links
    struct process *next;   // For process list
} process_t;

// Entry point signature with arguments
typedef int (*ipob_entry_t)(int argc, char **argv);

// Function prototypes
void process_init(void);
int process_exec(const char *path, int argc, char **argv);
int process_get_exit_code(void);
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
process_t *process_get_foreground(void);
bool process_is_foreground(void);
bool process_in_process_context(void);
void process_map_app(process_t *proc);
process_t *process_get_mapped_app(void);
void process_set_current(process_t *proc);
bool process_is_valid(process_t *proc);

#endif
