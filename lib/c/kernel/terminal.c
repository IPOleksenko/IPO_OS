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

/* Terminal Buffers - each can store multiple lines of history */
#define SCROLL_HISTORY_SIZE 1024  // Number of lines to store in history
static uint16_t terminal_top_buffer[SCROLL_HISTORY_SIZE][VGA_WIDTH];
static uint16_t terminal_bottom_buffer[SCROLL_HISTORY_SIZE][VGA_WIDTH];

/* Input buffer for simple command handling */
#define INPUT_BUF_SIZE 256

static char input_buf[INPUT_BUF_SIZE];
static int input_len = 0;
static bool prompt_shown = false;

/* Scroll state */
static int top_buffer_count = 0;      // How many lines are stored in terminal_top_buffer
static int bottom_buffer_count = 0;   // How many lines are stored in terminal_bottom_buffer

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

/* Read a line from VGA screen */
static void read_line_from_vga(uint16_t row, uint16_t *buffer) {
    volatile uint16_t* vga = VGA_MEMORY;
    uint16_t offset = row * VGA_WIDTH;
    
    for (uint16_t col = 0; col < VGA_WIDTH; col++) {
        buffer[col] = vga[offset + col];  // Save full value with colors
    }
}

/* Write a line to VGA screen with preserved colors */
static void write_line_to_vga(uint16_t row, const uint16_t *buffer) {
    volatile uint16_t* vga = VGA_MEMORY;
    uint16_t offset = row * VGA_WIDTH;
    
    for (uint16_t col = 0; col < VGA_WIDTH; col++) {
        vga[offset + col] = buffer[col];  // Restore full value with colors
    }
}

