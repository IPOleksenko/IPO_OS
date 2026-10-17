#include <kernel/autorun.h>
#include <kernel/process.h>
#include <kernel/terminal.h>
#include <file_system/ipo_fs.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <stdio.h>

#define AUTORUN_PATH "/autorun"


/**
 * Skip rest of line
 */
static const char* skip_line(const char *line) {
    while (*line && *line != '\n') line++;
    if (*line == '\n') line++;
    return line;
}

/**
 * Execute autorun file
 */
void autorun_init(void) {
    serial_printf("[autorun] Starting autorun system\n");
    
    struct ipo_inode stat;
    if (!ipo_fs_stat(AUTORUN_PATH, &stat)) {
        serial_printf("[autorun] /autorun not found, skipping\n");
        return;
    }
    
    if ((stat.mode & IPO_INODE_TYPE_DIR) != 0) {
        serial_printf("[autorun] /autorun is a directory, skipping\n");
        return;
    }
    
    // Allocate buffer for autorun file
    char *autorun_buf = kmalloc(stat.size + 1);
    if (!autorun_buf) {
        serial_printf("[autorun] Memory allocation failed\n");
        return;
    }
    
    // Read autorun file
    int fd = ipo_fs_open(AUTORUN_PATH);
    if (fd < 0) {
        serial_printf("[autorun] Failed to open /autorun\n");
        kfree(autorun_buf);
        return;
    }
    
    int bytes_read = ipo_fs_read(fd, autorun_buf, stat.size, 0);
    ipo_fs_close(fd);
    if (bytes_read < (int)stat.size) {
        serial_printf("[autorun] Failed to read /autorun (read %d of %d bytes)\n", bytes_read, stat.size);
        kfree(autorun_buf);
        return;
    }
    
    autorun_buf[stat.size] = '\0';
    
    // Process each line
    const char *ptr = autorun_buf;
    int line_num = 0;
    
    while (*ptr) {
        line_num++;
        
        // Skip leading whitespace and comments
        while (*ptr && (*ptr == ' ' || *ptr == '\t')) ptr++;
        
        if (*ptr == '#') {
            // Comment line
            ptr = skip_line(ptr);
            continue;
        }
        
        if (*ptr == '\n') {
            // Empty line
            ptr++;
            continue;
        }
        
        if (*ptr == '\0') {
            break;
        }
        
        // Extract full line (command + arguments)
        const char *line_start = ptr;
        while (*ptr && *ptr != '\n') {
            ptr++;
        }
        int line_len = (int)(ptr - line_start);
        char *full_line = kmalloc(line_len + 1);
        if (!full_line) {
            if (*ptr == '\n') ptr++;
            continue;
        }
        memcpy(full_line, line_start, line_len);
        full_line[line_len] = '\0';
        
        // Skip newline
        if (*ptr == '\n') ptr++;
        
        serial_printf("[autorun] Line %d: executing '%s'\n", line_num, full_line);
        
        // Try to execute with full line (including arguments)
        int exec_result = try_execute_command(full_line);
        if (exec_result == 0) {
            serial_printf("[autorun] Command not found\n");
        } else if (exec_result < 0) {
            serial_printf("[autorun] Command execution failed (error %d)\n", exec_result);
        } else {
            serial_printf("[autorun] Return value: %s\n", process_get_last_return_string());
        }
        kfree(full_line);
    }
    
    kfree(autorun_buf);
    serial_printf("[autorun] Autorun complete\n");
}
