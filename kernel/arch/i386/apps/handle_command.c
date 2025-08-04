#include <ctype.h>
#include <string.h>
#include <stdio.h>

#include <kernel/apps/handle_command.h>
#include <kernel/drv/tty.h>
#include <kernel/sys/ioports.h>
#include <kernel/sys/kheap.h>
#include <kernel/sys/fs.h>
#include <kernel/sys/power.h>


// Function to remove extra spaces
void trim_spaces(char* str) {
    char* dst = str;
    char* src = str;
    
    // Skip leading spaces
    while (isspace((unsigned char)*src)) src++;
    
    while (*src) {
        if (isspace((unsigned char)*src)) {
            // Replace sequence of spaces with single space
            *dst++ = ' ';
            while (isspace((unsigned char)*src)) src++;
        } else {
            *dst++ = *src++;
        }
    }
    
    // Remove trailing spaces
    if (dst > str && isspace((unsigned char)dst[-1])) {
        dst--;
    }
    *dst = '\0';
}

// Function to parse command into parts
void parse_command(char* command, char** cmd, char** arg1, char** arg2) {
    *cmd = NULL;
    *arg1 = NULL;
    *arg2 = NULL;
    
    if (!command) return;
    
    // Simple command parser
    char* start = command;
    
    // Find first word (command)
    while (*start == ' ') start++; // skip spaces
    if (*start == '\0') return;
    
    *cmd = start;
    while (*start && *start != ' ') start++; // find end of command
    if (*start) {
        *start = '\0';
        start++;
        
        // Special handling for echo command: echo filename content
        if (strcmp(*cmd, "echo") == 0) {
            // Find first argument (filename)
            while (*start == ' ') start++; // skip spaces
            if (*start != '\0') {
                *arg1 = start;
                while (*start && *start != ' ') start++; // find end of filename
                if (*start) {
                    *start = '\0';
                    start++;
                    
                    // Rest of the line is the content (arg2)
                    while (*start == ' ') start++; // skip spaces
                    if (*start != '\0') {
                        *arg2 = start;
                    }
                }
            }
            return;
        }
        
        // Special handling for touch command with content
        if (strcmp(*cmd, "touch") == 0) {
            // Find first argument (filename)
            while (*start == ' ') start++; // skip spaces
            if (*start != '\0') {
                *arg1 = start;
                while (*start && *start != ' ') start++; // find end of filename
                if (*start) {
                    *start = '\0';
                    start++;
                    
                    // Rest of the line is the content (arg2)
                    while (*start == ' ') start++; // skip spaces
                    if (*start != '\0') {
                        *arg2 = start;
                    }
                }
            }
            return;
        }
        
        // Standard parsing for other commands
        // Find first argument
        while (*start == ' ') start++; // skip spaces
        if (*start != '\0') {
            *arg1 = start;
            while (*start && *start != ' ') start++; // find end of argument
            if (*start) {
                *start = '\0';
                start++;
                
                // Find second argument (rest of string)
                while (*start == ' ') start++; // skip spaces
                if (*start != '\0') {
                    *arg2 = start;
                }
            }
        }
    }
}

// Filesystem commands
void cmd_create_file(const char* filename) {
    if (!filename) {
        printf("Usage: touch <filename> [content]\n");
        return;
    }
    
    if (fs_create_file(filename) == 0) {
        printf("File '%s' created successfully\n", filename);
    } else {
        printf("Failed to create file '%s'\n", filename);
    }
}

void cmd_create_file_with_content(const char* filename, const char* content) {
    if (!filename) {
        printf("Usage: touch <filename> [content]\n");
        return;
    }
    
    // Create the file first
    if (fs_create_file(filename) != 0) {
        printf("Failed to create file '%s'\n", filename);
        return;
    }
    
    // If content is provided, write it to the file
    if (content && strlen(content) > 0) {
        if (fs_write_file(filename, content, strlen(content)) == 0) {
            printf("File '%s' created with content\n", filename);
        } else {
            printf("File '%s' created but failed to write content\n", filename);
        }
    } else {
        printf("Empty file '%s' created successfully\n", filename);
    }
}

void cmd_delete_file(const char* filename) {
    if (!filename) {
        printf("Usage: delete <filename>\n");
        return;
    }
    
    if (fs_delete_file(filename) == 0) {
        printf("File '%s' deleted successfully\n", filename);
    } else {
        printf("Failed to delete file '%s'\n", filename);
    }
}

void cmd_write_file(const char* filename, const char* content) {
    if (!filename || !content) {
        printf("Usage: write <filename> <content>\n");
        return;
    }
    
    if (fs_write_file(filename, content, strlen(content)) == 0) {
        printf("Data written to file '%s'\n", filename);
    } else {
        printf("Failed to write to file '%s'\n", filename);
    }
}

