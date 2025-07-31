#include <ctype.h>
#include <string.h>

#include <kernel/apps/handle_command.h>
#include <kernel/drv/tty.h>
#include <kernel/sys/ioports.h>
#include <kernel/sys/pit.h>
#include <kernel/sys/kheap.h>
#include <kernel/sys/fs.h>

void shutdown_system() {
    terminal_writestring("Shutting down...\n");
    sleep(1000);
    outw(0x604, 0x2000); // ACPI shutdown
}

void reboot_system(){
    terminal_writestring("Rebooting...\n");
    sleep(1000);
    outb(0x64, 0xFE);
}

extern size_t heap_used;
extern size_t heap_size;

void meminfo(){
    printf("\nRAM:\n");
    printf("Used %ld B / %ld B\n", heap_used, heap_size);
    printf("Used %ld KB / %ld KB\n", heap_used/1024, heap_size/1024);
    printf("Used %ld MB / %ld MB\n", heap_used/1024/1024, heap_size/1024/1024);
    printf("\n");
}

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
        terminal_writestring("Usage: create <filename>\n");
        return;
    }
    
    if (fs_create_file(filename) == 0) {
        printf("File '%s' created successfully\n", filename);
    } else {
        printf("Failed to create file '%s'\n", filename);
    }
}

void cmd_delete_file(const char* filename) {
    if (!filename) {
        terminal_writestring("Usage: delete <filename>\n");
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
        terminal_writestring("Usage: write <filename> <content>\n");
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
        terminal_writestring("Usage: read <filename>\n");
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
        terminal_writestring("Out of memory\n");
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

void handle_command(char* command) {
    if (!command || *command == '\0') {
        terminal_writestring("No command entered.\n");
        return;
    }

    trim_spaces(command);
    
    // Create copy of command for parsing
    char command_copy[256];
    strncpy(command_copy, command, sizeof(command_copy) - 1);
    command_copy[sizeof(command_copy) - 1] = '\0';
    
    char* cmd = NULL;
    char* arg1 = NULL;
    char* arg2 = NULL;
    
    parse_command(command_copy, &cmd, &arg1, &arg2);
    
    if (!cmd) {
        terminal_writestring("No command entered.\n");
        return;
    }

    if (strcmp(cmd, "help") == 0){
        terminal_writestring("Available commands:\n");
        terminal_writestring("  help              - Show this help message\n");
        terminal_writestring("  meminfo           - Displaying RAM information\n");
        terminal_writestring("  clean             - Clear the terminal screen\n");
        terminal_writestring("  reboot            - Rebooting PC\n");
        terminal_writestring("  exit              - Exit the system\n");
        terminal_writestring("  ls                - List files\n");
        terminal_writestring("  create <filename> - Create a new file\n");
        terminal_writestring("  delete <filename> - Delete a file\n");
        terminal_writestring("  write <file> <text> - Write text to file\n");
        terminal_writestring("  read <filename>   - Read file content\n");
    } else if (strcmp(cmd, "clean") == 0) {
        terminal_clear();
    } else if (strcmp(cmd, "reboot") == 0) {
        reboot_system();
    } else if (strcmp(cmd, "exit") == 0) {
        shutdown_system();
    } else if (strcmp(cmd, "meminfo") == 0) {
        meminfo();
    } else if (strcmp(cmd, "ls") == 0) {
        fs_list_files();
    } else if (strcmp(cmd, "create") == 0) {
        cmd_create_file(arg1);
    } else if (strcmp(cmd, "delete") == 0) {
        cmd_delete_file(arg1);
    } else if (strcmp(cmd, "write") == 0) {
        cmd_write_file(arg1, arg2);
    } else if (strcmp(cmd, "read") == 0) {
        cmd_read_file(arg1);
    } else {
        terminal_writestring("Unknown command.\n");
    }
}