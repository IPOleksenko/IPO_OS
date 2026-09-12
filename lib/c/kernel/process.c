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
static process_t *currently_mapped_app = NULL;

void process_map_app(process_t *proc) {
    if (currently_mapped_app == proc) {
        return;
    }

    if (currently_mapped_app != NULL && currently_mapped_app->binary_storage != NULL) {
        // Save previous app's modified data/bss from 0x800000
        memcpy(currently_mapped_app->binary_storage, (void *)PROCESS_HEAP_START, currently_mapped_app->binary_size);
    }

    if (proc != NULL && proc->binary_storage != NULL) {
        // Load this app's code/data/bss into 0x800000
        memcpy((void *)PROCESS_HEAP_START, proc->binary_storage, proc->binary_size);
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
    int running = 0;
    for (int i = 0; i < batch_proc_count; i++) {
        if (batch_procs[i] && batch_procs[i]->is_running) {
            if (batch_procs[i]->waiting_for_input && batch_procs[i] != fg) {
                continue;
            }
            running++;
        }
    }

    if (current_process == fg && running <= 1) {
        return;
    }

    in_process_context = false;
    process_switch_context(&current_process->stack_ptr, scheduler_esp);
    in_process_context = true;
}

process_t *process_get_foreground(void) {
    if (!batch_active) {
        return current_process;
    }
    for (int i = batch_proc_count - 1; i >= 0; i--) {
        if (batch_procs && batch_procs[i] && batch_procs[i]->is_running) {
            return batch_procs[i];
        }
    }
    return NULL;
}

bool process_is_foreground(void) {
    if (batch_active) {
        bool is_fg = (process_get_foreground() == current_process);
        if (!is_fg && current_process) {
            current_process->waiting_for_input = true;
        }
        return is_fg;
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

static char *default_envp[] = {
    "PATH=/app",
    "USER=root",
    "HOME=/",
    "SHELL=/app/term_ctl",
    "TERM=xterm",
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
        exit_code = entry(proc->argc, argv, default_envp);
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
        process_cleanup(proc);
        log_process_heap_state("after cleanup");
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

    if (proc->async_task_count > 0) {
        proc->is_running = 0;
        return;
    }
    
    if (currently_mapped_app == proc) {
        currently_mapped_app = NULL;
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

/**
 * universal_exec_load - Universally loads any file as an executable image.
 *
 * Every file is loaded and executed identically:
 *   - No format checks (zero magic byte checks, zero ELF, zero IPOB, zero PE).
 *   - Entire file image is loaded at PROCESS_HEAP_START (0x00800000).
 *   - Entry point is uniformly PROCESS_HEAP_START.
 */
static int universal_exec_load(process_t *proc, const uint8_t *file_data, uint32_t file_size) {
    if (!proc || !file_data || file_size == 0) {
        return -1;
    }

    /* Integer overflow check against process address space */
    if (file_size > UINT32_MAX - PROCESS_HEAP_START) {
        return -1;
    }

    proc->binary_storage = kmalloc(file_size);
    if (!proc->binary_storage) {
        return -1;
    }

    memset(proc->binary_storage, 0, file_size);
    memcpy(proc->binary_storage, file_data, file_size);

    proc->binary_base = (void *)PROCESS_HEAP_START;
    proc->binary_size = file_size;
    proc->entry_point = (uint32_t)PROCESS_HEAP_START;

    serial_printf("[universal_exec_load] Universally loaded: size=%u, entry=0x%x\n",
                  file_size, proc->entry_point);

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

static int last_batch_exit_codes[32];
static int last_batch_count = 0;

int process_get_batch_exit_code(int idx) {
    if (idx >= 0 && idx < last_batch_count) {
        return last_batch_exit_codes[idx];
    }
    return 0;
}

/**
 * process_run_batch - Executes one or more processes concurrently using cooperative multitasking
 */
int process_run_batch(process_t **procs, int count) {
    if (!procs || count <= 0) {
        return -1;
    }

    process_t *old_process = current_process;
    process_t **old_batch_procs = batch_procs;
    int old_batch_proc_count = batch_proc_count;
    int old_batch_current_idx = batch_current_idx;
    bool old_batch_active = batch_active;
    uint32_t old_scheduler_esp = scheduler_esp;
    bool old_in_process_context = in_process_context;

    batch_procs = procs;
    batch_proc_count = count;
    batch_current_idx = 0;
    batch_active = true;
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

    process_t *fg_init = process_get_foreground();
    if (fg_init != NULL) {
        for (int i = 0; i < count; i++) {
            if (procs[i] == fg_init) {
                batch_current_idx = i;
                break;
            }
        }
    }

    process_t *last_fg = NULL;
    system_clear_interrupt();

    while (1) {
        if (vga_is_graphics_mode() || wm_session_active()) {
            had_graphics = true;
        }

        if (system_is_interrupted()) {
            break;
        }

        process_t *fg = process_get_foreground();
        if (fg && fg->wants_graphics) {
            had_graphics = true;
        }
        if (fg != last_fg) {
            keyboard_clear_key_state();
            keyboard_flush_hardware();
            keyboard_flush_queue();
            keyboard_flush_app_queue();
            if (fg == NULL) {
                keyboard_set_app_input_mode(false);
                if (wm_session_active()) {
                    vga_set_mode_13h_hardware();
                    vga_gfx_init_default_palette();
                    mouse_set_bounds(VGA_GFX_WIDTH, VGA_GFX_HEIGHT);
                    wm_invalidate_all();
                } else {
                    vga_set_mode_text_hardware();
                }
            } else {
                fg->waiting_for_input = false;
                keyboard_set_app_input_mode(true);
                if (fg->wants_graphics) {
                    vga_set_mode_13h_hardware();
                    vga_gfx_init_default_palette();
                } else {
                    vga_set_mode_text_hardware();
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

        int running_count = 0;
        for (int i = 0; i < count; i++) {
            if (procs[i] && procs[i]->is_running) {
                running_count++;
            }
        }
        if (running_count == 0) {
            if (wm_session_active()) {
                async_scheduler_tick();
                wm_compositor_tick();
                io_wait();
                continue;
            }
            break;
        }

        // Drive async tasks and WM compositor if active and no full-screen app is running
        async_scheduler_tick();
        if (wm_session_active() && fg == NULL) {
            wm_compositor_tick();
        }

        // Find next runnable process
        int found = -1;
        for (int step = 0; step < count; step++) {
            int idx = (batch_current_idx + step) % count;
            process_t *p = procs[idx];
            if (p && p->is_running) {
                if (p->waiting_for_input && p != fg) {
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
            }
            if (found == -1) {
                io_wait();
                continue;
            }
        }

        batch_current_idx = (found + 1) % count;
        current_process = procs[found];

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
    batch_procs = old_batch_procs;
    batch_proc_count = old_batch_proc_count;
    batch_current_idx = old_batch_current_idx;
    scheduler_esp = old_scheduler_esp;
    in_process_context = old_in_process_context;
    current_process = old_process;

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
        if (had_graphics && batch_saved_screen && !wm_session_active()) {
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
        if (had_graphics && batch_saved_screen) {
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
    }

    if (batch_saved_screen) {
        kfree(batch_saved_screen);
        batch_saved_screen = NULL;
    }

    last_batch_count = (count < 32) ? count : 32;
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
    return current_process;
}

void process_set_current(process_t *proc) {
    current_process = proc;
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
 * process_is_alive - Check if a process with @pid is still in the process list.
 * Returns true if the process exists (even in background/keep-alive state).
 */
bool process_is_alive(uint32_t pid) {
    process_t *curr = process_list;
    while (curr != NULL) {
        if (curr->pid == pid) return true;
        curr = curr->next;
    }
    return false;
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