void cmd_read_file(const char* filename) {
    if (!filename) {
        printf("Usage: read <filename>\n");
        return;
    }
    
    int size = fs_get_file_size(filename);
    if (size < 0) {
        printf("File '%s' not found\n", filename);
        return;
    }
    
    if (size == 0) {
        printf("File '%s' is empty\n", filename);
        return;
    }
    
    char* buffer = (char*)kmalloc(size + 1);
    if (!buffer) {
        printf("Out of memory\n");
        return;
    }
    
    int bytes_read = fs_read_file(filename, buffer, size);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        printf("Content of '%s':\n%s\n", filename, buffer);
    } else {
        printf("Failed to read file '%s'\n", filename);
    }
    
    kfree(buffer);
}

void cmd_clear_file(const char* filename) {
    if (!filename) {
        printf("Usage: truncate <filename>\n");
        return;
    }
    
    // Check if file exists
    if (!fs_file_exists(filename)) {
        printf("File '%s' not found\n", filename);
        return;
    }
    
    // Clear file by writing empty content
    if (fs_write_file(filename, "", 0) == 0) {
        printf("File '%s' cleared successfully\n", filename);
    } else {
        printf("Failed to clear file '%s'\n", filename);
    }
}

// Directory commands
void cmd_create_directory(const char* dirname) {
    if (!dirname) {
        printf("Usage: mkdir <dirname>\n");
        return;
    }
    
    if (fs_create_directory(dirname) == 0) {
        printf("Directory '%s' created successfully\n", dirname);
    } else {
        printf("Failed to create directory '%s'\n", dirname);
    }
}

void cmd_delete_directory(const char* dirname) {
    if (!dirname) {
        printf("Usage: rmdir <dirname>\n");
        return;
    }
    
    if (fs_delete_directory(dirname) == 0) {
        printf("Directory '%s' deleted successfully\n", dirname);
    } else {
        printf("Failed to delete directory '%s'\n", dirname);
    }
}

void cmd_rename(const char* old_name, const char* new_name) {
    if (!old_name || !new_name) {
        printf("Usage: rename <old_name> <new_name>\n");
        return;
    }
    
    // First try to find the entry to determine if it's a file or directory
    file_entry_t* entry = fs_find_entry_by_path(old_name);
    if (!entry) {
        printf("File or directory '%s' not found\n", old_name);
        return;
    }
    
    if (entry->type == ENTRY_TYPE_DIRECTORY) {
        if (fs_rename_directory(old_name, new_name) == 0) {
            printf("Directory '%s' renamed to '%s' successfully\n", old_name, new_name);
        } else {
            printf("Failed to rename directory '%s' to '%s'\n", old_name, new_name);
        }
    } else if (entry->type == ENTRY_TYPE_FILE) {
        if (fs_rename_file(old_name, new_name) == 0) {
            printf("File '%s' renamed to '%s' successfully\n", old_name, new_name);
        } else {
            printf("Failed to rename file '%s' to '%s'\n", old_name, new_name);
        }
    } else {
        printf("Unknown entry type for '%s'\n", old_name);
    }
}

void cmd_move_entry(const char* source_path, const char* dest_path) {
    if (!source_path || !dest_path) {
        printf("Usage: mv <source_path> <dest_path>\n");
        return;
    }
    
    if (fs_move_entry(source_path, dest_path) != 0) {
        // Error message is already printed by fs_move_entry
    }
}

void cmd_change_directory(const char* dirname) {
    if (fs_change_directory(dirname) == 0) {
        printf("Changed to directory: %s\n", fs_get_current_path());
    } else {
        if (!dirname) {
            printf("Failed to change to root directory\n");
        } else {
            printf("Failed to change to directory '%s'\n", dirname);
        }
    }
}

void cmd_print_working_directory(void) {
    printf("%s\n", fs_get_current_path());
}

// Help system functions
void show_help_system(void) {
    printf("System Commands:\n");
    printf("  help              - Show list of help categories\n");
    printf("  help <category>   - Show help for specific category\n");
    printf("  meminfo           - Display RAM information\n");
    printf("  clear             - Clear the terminal screen\n");
    printf("  reboot            - Reboot PC\n");
    printf("  exit              - Exit the system\n");
}

void show_help_filesystem(void) {
    printf("File & Directory Management:\n");
    printf("  touch <filename> [content] - Create a new file with optional content\n");
    printf("  mkdir <dirname>   - Create a new directory\n");
    printf("  rm <filename>     - Delete a file\n");
    printf("  rmdir <dirname>   - Delete a directory\n");
    printf("  mv <source> <dest> - Move a file or directory to different location\n");
    printf("  rename <old_name> <new_name> - Rename a file or directory\n");
    printf("  ls                - List files and directories\n");
}

