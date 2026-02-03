#include <kernel/terminal.h>

#include <vga.h>
#include <driver/keyboard.h>
#include <driver/input/keymap/keymap.h>
#include <file_system/ipo_fs.h>
#include <kernel/process.h>
#include <memory/kmalloc.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* Scancodes for navigation */
#define SC_PAGE_UP   0x49
#define SC_PAGE_DOWN 0x51
#define SC_ARROW_UP  0x48
#define SC_ARROW_DOWN 0x50

/* Prompt / styling */
#define PROMPT_STR "> "
#define PROMPT_LEN 2
#define PROMPT_FG VGA_COLOR_LIGHT_GREEN
#define INPUT_FG VGA_COLOR_LIGHT_GREY

// Terminal drawing area (below header)
static inline uint16_t terminal_top_row(void) {
    return VGA_START_CURSOR_POSITION / VGA_WIDTH;
}

static inline uint16_t terminal_rows(void) {
    return VGA_HEIGHT - terminal_top_row();
}

static void clear_terminal_area(void) {
    volatile uint16_t* vga = VGA_MEMORY;
    uint16_t top = terminal_top_row();
    uint16_t rows = terminal_rows();
    uint16_t blank = vga_entry(0x00, VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    for (uint16_t r = 0; r < rows; r++) {
        uint16_t offset = (top + r) * VGA_WIDTH;
        for (uint16_t c = 0; c < VGA_WIDTH; c++) {
            vga[offset + c] = blank;
        }
    }
}

void print_header(void) {
    volatile uint16_t* vga = VGA_MEMORY;

    char os_name[] = "IPO_OS";
    char created_by[] = "Created by IPOleksenko";

    uint8_t os_name_length = sizeof(os_name) - 1;
    uint8_t created_by_length = sizeof(created_by) - 1;

    for (uint8_t i = 0; i < os_name_length; i++) {
        vga[i] = vga_entry(os_name[i], VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    }

    for (uint8_t i = 0; i < created_by_length; i++) {
        vga[VGA_WIDTH - created_by_length + i] = vga_entry(created_by[i], VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    }
}

char* resolve_command_path(const char *cmd) {
    if (!cmd || !cmd[0]) return NULL;
    
    char *path = kmalloc(256);
    if (!path) return NULL;
    
    char to_check[256];
    char canonical[256];
    uint32_t inode;
    struct ipo_inode stat;
    
    // Determine how to interpret the command
    if (cmd[0] == '/') {
        // Absolute path - use as-is
        strncpy(to_check, cmd, sizeof(to_check) - 1);
        to_check[sizeof(to_check) - 1] = '\0';
    } 
    else if (cmd[0] == '.' || strchr(cmd, '/')) {
        // Path-like (starts with . or contains /) - resolve from root
        snprintf(to_check, sizeof(to_check), "/%s", cmd);
    }
    else {
        // Simple name - look in /app/
        snprintf(to_check, sizeof(to_check), "/app/%s", cmd);
    }
    
    // Canonicalize to handle .., ., //, etc
    fs_canonicalize(to_check, canonical, sizeof(canonical));
    
    // Try to resolve and verify it's a file (not directory)
    if (path_resolve(canonical, &inode) == 0 && 
        ipo_fs_stat(canonical, &stat) && 
        (stat.mode & IPO_INODE_TYPE_DIR) == 0) {
        strncpy(path, canonical, 255);
        path[255] = '\0';
        return path;
    }
    
    kfree(path);
    return NULL;
}

void terminal_initialize(void) {
    vga_clear(VGA_COLOR_WHITE, VGA_COLOR_BLACK, true, VGA_START_CURSOR_POSITION);

    print_header();
}

/* Input buffer for simple command handling */
#define INPUT_BUF_SIZE 256

static char input_buf[INPUT_BUF_SIZE];
static int input_len = 0;
static bool prompt_shown = false;

/* Print the command prompt */
static void print_prompt(void) {
    for (int i = 0; i < PROMPT_LEN; i++) putchar(PROMPT_STR[i]);
    prompt_shown = true;
}

int try_execute_command(const char *cmdline) {
    if (!cmdline) return -1;

    // Extract command name (first token)
    while (*cmdline == ' ' || *cmdline == '\t') cmdline++;
    if (*cmdline == '\0') return 0;

    char name[128];
    int i = 0;
    while (*cmdline && *cmdline != ' ' && *cmdline != '\t' && i < (int)sizeof(name)-1) {
        name[i++] = *cmdline++;
    }
    name[i] = '\0';

    // Resolve to filesystem path
    char *path = resolve_command_path(name);
    if (!path) return 0; // not found

    // Execute program
    int result = process_exec_simple(path);

    kfree(path);
    return result;
}

void terminal_console(void){
    uint8_t scancode = keyboard_get_scancode();
    update_hot_key_state(scancode);
    hot_key_handler(scancode);

    if (!prompt_shown) print_prompt();

    if (scancode != 0x00) {
        update_hot_key_state(scancode);
        hot_key_handler(scancode);

        bool is_break_code = (scancode & 0x80) != 0;

        if (!is_break_code) {
            char c = get_char(scancode);
            if (c != 0x00) {
                /* Handle newline / carriage return */
                if (c == '\n' || c == '\r') {
                    putchar('\n');

                    input_buf[input_len] = '\0';

                    if (input_len > 0) {
                        int exec = try_execute_command(input_buf);
                        if (exec == 0) {
                            printf("Command not found: %s\n", input_buf);
                        } else if (exec < 0) {
                            printf("Execution failed (error %d): %s\n", exec, input_buf);
                        }
                    }

                    /* Reset buffer and show prompt */
                    input_len = 0;
                    prompt_shown = false;
                    print_prompt();
                }
                /* Handle backspace */
                else if (c == '\b' || c == 127) {
                    if (input_len > 0) {
                        /* Move cursor back, overwrite with space, move back again */
                        uint16_t cur = vga_get_cursor_position();
                        if (cur > 0) {
                            vga_set_cursor(cur - 1);
                            putchar(' ');
                            vga_set_cursor(cur - 1);
                        }
                        input_len--;
                    }
                }
                /* Printable characters */
                else if (c >= 32 && c < 127) {
                    if (input_len < INPUT_BUF_SIZE - 1) {
                        putchar(c);
                        input_buf[input_len++] = c;
                    }
                }
            }
        }
    }
}