#include <kernel/terminal.h>

#include <vga.h>
#include <driver/keyboard.h>
#include <driver/input/keymap/keymap.h>
#include <file_system/ipo_fs.h>
#include <kernel/process.h>
#include <memory/kmalloc.h>
#include <system/timer.h>
#include <kernel/async.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* Scancodes for navigation */
#define SC_PAGE_UP   0x49
#define SC_PAGE_DOWN 0x51
#define SC_ARROW_UP  0x48
#define SC_ARROW_DOWN 0x50
#define SC_ARROW_LEFT 0x4B
#define SC_ARROW_RIGHT 0x4D
#define COMMAND_HISTORY_SIZE 128
#define terminal_history_PATH "/terminal_history"

/* Prompt / styling */
#define PROMPT_STR "> "
#define PROMPT_LEN 2
#define PROMPT_FG VGA_COLOR_LIGHT_GREEN
#define INPUT_FG VGA_COLOR_LIGHT_GREY

/* Input buffer for simple command handling */
#define INPUT_BUF_SIZE 256

static char input_buf[INPUT_BUF_SIZE];
static int input_len = 0;
static bool prompt_shown = false;
static uint16_t input_start_cursor = 0;

static char command_history[COMMAND_HISTORY_SIZE][INPUT_BUF_SIZE];
static int command_history_count = 0;
static int command_history_index = -1;
static int command_history_scroll_offset = 0;
static char current_input_snapshot[INPUT_BUF_SIZE];
static uint8_t last_terminal_scancode = 0;
static uint32_t last_history_action_ms = 0;

#define SCROLL_HISTORY_SIZE 1024
#define SCROLL_HISTORY_CLEAR_THRESHOLD (SCROLL_HISTORY_SIZE * 3 / 4)
#define SCROLL_HISTORY_KEEP_SIZE (SCROLL_HISTORY_SIZE / 2)
static uint16_t terminal_top_buffer[SCROLL_HISTORY_SIZE][VGA_WIDTH];
static uint16_t terminal_bottom_buffer[SCROLL_HISTORY_SIZE][VGA_WIDTH];

/* Scroll state */
static int top_buffer_count = 0;
static int bottom_buffer_count = 0;

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

