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
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <kernel/elf.h>

/**
 * IPO_BINARY Application Header (20 bytes)
 */
typedef struct {
    uint8_t magic[8];
    uint32_t entry_offset;
    uint32_t total_size;
    uint32_t reserved;
} ipob_header_t;

#define IPOB_HEADER_SIZE 20

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

    proc->stack_base = allocate_process_memory(proc, stack_size,
                                              PROT_READ | PROT_WRITE);
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

static int process_call_entry(ipob_entry_t entry, int argc, char **argv,
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
 * load_ipob_file - Downloads IPOB file with large file support
 */
static int load_ipob_file(const char *path, ipob_header_t *header_out, void **data_out) {
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
    
    serial_printf("Loading file: %s, size: %d bytes\n", path, stat.size);
    
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
    
    // Check if binary has legacy IPO_B header or is a pure flat binary
    ipob_header_t *header = (ipob_header_t *)binary_image;
    bool has_header = (stat.size >= IPOB_HEADER_SIZE &&
                       memcmp(header->magic, "IPO_B\x00\x00\x00", 8) == 0);
    
    if (has_header) {
        uint32_t payload_size = stat.size - IPOB_HEADER_SIZE;
        if (header->entry_offset >= payload_size) {
            kfree(binary_image);
            printf("Entry offset out of bounds: %d >= %d\n", header->entry_offset, payload_size);
            return -2;
        }
        if (header_out != NULL) {
            memcpy(header_out, header, IPOB_HEADER_SIZE);
        }
    } else {
        // Pure flat binary: no mandatory superblock/header, starts at offset 0
        if (header_out != NULL) {
            memset(header_out, 0, sizeof(ipob_header_t));
            header_out->entry_offset = 0;
            header_out->total_size = stat.size;
        }
    }
    
    *data_out = binary_image;
    serial_printf("File loaded successfully (has_header=%d)\n", has_header ? 1 : 0);
    return stat.size;
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
    
    // Keep the binary resident if it owns background async tasks.
    // The callback function pointer remains valid only while the code stays in memory.
    if (proc->binary_base) {
        free_process_memory(proc, proc->binary_base, proc->binary_size);
        proc->binary_base = NULL;
    }

    if (proc->stack_base) {
        if ((uintptr_t)proc->stack_base >= PROCESS_HEAP_START) {
            free_process_memory(proc, proc->stack_base, proc->stack_size);
        } else {
            kfree(proc->stack_base);
        }
        proc->stack_base = NULL;
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
        serial_printf("[process] All processes terminated, heap reset to 0\n");
    }
}

/**
 * process_exec - The main function for executing a process with arguments
 */
int process_exec(const char *path, int argc, char **argv) {
    if (path == NULL) {
        return -1;
    }

    char *resolved = NULL;
    const char *actual_path = path;
    if (path[0] != '/') {
        resolved = resolve_command_path(path);
        if (resolved != NULL) {
            actual_path = resolved;
        }
    }

    if (process_list == NULL) {
        global_process_heap_used = 0;
        block_count = 0;
    }

    serial_printf("process_exec: %s (actual=%s), argc=%d\n", path, actual_path, argc);
    log_process_heap_state("before exec");

    // Creating a process structure
    process_t *proc = kmalloc(sizeof(process_t));
    if (!proc) {
        printf("Failed to allocate process structure\n");
        if (resolved != NULL) kfree(resolved);
        return -2;
    }
    
    memset(proc, 0, sizeof(process_t));
    proc->pid = allocate_pid();
    proc->is_running = 1;
    
    // Store process name
    strncpy(proc->name, actual_path, sizeof(proc->name) - 1);
    
    // Add to the list of processes
    proc->next = process_list;
    process_list = proc;
    
    // Uploading the file
    ipob_header_t header;
    void *binary_image;
    
    int size = load_ipob_file(actual_path, &header, &binary_image);
    if (size < 0) {
        printf("Failed to load file: error %d\n", size);
        process_cleanup(proc);
        if (resolved != NULL) kfree(resolved);
        return size;
    }
    
    serial_printf("File loaded, entry offset: 0x%x, total size: %d\n", 
           header.entry_offset, header.total_size);
    
    bool is_elf = (size >= (int)sizeof(Elf32_Ehdr) &&
                   memcmp(binary_image, ELF_MAGIC, 4) == 0);

    if (is_elf) {
        const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)binary_image;
        if (ehdr->e_ident[4] != 1 || ehdr->e_ident[5] != 1 ||
            ehdr->e_machine != EM_386 || ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
            printf("Invalid ELF32 binary (requires x86 32-bit)\n");
            kfree(binary_image);
            process_cleanup(proc);
            if (resolved != NULL) kfree(resolved);
            return -5;
        }

        const Elf32_Phdr *phdrs = (const Elf32_Phdr *)((const uint8_t *)binary_image + ehdr->e_phoff);
        uint32_t min_vaddr = 0xFFFFFFFF;
        uint32_t max_vaddr = 0;
        for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
            if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_memsz > 0) {
                if (phdrs[i].p_vaddr < min_vaddr) min_vaddr = phdrs[i].p_vaddr;
                uint32_t seg_end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
                if (seg_end > max_vaddr) max_vaddr = seg_end;
            }
        }

        if (min_vaddr >= max_vaddr) {
            printf("No LOAD segments in ELF binary\n");
            kfree(binary_image);
            process_cleanup(proc);
            if (resolved != NULL) kfree(resolved);
            return -5;
        }

        uint32_t total_span = max_vaddr - min_vaddr;
        void *target_addr = allocate_process_memory(proc, total_span,
                                                   PROT_READ | PROT_WRITE | PROT_EXEC);
        if (!target_addr) {
            printf("Failed to allocate %u bytes for ELF binary\n", total_span);
            kfree(binary_image);
            process_cleanup(proc);
            if (resolved != NULL) kfree(resolved);
            return -5;
        }

        memset(target_addr, 0, total_span);
        uint32_t delta = (uint32_t)target_addr - min_vaddr;

        for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
            if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_memsz > 0) {
                uint8_t *seg_dest = (uint8_t *)phdrs[i].p_vaddr + delta;
                if (phdrs[i].p_filesz > 0) {
                    memcpy(seg_dest, (const uint8_t *)binary_image + phdrs[i].p_offset, phdrs[i].p_filesz);
                }
                if (phdrs[i].p_memsz > phdrs[i].p_filesz) {
                    memset(seg_dest + phdrs[i].p_filesz, 0, phdrs[i].p_memsz - phdrs[i].p_filesz);
                }
            }
        }

        kfree(binary_image);
        if (resolved != NULL) kfree(resolved);

        proc->binary_base = target_addr;
        proc->binary_size = total_span;
        proc->entry_point = ehdr->e_entry + delta;
        serial_printf("ELF32 loaded: span=%u, entry=0x%x\n", total_span, proc->entry_point);
    } else {
        bool has_header = (size >= IPOB_HEADER_SIZE &&
                           memcmp(((ipob_header_t *)binary_image)->magic, "IPO_B\x00\x00\x00", 8) == 0);
        uint32_t header_size = has_header ? IPOB_HEADER_SIZE : 0;
        uint32_t payload_size = size - header_size;

        // Allocating memory dynamically for the binary
        void *target_addr = allocate_process_memory(proc, payload_size,
                                                   PROT_READ | PROT_WRITE | PROT_EXEC);
        
        if (!target_addr) {
            printf("Failed to allocate memory for process (need %d bytes)\n", payload_size);
            kfree(binary_image);
            process_cleanup(proc);
            if (resolved != NULL) kfree(resolved);
            return -5;
        }
        
        memset(target_addr, 0, payload_size);
        memcpy(target_addr, (uint8_t *)binary_image + header_size, payload_size);
        
        // Relocate if necessary.
        uint32_t load_address = (uint32_t)target_addr;
        if (relocate_binary(target_addr, load_address, payload_size) < 0) {
            printf("Relocation failed\n");
            free_process_memory(proc, target_addr, payload_size);
            kfree(binary_image);
            process_cleanup(proc);
            if (resolved != NULL) kfree(resolved);
            return -6;
        }
        
        // Freeing up the temporary buffer
        kfree(binary_image);
        if (resolved != NULL) kfree(resolved);
        
        // Saving information about the process
        proc->binary_base = target_addr;
        proc->binary_size = payload_size;
        // Entry point is relative to where we actually loaded the binary in memory
        proc->entry_point = (uint32_t)target_addr + header.entry_offset;
    }
    
    // Setting up arguments - argv is allocated in kernel memory
    uint32_t argv_addr = 0;
    if (setup_arguments(proc, argc, argv, &argv_addr) < 0) {
        printf("Failed to setup arguments\n");
        process_cleanup(proc);
        return -7;
    }
    
    // Setting up the stack for calling main()
    if (setup_stack(proc) == 0) {
        printf("Failed to allocate process stack\n");
        process_cleanup(proc);
        return -7;
    }
    
    serial_printf("Process %d ready: entry=0x%x, argc=%d, argv=0x%x\n",
           proc->pid, proc->entry_point, proc->argc, argv_addr);
    
    // Save the current process
    process_t *old_process = current_process;
    current_process = proc;

    // Defensive validation: a broken application must never crash the kernel.
    if (proc->binary_base == NULL || proc->binary_size == 0 ||
        proc->entry_point < PROCESS_HEAP_START) {
        printf("[process] pid=%u: invalid app image, closing process safely\n", proc->pid);
        current_process = old_process;
        process_cleanup(proc);
        return -9;
    }
    
    // Call the entry point with arguments
    serial_printf("Calling entry point with argc=%d, argv at 0x%x...\n", proc->argc, argv_addr);
    log_process_heap_state("during exec");
    
    // The entry point has a signature: int main(int argc, char **argv)
    char **argv_ptr = (char**)argv_addr;

    // If the executable is invalid or takes too long, the kernel must still recover.
    typedef int (*entry_func_t)(int, char**);
    entry_func_t entry_point = (entry_func_t)proc->entry_point;
    if (entry_point == NULL) {
        printf("[process] pid=%u: entry point is NULL, closing process\n", proc->pid);
        current_process = old_process;
        process_cleanup(proc);
        return -10;
    }

    terminal_lock_input();
    system_set_state(SYSTEM_STATE_PROCESS_RUNNING);
    serial_printf("[process] pid=%u start execution: %s\n", proc->pid, path);

    int exit_code = process_call_entry(entry_point, proc->argc, argv_ptr,
                                       proc->stack_ptr);
    last_exit_code = exit_code;

    serial_printf("[process] pid=%u returned, exit_code=%d\n", proc->pid, exit_code);
    keyboard_set_app_input_mode(false);
    keyboard_clear_key_state();
    terminal_unlock_input();
    system_set_state(SYSTEM_STATE_TERMINAL_IDLE);
    vga_font_set_app_mode(false);
    dynamic_keymap_reapply_fonts();
    vga_cursor_reset();

    /* If Ctrl+C was used to stop the process, print a clean message */
    if (system_is_interrupted()) {
        printf("Application stopped.\n");
        system_clear_interrupt();
    }

    // Restoring the old process
    current_process = old_process;

    if (proc->async_task_count > 0) {
        proc->is_running = 0;
        proc->exit_code = exit_code;
        printf("[process] keeping process %u alive in background (owns %u async task(s))\n",
               proc->pid, proc->async_task_count);
        current_process = old_process;
        return proc->pid;
    }
    
    uint32_t finished_pid = proc->pid;

    // Cleaning resources
    process_cleanup(proc);
    log_process_heap_state("after cleanup");
    
    return finished_pid ? (int)finished_pid : 1;
}

