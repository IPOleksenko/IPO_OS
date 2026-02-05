#include <kernel/process.h>
#include <file_system/ipo_fs.h>
#include <memory/kmalloc.h>
#include <vga.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

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

// Global variables
static int last_exit_code = 0;
static process_t *current_process = NULL;
static process_t *process_list = NULL;
static uint32_t next_pid = 1;

/**
 * process_init - Initialize process manager
 */
void process_init(void) {
    // Initialize memory allocator
    kmalloc_init();
    
    printf("Process manager initialized\n");
}

/**
 * allocate_process_memory - Allocates memory for a process at a fixed address.
 */
static void *allocate_process_memory(uint32_t base, uint32_t size, uint32_t prot_flags) {
    void *addr = kmalloc(size);
    if (!addr) {
        return NULL;
    }
    
    return addr;
}

/**
 * free_process_memory - Frees up process memory
 */
static void free_process_memory(void *addr, uint32_t size) {
    kfree(addr);
}

/**
 * copy_string_to_process - Copies a string into the process's address space.
 */
static uint32_t copy_string_to_process(process_t *proc, const char *str) {
    if (!str) return 0;
    
    size_t len = strlen(str) + 1;
    
    // Allocate memory in the process stack for the string
    proc->stack_ptr -= len;
    proc->stack_ptr &= ~0x3;  // 4-byte alignment
    
    // Copy the line
    memcpy((void*)proc->stack_ptr, str, len);
    
    return proc->stack_ptr;
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
    
    // Limiting the number of arguments
    if (argc > MAX_ARGV_COUNT) {
        argc = MAX_ARGV_COUNT;
    }
    
    // Allocate memory in the kernel for the argv pointer array
    uint32_t argv_array_size = (argc + 1) * sizeof(uint32_t);
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
 */
static uint32_t setup_stack(process_t *proc) {
    proc->stack_ptr = PROCESS_STACK_TOP;
    proc->stack_start = PROCESS_STACK_TOP - PROCESS_STACK_SIZE;
    proc->stack_size = PROCESS_STACK_SIZE;
    
    return PROCESS_STACK_TOP;
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
    
    if (stat.size < IPOB_HEADER_SIZE) {
        printf("File too small: %s (%d bytes)\n", path, stat.size);
        return -1;  // File too small for header
    }
    
    if (stat.size > MAX_PROCESS_SIZE) {
        printf("File too large: %s (%d bytes, max %d)\n", 
               path, stat.size, MAX_PROCESS_SIZE);
        return -1;  // File too large
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
            kfree(binary_image);
            printf("Read failed at offset %d, read %d bytes\n", total_read, bytes_read);
            return -4;  // Read failed
        }
        
        total_read += bytes_read;
        serial_printf("Read chunk: %d bytes, total: %d/%d\n", bytes_read, total_read, stat.size);
    }
    
    // Parse and check the header
    ipob_header_t *header = (ipob_header_t *)binary_image;
    
    if (memcmp(header->magic, "IPO_B\x00\x00\x00", 8) != 0) {
        kfree(binary_image);
        printf("Invalid magic in file: %s\n", path);
        return -2;  // Invalid executable format
    }
    
    if (header->entry_offset >= stat.size) {
        kfree(binary_image);
        printf("Entry offset out of bounds: %d >= %d\n", header->entry_offset, stat.size);
        return -2;  // Entry offset out of bounds
    }
    
    if (header->total_size < stat.size) {
        printf("Warning: header total_size (%d) < actual size (%d)\n", 
               header->total_size, stat.size);
    }
    
    // Success: Copy the title and return
    if (header_out != NULL) {
        memcpy(header_out, header, IPOB_HEADER_SIZE);
    }
    
    *data_out = binary_image;
    serial_printf("File loaded successfully\n");
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
    
    // Freeing up binary memory
    if (proc->binary_base) {
        free_process_memory(proc->binary_base, proc->binary_size);
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
    
    kfree(proc);
}

/**
 * process_exec - The main function for executing a process with arguments
 */
int process_exec(const char *path, int argc, char **argv) {
    if (path == NULL) {
        return -1;
    }
    
    serial_printf("process_exec: %s, argc=%d\n", path, argc);
    
    // Creating a process structure
    process_t *proc = kmalloc(sizeof(process_t));
    if (!proc) {
        printf("Failed to allocate process structure\n");
        return -2;
    }
    
    memset(proc, 0, sizeof(process_t));
    proc->pid = next_pid++;
    proc->is_running = 1;
    
    // Add to the list of processes
    proc->next = process_list;
    process_list = proc;
    
    // Uploading the file
    ipob_header_t header;
    void *binary_image;
    
    int size = load_ipob_file(path, &header, &binary_image);
    if (size < 0) {
        printf("Failed to load file: error %d\n", size);
        process_cleanup(proc);
        return size;
    }
    
    serial_printf("File loaded, entry offset: 0x%x, total size: %d\n", 
           header.entry_offset, header.total_size);
    
    // Allocating memory at a fixed address
    // Applications are compiled for the PROCESS_BASE_ADDR address
    void *target_addr = allocate_process_memory(PROCESS_BASE_ADDR, size, 
                                               PROT_READ | PROT_WRITE | PROT_EXEC);
    
    if (!target_addr) {
        printf("Failed to allocate memory at 0x%x\n", PROCESS_BASE_ADDR);
        kfree(binary_image);
        process_cleanup(proc);
        return -5;
    }
    
    // Copy the binary to the desired address
    memcpy(target_addr, binary_image, size);
    
    // Relocate if necessary.
    if (relocate_binary(target_addr, PROCESS_BASE_ADDR, size) < 0) {
        printf("Relocation failed\n");
        free_process_memory(target_addr, size);
        kfree(binary_image);
        process_cleanup(proc);
        return -6;
    }
    
    // Freeing up the temporary buffer
    kfree(binary_image);
    
    // Saving information about the process
    proc->binary_base = target_addr;
    proc->binary_size = size;
    // Entry point is relative to where we actually loaded the binary in memory
    proc->entry_point = (uint32_t)target_addr + header.entry_offset;
    
    // Setting up the stack
    proc->stack_ptr = PROCESS_STACK_TOP;
    
    // Setting up arguments - argv is allocated in kernel memory
    uint32_t argv_addr = 0;
    if (setup_arguments(proc, argc, argv, &argv_addr) < 0) {
        printf("Failed to setup arguments\n");
        process_cleanup(proc);
        return -7;
    }
    
    // Setting up the stack for calling main()
    setup_stack(proc);
    
    serial_printf("Process %d ready: entry=0x%x, argc=%d, argv=0x%x\n",
           proc->pid, proc->entry_point, proc->argc, argv_addr);
    
    // Save the current process
    process_t *old_process = current_process;
    current_process = proc;
    
    // Call the entry point with arguments
    serial_printf("Calling entry point with argc=%d, argv at 0x%x...\n", proc->argc, argv_addr);
    
    // The entry point has a signature: int main(int argc, char **argv)
    // argv is now a pointer to an array in kernel memory
    char **argv_ptr = (char**)argv_addr;
    
    // Call with arguments
    typedef int (*entry_func_t)(int, char**);
    entry_func_t entry_point = (entry_func_t)proc->entry_point;
    int exit_code = entry_point(proc->argc, argv_ptr);
    last_exit_code = exit_code;
    
    serial_printf("Process returned\n");
    
    // Restoring the old process
    current_process = old_process;
    
    // Cleaning resources
    process_cleanup(proc);
    
    return proc->pid;
}

/**
 * process_exec_simple - Simplified launch without arguments
 */
int process_exec_simple(const char *path) {
    return process_exec(path, 0, NULL);
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