void show_help_file_content(void) {
    printf("File Content Operations:\n");
    printf("  echo <filename> <content> - Write content to file\n");
    printf("  cat <filename>    - Read file content\n");
    printf("  truncate <filename> - Clear file content (make file empty)\n");
}

void show_help_navigation(void) {
    printf("Navigation Commands:\n");
    printf("  pwd               - Print working directory\n");
    printf("  cd [dirname]      - Change directory (cd .. for parent, cd for root)\n");
    printf("  Note: Use ~ prefix for paths starting from home directory\n");
    printf("  Examples: cd ~/documents, mv file.txt ~/backup/\n");
}

void show_help_all(void) {
    printf("Available command categories:\n");
    printf("  help system       - System and control commands\n");
    printf("  help fs           - File & directory management\n");
    printf("  help content      - File content operations\n");
    printf("  help navigation   - Navigation and path commands\n");
    printf("\nUse 'help all' to see all commands at once.\n");
}

void show_help_complete(void) {
    show_help_system();
    printf("\n");
    show_help_filesystem();
    printf("\n");
    show_help_file_content();
    printf("\n");
    show_help_navigation();
}

void handle_command(char* command) {
    if (!command || *command == '\0') {
        printf("No command entered.\n");
        return;
    }

    trim_spaces(command);
    
    // Create copy of command for parsing
    size_t command_len = strlen(command);
    char* command_copy = (char*)kmalloc(command_len + 1);
    if (!command_copy) {
        printf("Out of memory\n");
        return;
    }
    strcpy(command_copy, command);
    
    char* cmd = NULL;
    char* arg1 = NULL;
    char* arg2 = NULL;
    
    parse_command(command_copy, &cmd, &arg1, &arg2);
    
    if (!cmd) {
        printf("No command entered.\n");
        return;
    }

    if (strcmp(cmd, "help") == 0){
        if (!arg1) {
            // No category specified - show categories overview
            show_help_all();
        } else if (strcmp(arg1, "system") == 0) {
            show_help_system();
        } else if (strcmp(arg1, "fs") == 0 || strcmp(arg1, "filesystem") == 0) {
            show_help_filesystem();
        } else if (strcmp(arg1, "content") == 0) {
            show_help_file_content();
        } else if (strcmp(arg1, "navigation") == 0) {
            show_help_navigation();
        } else if (strcmp(arg1, "all") == 0) {
            show_help_complete();
        } else {
            printf("Unknown help category '%s'\n", arg1);
            show_help_all();
        }
    } else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "clean") == 0) {
        terminal_clear();
    } else if (strcmp(cmd, "reboot") == 0) {
        reboot_system();
    } else if (strcmp(cmd, "exit") == 0) {
        shutdown_system();
    } else if (strcmp(cmd, "meminfo") == 0) {
        meminfo();
    } else if (strcmp(cmd, "ls") == 0) {
        fs_list_files();
    } else if (strcmp(cmd, "pwd") == 0) {
        cmd_print_working_directory();
    } else if (strcmp(cmd, "cd") == 0) {
        cmd_change_directory(arg1);
    } else if (strcmp(cmd, "mkdir") == 0) {
        cmd_create_directory(arg1);
    } else if (strcmp(cmd, "rmdir") == 0) {
        cmd_delete_directory(arg1);
    } else if (strcmp(cmd, "rename") == 0) {
        cmd_rename(arg1, arg2);
    } else if (strcmp(cmd, "mv") == 0) {
        cmd_move_entry(arg1, arg2);
    } else if (strcmp(cmd, "touch") == 0) {
        if (arg2) {
            cmd_create_file_with_content(arg1, arg2);
        } else {
            cmd_create_file(arg1);
        }
    } else if (strcmp(cmd, "rm") == 0) {
        cmd_delete_file(arg1);
    } else if (strcmp(cmd, "echo") == 0) {
        // Handle "echo filename content" syntax
        if (arg1 && arg2) {
            // Write content to file
            cmd_write_file(arg1, arg2);
        } else if (arg1) {
            // Just print filename to console (no content provided)
            printf("%s\n", arg1);
        } else {
            // No arguments
            printf("\n");
        }
    } else if (strcmp(cmd, "cat") == 0) {
        cmd_read_file(arg1);
    } else if (strcmp(cmd, "create") == 0) {
        cmd_create_file(arg1);
    } else if (strcmp(cmd, "delete") == 0) {
        cmd_delete_file(arg1);
    } else if (strcmp(cmd, "write") == 0) {
        cmd_write_file(arg1, arg2);
    } else if (strcmp(cmd, "read") == 0) {
        cmd_read_file(arg1);
    } else if (strcmp(cmd, "truncate") == 0) {
        cmd_clear_file(arg1);
    } else {
        printf("Unknown command.\n");
    }
    
    // Free allocated memory
    kfree(command_copy);
}