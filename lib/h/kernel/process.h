#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include <stdint.h>

// Maximum sizes
#define MAX_PROCESS_SIZE (512 * 1024 * 1024)  // 512 MB max per app
#define MAX_ARGV_COUNT 64
#define MAX_ARG_LENGTH 256

// Fixed addresses for process loading
#define PROCESS_BASE_ADDR   0x10000000  // Base address for all applications
#define PROCESS_STACK_TOP   0xC0000000  // Top of the stack
#define PROCESS_STACK_SIZE  (2 * 1024 * 1024)  // 2MB stack

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
    
    // Stack
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
int process_exec_simple(const char *path);
int process_get_exit_code(void);
process_t *process_get_current(void);
void process_cleanup(process_t *proc);

#endif