/**
 * process_get_exit_code - Returns the exit code of the last process.
 */
int process_get_exit_code(void) {
    return last_exit_code;
}

/**
 * process_get_current - Returns the current process
 */
process_t *process_get_current(void) {
    return current_process;
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

        printf("%s\n", curr->name[0] ? curr->name : "unnamed");

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
            char name_copy[256];
            strncpy(name_copy, curr->name, sizeof(name_copy) - 1);
            name_copy[sizeof(name_copy) - 1] = '\0';

            /* Stop all async tasks owned by this process */
            async_stop_tasks_by_owner(curr);
            curr->async_task_count = 0;

            /* Cleanup memory and unlink from list */
            process_cleanup(curr);
            printf("[process] pid=%u (%s) killed and memory freed.\n", pid, name_copy);
            return 0;
        }
        curr = curr->next;
    }

    printf("kill: no process found with PID %u\n", pid);
    return -1;
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
        char name_copy[256];
        strncpy(name_copy, proc->name, sizeof(name_copy) - 1);
        name_copy[sizeof(name_copy) - 1] = '\0';
        uint32_t pid = proc->pid;

        process_cleanup(proc);
        printf("[process] killed pid=%u (%s)\n", pid, name_copy);
        killed++;
    }

    printf("[process] Terminated %u process(es).\n", killed);
    return (int)killed;
}