static void render_input_line(const char *text) {
    if (!text) {
        text = "";
    }

    volatile uint16_t *vga = VGA_MEMORY;
    uint16_t row = input_start_cursor / VGA_WIDTH;
    uint16_t col = input_start_cursor % VGA_WIDTH;
    uint16_t blank = vga_entry(0x00, VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    while (row < VGA_HEIGHT) {
        uint16_t offset = row * VGA_WIDTH + col;
        if (offset >= VGA_WIDTH * VGA_HEIGHT) {
            break;
        }
        vga[offset] = blank;
        col++;
        if (col >= VGA_WIDTH) {
            row++;
            col = 0;
        }
    }

    row = input_start_cursor / VGA_WIDTH;
    col = input_start_cursor % VGA_WIDTH;
    const unsigned char *p = (const unsigned char *)text;
    while (*p && row < VGA_HEIGHT) {
        uint16_t offset = row * VGA_WIDTH + col;
        if (offset >= VGA_WIDTH * VGA_HEIGHT) {
            break;
        }
        vga[offset] = vga_entry(*p, INPUT_FG, VGA_COLOR_BLACK);
        p++;
        col++;
        if (col >= VGA_WIDTH) {
            row++;
            col = 0;
        }
    }

    uint16_t end_offset = row * VGA_WIDTH + col;
    vga_set_cursor(end_offset);
}

static void load_command_history_from_file(void) {
    for (int i = 0; i < COMMAND_HISTORY_SIZE; i++) {
        command_history[i][0] = '\0';
    }
    command_history_count = 0;

    if (!fs_mounted) {
        return;
    }

    struct ipo_inode inode;
    if (!ipo_fs_stat(terminal_history_PATH, &inode)) {
        return;
    }

    if (inode.size == 0) {
        return;
    }

    uint32_t size = inode.size;
    if (size > 65535u) {
        size = 65535u;
    }

    char *buffer = kmalloc((size + 1) * sizeof(char));
    if (!buffer) {
        return;
    }

    int fd = ipo_fs_open(terminal_history_PATH);
    if (fd < 0) {
        kfree(buffer);
        return;
    }

    int bytes_read = ipo_fs_read(fd, buffer, size, 0);
    ipo_fs_close(fd);

    if (bytes_read <= 0) {
        kfree(buffer);
        return;
    }

    buffer[bytes_read] = '\0';

    char *line = buffer;
    char *cursor = buffer;
    char *end = buffer + bytes_read;

    while (cursor <= end && command_history_count < COMMAND_HISTORY_SIZE) {
        if (cursor == end || *cursor == '\n') {
            bool end_of_file = (cursor == end);
            if (cursor != end) {
                *cursor = '\0';
            }

            if (strncmp(line, "CMD:", 4) == 0) {
                char *cmd = line + 4;
                while (*cmd == ' ' || *cmd == '\t') {
                    cmd++;
                }

                char *trim_end = cmd + strlen(cmd);
                while (trim_end > cmd && (trim_end[-1] == ' ' || trim_end[-1] == '\t' || trim_end[-1] == '\r' || trim_end[-1] == '\n')) {
                    trim_end--;
                }
                *trim_end = '\0';

                if (*cmd != '\0') {
                    strncpy(command_history[command_history_count], cmd, INPUT_BUF_SIZE - 1);
                    command_history[command_history_count][INPUT_BUF_SIZE - 1] = '\0';
                    command_history_count++;
                }
            }

            if (end_of_file) {
                break;
            }

            line = cursor + 1;
        }
        cursor++;
    }

    kfree(buffer);
}

static void push_command_history(const char *cmd) {
    if (!cmd || !cmd[0]) {
        return;
    }

    char record[INPUT_BUF_SIZE + 8];
    int len = snprintf(record, sizeof(record), "CMD:%s\n", cmd);
    if (len <= 0) {
        return;
    }

    if (fs_mounted) {
        ipo_fs_write_text(terminal_history_PATH, record, true);
    }

    load_command_history_from_file();
    command_history_index = -1;
}

static void restore_snapshot_input(void);

static void load_history_command(int index) {
    if (index < 0 || index >= command_history_count) {
        return;
    }

    strncpy(input_buf, command_history[index], INPUT_BUF_SIZE - 1);
    input_buf[INPUT_BUF_SIZE - 1] = '\0';
    input_len = (int)strlen(input_buf);
    render_input_line(input_buf);
}

static void apply_history_position(void) {
    if (command_history_index < 0) {
        restore_snapshot_input();
        return;
    }

    load_history_command(command_history_index);
}

static void restore_snapshot_input(void) {
    strncpy(input_buf, current_input_snapshot, INPUT_BUF_SIZE - 1);
    input_buf[INPUT_BUF_SIZE - 1] = '\0';
    input_len = (int)strlen(input_buf);
    render_input_line(input_buf);
}

static void trim_top_buffer(void) {
    if (top_buffer_count < SCROLL_HISTORY_CLEAR_THRESHOLD) {
        return;
    }

    int remove_count = top_buffer_count - SCROLL_HISTORY_KEEP_SIZE;
    if (remove_count <= 0) {
        return;
    }

    for (int i = 0; i < SCROLL_HISTORY_KEEP_SIZE; i++) {
        memcpy(
            terminal_top_buffer[i],
            terminal_top_buffer[i + remove_count],
            VGA_WIDTH * sizeof(uint16_t)
        );
    }

    top_buffer_count = SCROLL_HISTORY_KEEP_SIZE;
}

static void save_top_line(uint16_t row) {
    if (top_buffer_count >= SCROLL_HISTORY_SIZE) {
        trim_top_buffer();
    }

    if (top_buffer_count < SCROLL_HISTORY_SIZE) {
        read_line_from_vga(row, terminal_top_buffer[top_buffer_count]);
        top_buffer_count++;
    }
}

/* Return to present - restore current output when user starts typing */
static void return_to_present(void) {
    if (bottom_buffer_count == 0) {
        return;
    }

    volatile uint16_t *vga = VGA_MEMORY;

    uint16_t top = terminal_top_row();
    uint16_t rows = terminal_rows();

    if (rows <= 1) {
        bottom_buffer_count = 0;
        vga_show_cursor();
        return;
    }

    while (bottom_buffer_count > 0) {
        if (top_buffer_count < SCROLL_HISTORY_SIZE) {
            read_line_from_vga(top, terminal_top_buffer[top_buffer_count]);
            top_buffer_count++;
        }

        for (uint16_t r = 0; r < rows - 1; r++) {
            uint16_t src_offset = (top + r + 1) * VGA_WIDTH;
            uint16_t dst_offset = (top + r) * VGA_WIDTH;

            for (uint16_t c = 0; c < VGA_WIDTH; c++) {
                vga[dst_offset + c] = vga[src_offset + c];
            }
        }

        write_line_to_vga(top + rows - 1, terminal_bottom_buffer[bottom_buffer_count - 1]);
        bottom_buffer_count--;
    }

    vga_show_cursor();
}

/* Scroll down - restore next line from history if available */
static void scroll_down(void) {
    if (bottom_buffer_count == 0) {
        return;
    }

    volatile uint16_t *vga = VGA_MEMORY;

    uint16_t top = terminal_top_row();
    uint16_t rows = terminal_rows();

    if (rows <= 1) {
        return;
    }

    if (top_buffer_count < SCROLL_HISTORY_SIZE) {
        read_line_from_vga(top, terminal_top_buffer[top_buffer_count]);
        top_buffer_count++;
    }

    for (uint16_t r = 0; r < rows - 1; r++) {
        uint16_t src_offset = (top + r + 1) * VGA_WIDTH;
        uint16_t dst_offset = (top + r) * VGA_WIDTH;

        for (uint16_t c = 0; c < VGA_WIDTH; c++) {
            vga[dst_offset + c] = vga[src_offset + c];
        }
    }

    write_line_to_vga(top + rows - 1, terminal_bottom_buffer[bottom_buffer_count - 1]);
    bottom_buffer_count--;

    if (bottom_buffer_count == 0) {
        vga_show_cursor();
    }
}

/* Scroll up - show previous line from history */
static void scroll_up(void) {
    if (top_buffer_count == 0) {
        return;
    }

    volatile uint16_t *vga = VGA_MEMORY;

    uint16_t top = terminal_top_row();
    uint16_t rows = terminal_rows();

    if (rows <= 1) {
        return;
    }

    vga_hide_cursor();

    if (bottom_buffer_count < SCROLL_HISTORY_SIZE) {
        read_line_from_vga(top + rows - 1, terminal_bottom_buffer[bottom_buffer_count]);
        bottom_buffer_count++;
    }

    for (uint16_t r = rows - 1; r > 0; r--) {
        uint16_t src_offset = (top + r - 1) * VGA_WIDTH;
        uint16_t dst_offset = (top + r) * VGA_WIDTH;

        for (uint16_t c = 0; c < VGA_WIDTH; c++) {
            vga[dst_offset + c] = vga[src_offset + c];
        }
    }

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
    vga_clear(
        VGA_COLOR_WHITE,
        VGA_COLOR_BLACK,
        true,
        VGA_START_CURSOR_POSITION
    );

    print_header();

    input_len = 0;
    prompt_shown = false;

    input_start_cursor = 0;

    command_history_count = 0;
    command_history_index = -1;
    command_history_scroll_offset = 0;
    top_buffer_count = 0;
    bottom_buffer_count = 0;

    current_input_snapshot[0] = '\0';

    last_terminal_scancode = 0;
    last_history_action_ms = 0;

    load_command_history_from_file();
}

/* Print the command prompt */
static void print_prompt(void) {
    for (int i = 0; i < PROMPT_LEN; i++) putchar_color(PROMPT_STR[i], PROMPT_FG, VGA_COLOR_BLACK);
    prompt_shown = true;
    input_start_cursor = vga_get_cursor_position();
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
        bool is_break_code = (scancode & 0x80) != 0;

        if (!is_break_code && scancode == last_terminal_scancode) {
            return;
        }

        if (is_break_code) {
            last_terminal_scancode = 0;
        } else {
            last_terminal_scancode = scancode;
        }

        update_hot_key_state(scancode);
        hot_key_handler(scancode);

        /* Screen scrolling uses Up/Down arrows; history navigation stays on PageUp/PageDown. */
        if (!is_break_code) {
            if (scancode == SC_ARROW_UP) {
                scroll_up();
                return;
            }
            if (scancode == SC_ARROW_DOWN) {
                scroll_down();
                return;
            }
            if (scancode == SC_PAGE_UP || scancode == SC_PAGE_DOWN) {
                if (scancode == SC_PAGE_UP) {
                    if (command_history_count > 0) {
                        uint32_t now = timer_millis();
                        if (now - last_history_action_ms < 150u) {
                            return;
                        }
                        last_history_action_ms = now;

                        return_to_present();

                        if (command_history_index < 0) {
                            strncpy(current_input_snapshot, input_buf, INPUT_BUF_SIZE - 1);
                            current_input_snapshot[INPUT_BUF_SIZE - 1] = '\0';
                            command_history_index = command_history_count - 1;
                            command_history_scroll_offset = 1;
                        } else if (command_history_index > 0) {
                            command_history_index--;
                            command_history_scroll_offset++;
                        }

                        apply_history_position();
                    }
                    return;
                }

                if (command_history_index >= 0) {
                    uint32_t now = timer_millis();
                    if (now - last_history_action_ms < 150u) {
                        return;
                    }
                    last_history_action_ms = now;

                    return_to_present();

                    if (command_history_index < command_history_count - 1) {
                        command_history_index++;
                        command_history_scroll_offset--;
                        apply_history_position();
                    } else {
                        command_history_index = -1;
                        command_history_scroll_offset = 0;
                        restore_snapshot_input();
                        current_input_snapshot[0] = '\0';
                    }
                }
                return;
            }
            if (scancode == SC_ARROW_LEFT) {
                if (command_history_count > 0) {
                    uint32_t now = timer_millis();
                    if (now - last_history_action_ms < 150u) {
                        return;
                    }
                    last_history_action_ms = now;

                    return_to_present();

                    if (command_history_index < 0) {
                        strncpy(current_input_snapshot, input_buf, INPUT_BUF_SIZE - 1);
                        current_input_snapshot[INPUT_BUF_SIZE - 1] = '\0';
                        command_history_index = command_history_count - 1;
                        command_history_scroll_offset = 1;
                    } else if (command_history_index > 0) {
                        command_history_index--;
                        command_history_scroll_offset++;
                    }

                    apply_history_position();
                }
                return;
            }
            if (scancode == SC_ARROW_RIGHT) {
                if (command_history_index >= 0) {
                    uint32_t now = timer_millis();
                    if (now - last_history_action_ms < 150u) {
                        return;
                    }
                    last_history_action_ms = now;

                    return_to_present();

                    if (command_history_index < command_history_count - 1) {
                        command_history_index++;
                        command_history_scroll_offset--;
                        apply_history_position();
                    } else {
                        command_history_index = -1;
                        command_history_scroll_offset = 0;
                        restore_snapshot_input();
                        current_input_snapshot[0] = '\0';
                    }
                }
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
                        push_command_history(input_buf);
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
                    input_buf[0] = '\0';
                    current_input_snapshot[0] = '\0';
                    command_history_index = -1;
                    command_history_scroll_offset = 0;
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

/* Auto scroll when terminal overflows - keep only current screen in RAM */
void terminal_auto_scroll(void) {
    volatile uint16_t *vga = VGA_MEMORY;

    uint16_t top = terminal_top_row();
    uint16_t rows = terminal_rows();

    if (rows <= 1) {
        return;
    }

    save_top_line(top);
    bottom_buffer_count = 0;

    for (uint16_t r = 0; r < rows - 1; r++) {
        uint16_t src_offset = (top + r + 1) * VGA_WIDTH;
        uint16_t dst_offset = (top + r) * VGA_WIDTH;

        for (uint16_t c = 0; c < VGA_WIDTH; c++) {
            vga[dst_offset + c] = vga[src_offset + c];
        }
    }

    uint16_t blank = vga_entry(0x00, VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    uint16_t bottom_offset = (top + rows - 1) * VGA_WIDTH;

    for (uint16_t c = 0; c < VGA_WIDTH; c++) {
        vga[bottom_offset + c] = blank;
    }
}