/* Auto scroll when terminal overflows - called by putchar/printf when needed */
void terminal_auto_scroll(void) {
    volatile uint16_t* vga = VGA_MEMORY;
    uint16_t top = terminal_top_row();
    uint16_t rows = terminal_rows();
    
    // Save the TOP line (which will disappear) to top_buffer history before shifting
    if (top_buffer_count < SCROLL_HISTORY_SIZE) {
        read_line_from_vga(top, terminal_top_buffer[top_buffer_count]);
        top_buffer_count++;
     } else {
        // Buffer is full, shift all lines and add new at end
        for (int i = 0; i < SCROLL_HISTORY_SIZE - 1; i++) {
            for (uint16_t c = 0; c < VGA_WIDTH; c++) {
                terminal_top_buffer[i][c] = terminal_top_buffer[i + 1][c];
            }
        }
        read_line_from_vga(top, terminal_top_buffer[SCROLL_HISTORY_SIZE - 1]);
    }
    
    // Reset bottom buffer since we got new output
    bottom_buffer_count = 0;
    
    // Shift lines up by one
    for (uint16_t r = 0; r < rows - 1; r++) {
        uint16_t src_offset = (top + r + 1) * VGA_WIDTH;
        uint16_t dst_offset = (top + r) * VGA_WIDTH;
        
        for (uint16_t c = 0; c < VGA_WIDTH; c++) {
            vga[dst_offset + c] = vga[src_offset + c];
        }
    }
    
    // Clear bottom line
    uint16_t blank = vga_entry(0x00, VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    for (uint16_t c = 0; c < VGA_WIDTH; c++) {
        vga[(top + rows - 1) * VGA_WIDTH + c] = blank;
    }
}

/* Return to present - restore current output when user starts typing */
static void return_to_present(void) {
    // If we're in history, restore to the present
    if (bottom_buffer_count == 0) {
        return;  // Already at present
    }
    
    volatile uint16_t* vga = VGA_MEMORY;
    uint16_t top = terminal_top_row();
    uint16_t rows = terminal_rows();
    
    // Restore all lines from bottom_buffer by scrolling down (reverse of scroll up)
    while (bottom_buffer_count > 0) {
        // Save the current top line to top_buffer
        read_line_from_vga(top, terminal_top_buffer[top_buffer_count]);
        top_buffer_count++;
        
        // Shift lines up by one (move forward in time)
        for (uint16_t r = 0; r < rows - 1; r++) {
            uint16_t src_offset = (top + r + 1) * VGA_WIDTH;
            uint16_t dst_offset = (top + r) * VGA_WIDTH;
            
            for (uint16_t c = 0; c < VGA_WIDTH; c++) {
                vga[dst_offset + c] = vga[src_offset + c];
            }
        }
        
        // Restore line from bottom_buffer at the bottom (move toward present)
        write_line_to_vga(top + rows - 1, terminal_bottom_buffer[bottom_buffer_count - 1]);
        bottom_buffer_count--;
    }
    
    // Show cursor when returning to present
    vga_show_cursor();
}

/* Scroll down - restore next line from history if available */
static void scroll_down(void) {
    // Can only scroll down if we're in scroll history (in the past)
    if (bottom_buffer_count == 0) {
        return;  // Can't scroll further, we're at the current output
    }
    
    volatile uint16_t* vga = VGA_MEMORY;
    uint16_t top = terminal_top_row();
    uint16_t rows = terminal_rows();
    
    // Save the current top line to top_buffer
    read_line_from_vga(top, terminal_top_buffer[top_buffer_count]);
    top_buffer_count++;
    
    // Shift lines up by one (move forward in time)
    for (uint16_t r = 0; r < rows - 1; r++) {
        uint16_t src_offset = (top + r + 1) * VGA_WIDTH;
        uint16_t dst_offset = (top + r) * VGA_WIDTH;
        
        for (uint16_t c = 0; c < VGA_WIDTH; c++) {
            vga[dst_offset + c] = vga[src_offset + c];
        }
    }
    
    // Restore line from bottom_buffer at the bottom (move toward present)
    write_line_to_vga(top + rows - 1, terminal_bottom_buffer[bottom_buffer_count - 1]);
    bottom_buffer_count--;
    
    // Show cursor when we return to present (bottom_buffer is now empty)
    if (bottom_buffer_count == 0) {
        vga_show_cursor();
    }
}

/* Scroll up - show previous line from history */
static void scroll_up(void) {
    // Only scroll up if we have history to go back to
    if (top_buffer_count == 0) {
        return;  // Nothing to scroll back to
    }
    
    volatile uint16_t* vga = VGA_MEMORY;
    uint16_t top = terminal_top_row();
    uint16_t rows = terminal_rows();
    
    // Hide cursor when entering history
    vga_hide_cursor();
    
    // Save the current bottom line to bottom_buffer before shifting
    if (bottom_buffer_count < SCROLL_HISTORY_SIZE) {
        read_line_from_vga(top + rows - 1, terminal_bottom_buffer[bottom_buffer_count]);
        bottom_buffer_count++;
    }
    
    // Shift lines down by one
    for (uint16_t r = rows - 1; r > 0; r--) {
        uint16_t src_offset = (top + r - 1) * VGA_WIDTH;
        uint16_t dst_offset = (top + r) * VGA_WIDTH;
        
        for (uint16_t c = 0; c < VGA_WIDTH; c++) {
            vga[dst_offset + c] = vga[src_offset + c];
        }
    }
    
    // Restore line from top_buffer at the top (go back in history)
    write_line_to_vga(top, terminal_top_buffer[top_buffer_count - 1]);
    top_buffer_count--;
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

    terminal_top_buffer[0][0] = 0;
    terminal_bottom_buffer[0][0] = 0;
    top_buffer_count = 0;
    bottom_buffer_count = 0;
    input_len = 0;
    prompt_shown = false;
}

/* Print the command prompt */
static void print_prompt(void) {
    for (int i = 0; i < PROMPT_LEN; i++) putchar_color(PROMPT_STR[i], PROMPT_FG, VGA_COLOR_BLACK);
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

    // Parse arguments from the remaining part of cmdline
    char *argv[32];  // Support up to 32 arguments
    int argc = 0;
    argv[argc++] = name;  // First argument is program name
    
    // Skip whitespace after command name
    while (*cmdline && (*cmdline == ' ' || *cmdline == '\t')) cmdline++;
    
    // Parse remaining arguments
    char arg_buf[512];  // Temporary buffer for arguments
    int arg_pos = 0;
    int in_arg = 0;
    
    while (*cmdline && argc < 31) {  // Leave room for NULL terminator
        if (*cmdline == ' ' || *cmdline == '\t') {
            if (in_arg) {
                // End current argument
                arg_buf[arg_pos] = '\0';
                char *arg_copy = kmalloc(arg_pos + 1);
                if (arg_copy) {
                    strcpy(arg_copy, arg_buf);
                    argv[argc++] = arg_copy;
                }
                arg_pos = 0;
                in_arg = 0;
            }
            cmdline++;
        } else {
            // Add character to current argument
            if (arg_pos < (int)sizeof(arg_buf) - 1) {
                arg_buf[arg_pos++] = *cmdline;
                in_arg = 1;
            }
            cmdline++;
        }
    }
    
    // Handle last argument
    if (in_arg) {
        arg_buf[arg_pos] = '\0';
        char *arg_copy = kmalloc(arg_pos + 1);
        if (arg_copy) {
            strcpy(arg_copy, arg_buf);
            argv[argc++] = arg_copy;
        }
    }
    
    argv[argc] = NULL;  // NULL-terminate argv

    // Execute program with arguments
    int result = process_exec(path, argc, argv);

    // Free allocated argument copies
    for (int j = 1; j < argc; j++) {
        kfree(argv[j]);
    }
    
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

        /* Handle scroll navigation - no break code check needed */
        if (!is_break_code) {
            if (scancode == SC_PAGE_DOWN || scancode == SC_ARROW_DOWN) {
                scroll_down();
                return;
            }
            if (scancode == SC_PAGE_UP || scancode == SC_ARROW_UP) {
                scroll_up();
                return;
            }
        }

        if (!is_break_code) {
            char c = get_char(scancode);
            if (c != 0x00) {
                // Return to present when user starts typing
                return_to_present();
                
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
                        } else {
                            int ret = process_get_exit_code();
                            printf("Return value: %d\n", ret);
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
                else if (c == '\t') {
                    int tab_size = 4;
                    int col = vga_get_cursor_position() % VGA_WIDTH;
                    int spaces = tab_size - (col % tab_size);

                    int free_space = INPUT_BUF_SIZE - 1 - input_len;
                    if (free_space <= 0) return;

                    if (spaces > free_space) {
                        spaces = free_space;
                    }

                    for (int i = 0; i < spaces; i++) {
                        putchar(' ');
                        input_buf[input_len++] = ' ';
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