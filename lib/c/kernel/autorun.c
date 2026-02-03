#include <kernel/autorun.h>
#include <kernel/process.h>
#include <kernel/terminal.h>
#include <file_system/ipo_fs.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <stdio.h>

#define AUTORUN_PATH "/autorun"
#define AUTORUN_BUF_SIZE (64 * 1024)
#define AUTORUN_LINE_SIZE 512

/**
 * Extract first token from line
 */
static void extract_token(const char *line, char *token_out, size_t max_len) {
    // Skip leading whitespace
    while (*line && (*line == ' ' || *line == '\t')) line++;
    
    // Extract token
    size_t pos = 0;
    while (*line && *line != ' ' && *line != '\t' && *line != '\n' && pos < max_len - 1) {
        token_out[pos++] = *line++;
    }
    token_out[pos] = '\0';
}

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
    printf("[autorun] Starting autorun system\n");
    
    struct ipo_inode stat;
    if (!ipo_fs_stat(AUTORUN_PATH, &stat)) {
        printf("[autorun] /autorun not found, skipping\n");
        return;
    }
    
    if ((stat.mode & IPO_INODE_TYPE_DIR) != 0) {
        printf("[autorun] /autorun is a directory, skipping\n");
        return;
    }
    
    if (stat.size > AUTORUN_BUF_SIZE) {
        printf("[autorun] /autorun too large (max %d bytes)\n", AUTORUN_BUF_SIZE);
        return;
    }
    
    // Allocate buffer for autorun file
    char *autorun_buf = kmalloc(stat.size + 1);
    if (!autorun_buf) {
        printf("[autorun] Memory allocation failed\n");
        return;
    }
    
    // Read autorun file
    int fd = ipo_fs_open(AUTORUN_PATH);
    if (fd < 0) {
        printf("[autorun] Failed to open /autorun\n");
        kfree(autorun_buf);
        return;
    }
    
    int bytes_read = ipo_fs_read(fd, autorun_buf, stat.size, 0);
    if (bytes_read < (int)stat.size) {
        printf("[autorun] Failed to read /autorun (read %d of %d bytes)\n", bytes_read, stat.size);
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
        char full_line[AUTORUN_LINE_SIZE];
        const char *line_start = ptr;
        int line_len = 0;
        
        // Copy until newline
        while (*ptr && *ptr != '\n' && line_len < (int)sizeof(full_line) - 1) {
            full_line[line_len++] = *ptr++;
        }
        full_line[line_len] = '\0';
        
        // Skip newline
        if (*ptr == '\n') ptr++;
        
        // Get just command name for logging
        char cmd_name[AUTORUN_LINE_SIZE];
        const char *space = strchr(full_line, ' ');
        if (space) {
            int cmd_len = space - full_line;
            if (cmd_len >= (int)sizeof(cmd_name)) cmd_len = sizeof(cmd_name) - 1;
            memcpy(cmd_name, full_line, cmd_len);
            cmd_name[cmd_len] = '\0';
        } else {
            strncpy(cmd_name, full_line, sizeof(cmd_name) - 1);
            cmd_name[sizeof(cmd_name) - 1] = '\0';
        }
        
        printf("[autorun] Line %d: executing '%s'\n", line_num, cmd_name);
        
        // Try to execute with full line (including arguments)
        int exec_result = try_execute_command(full_line);
        if (exec_result == 0) {
            printf("[autorun] Command '%s' not found\n", cmd_name);
        } else if (exec_result < 0) {
            printf("[autorun] Command '%s' execution failed (error %d)\n", cmd_name, exec_result);
        }
    }
    
    kfree(autorun_buf);
    printf("[autorun] Autorun complete\n");
}
