#include <kernel/process.h>
#include <file_system/ipo_fs.h>
#include <memory/kmalloc.h>
#include <vga.h>
#include <string.h>
#include <stdio.h>

#define MAX_PROCESS_SIZE (10 * 1024 * 1024)  // 10 MB max per app

/**
 * IPO_BINARY Application Header (20 bytes)
 * 
 * Offset  Size   Field           Description
 * ------  ----   -----           -----------
 * 0       8      MAGIC           Magic: "IPO_B" + 3 padding bytes
 * 8       4      ENTRY_OFFSET    Offset to entry point (20)
 * 12      4      TOTAL_SIZE      Total size of file
 * 16      4      RESERVED        Reserved for future use
 */
typedef struct {
    uint8_t magic[8];
    uint32_t entry_offset;
    uint32_t total_size;
    uint32_t reserved;
} ipob_header_t;

#define IPOB_HEADER_SIZE 20

// Global process state
static int last_exit_code = 0;

/**
 * process_init - Initialize process manager
 */
void process_init(void) {
    // Initialize memory allocator
    kmalloc_init();
}

/**
 * load_ipob_file - Load and validate an IPOB executable
 * 
 * @path: Path to the IPOB file
 * @header_out: Where to store the parsed header
 * @data_out: Where to store pointer to allocated binary image
 * 
 * Returns: Size of binary on success, negative on error
 */
static int load_ipob_file(const char *path, ipob_header_t *header_out, void **data_out) {
    if (data_out == NULL) {
        return -1;
    }
    
    *data_out = NULL;
    
    // Check if file exists and is a regular file
    struct ipo_inode stat;
    if (!ipo_fs_stat(path, &stat)) {
        return -1;  // File not found
    }
    
    if ((stat.mode & IPO_INODE_TYPE_DIR) != 0) {
        return -1;  // Path is a directory
    }
    
    if (stat.size < IPOB_HEADER_SIZE) {
        return -1;  // File too small for header
    }
    
    if (stat.size > MAX_PROCESS_SIZE) {
        return -1;  // File too large
    }
    
    // Allocate memory for the entire binary
    void *binary_image = kmalloc(stat.size);
    if (binary_image == NULL) {
        return -3;  // Memory allocation failed
    }
    
    // Read file into memory
    int fd = ipo_fs_open(path);
    if (fd < 0) {
        kfree(binary_image);
        return -4;  // File open failed
    }
    
    int bytes_read = ipo_fs_read(fd, binary_image, stat.size, 0);
    
    if (bytes_read < (int)stat.size) {
        kfree(binary_image);
        return -4;  // Read failed or incomplete
    }
    
    // Parse and validate header
    ipob_header_t *header = (ipob_header_t *)binary_image;
    
    if (memcmp(header->magic, "IPO_B\x00\x00\x00", 8) != 0) {
        kfree(binary_image);
        return -2;  // Invalid executable format
    }
    
    if (header->entry_offset >= stat.size) {
        kfree(binary_image);
        return -2;  // Entry offset out of bounds
    }
    
    if (header->total_size < stat.size) {
        kfree(binary_image);
        return -2;  // Invalid total size
    }
    
    // Success: copy header and return
    if (header_out != NULL) {
        memcpy(header_out, header, IPOB_HEADER_SIZE);
    }
    
    *data_out = binary_image;
    return stat.size;
}

/**
 * Execute an IPOB application
 * 
 * Entry point function signature: int (*entry_point)(void)
 * The application should return an integer exit code using 'return <code>;' in its
 * entry function. The kernel will capture and store this in 'last_exit_code'.
 */
typedef int (*ipob_entry_t)(void);

/**
 * process_exec_with_args - Execute application with arguments
 */
int process_exec(const char *path, int argc, char **argv) {
    if (path == NULL) {
        return -1;
    }
    
    ipob_header_t header;
    void *binary_image;
    
    int size = load_ipob_file(path, &header, &binary_image);
    if (size < 0) {
        return size;  // Return error code from loader
    }
    
    // Applications are compiled with PIC to fixed address 0x10000000
    // Execute from the allocated memory (kmalloc already returned valid address)
    // Relocation is not needed since we use PIC (Position Independent Code)
    
    // Get entry point address directly from allocated binary
    ipob_entry_t entry_point = (ipob_entry_t)(
        (uint8_t *)binary_image + header.entry_offset
    );
    
    /* Initialize exit code to 0. Applications should return a value from their
       entry point using a 'return' statement; we capture it below. */
    last_exit_code = 0;

    /* Run application entry point and capture its return value */
    int app_ret = entry_point();
    last_exit_code = app_ret;

    /* Free the binary image after execution */
    kfree(binary_image);

    /* Return success (PID 1 for now, real implementation would allocate PIDs) */
    return 1;
}

/**
 * Simplified execution without arguments
 */
int process_exec_simple(const char *path) {
    return process_exec(path, 0, NULL);
}

/**
 * Get exit code from last process
 */
int process_get_exit_code(void) {
    return last_exit_code;
}
