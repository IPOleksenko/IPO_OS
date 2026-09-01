#include <kernel/terminal.h>

#include <vga.h>
#include <driver/keyboard.h>
#include <driver/input/keymap/keymap.h>
#include <file_system/ipo_fs.h>
#include <kernel/process.h>
#include <memory/kmalloc.h>
#include <system/timer.h>
#include <kernel/async.h>
#include <system/state.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* Terminal input locking */
static bool terminal_input_locked = false;

void terminal_lock_input(void) {
    terminal_input_locked = true;
    serial_printf("[terminal] input locked\n");
}

void terminal_unlock_input(void) {
    terminal_input_locked = false;
    serial_printf("[terminal] input unlocked\n");
}

bool terminal_is_input_locked(void) {
    return terminal_input_locked;
}

/* Scancodes for navigation */
#define SC_PAGE_UP   0x49
#define SC_PAGE_DOWN 0x51
#define SC_ARROW_UP  0x48
#define SC_ARROW_DOWN 0x50
#define SC_ARROW_LEFT 0x4B
#define SC_ARROW_RIGHT 0x4D
#define COMMAND_HISTORY_SIZE 128
#define COMMAND_HISTORY_ENTRY_SIZE 1024
#define terminal_history_PATH "/terminal_history"

/* Prompt / styling */
#define PROMPT_STR "> "
#define PROMPT_LEN 2
#define PROMPT_FG VGA_COLOR_LIGHT_GREEN
#define INPUT_FG VGA_COLOR_LIGHT_GREY

/* Input buffer for simple command handling: no hard maximum length. */
static char *input_buf = NULL;
static size_t input_capacity = 0;
static size_t input_len = 0;
static bool prompt_shown = false;
static uint16_t input_start_cursor = 0;

static char **command_history = NULL;
static int command_history_count = 0;
static int command_history_index = -1;
static int command_history_scroll_offset = 0;
static char *current_input_snapshot = NULL;
static uint8_t last_terminal_scancode = 0;
static uint32_t last_terminal_key_ms = 0;
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

static void ensure_input_line_visible(const char *text) {
    if (!text) {
        return;
    }

    size_t text_len = strlen(text);
    uint16_t prompt_row = input_start_cursor / VGA_WIDTH;
    uint16_t prompt_col = input_start_cursor % VGA_WIDTH;
    size_t cols_used = text_len + prompt_col;
    uint16_t max_row = terminal_top_row() + terminal_rows() - 1u;
    uint16_t end_row = prompt_row + (uint16_t)((cols_used + VGA_WIDTH - 1u) / VGA_WIDTH);

    int scrolls = 0;
    while (end_row > max_row && scrolls < (int)terminal_rows()) {
        terminal_auto_scroll();
        prompt_row = input_start_cursor / VGA_WIDTH;
        prompt_col = input_start_cursor % VGA_WIDTH;
        cols_used = text_len + prompt_col;
        max_row = terminal_top_row() + terminal_rows() - 1u;
        end_row = prompt_row + (uint16_t)((cols_used + VGA_WIDTH - 1u) / VGA_WIDTH);
        scrolls++;
    }
}

static void render_input_line(const char *text) {
    if (!text) {
        text = "";
    }

    ensure_input_line_visible(text);

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

static void ensure_input_buffer(size_t needed) {
    if (input_buf == NULL || input_capacity < needed + 1u) {
        size_t new_capacity = input_capacity ? input_capacity : 256u;
        while (new_capacity < needed + 1u) {
            new_capacity *= 2u;
        }
        char *new_buf = kmalloc(new_capacity);
        if (new_buf == NULL) {
            return;
        }
        if (input_buf != NULL) {
            memcpy(new_buf, input_buf, input_len + 1u);
            kfree(input_buf);
        }
        input_buf = new_buf;
        input_capacity = new_capacity;
    }
}

static void ensure_history_storage(void) {
    if (command_history != NULL) {
        return;
    }

    command_history = kmalloc(COMMAND_HISTORY_SIZE * sizeof(char *));
    if (command_history == NULL) {
        return;
    }

    for (int i = 0; i < COMMAND_HISTORY_SIZE; i++) {
        command_history[i] = kmalloc(COMMAND_HISTORY_ENTRY_SIZE);
        if (command_history[i] == NULL) {
            command_history[i] = NULL;
        } else {
            command_history[i][0] = '\0';
        }
    }
}

static void load_command_history_from_file(void) {
    ensure_history_storage();
    for (int i = 0; i < COMMAND_HISTORY_SIZE; i++) {
        if (command_history[i] != NULL) {
            command_history[i][0] = '\0';
        }
    }
    command_history_count = 0;
    command_history_index = -1;
}

static void push_command_history(const char *cmd) {
    if (!cmd || !cmd[0]) {
        return;
    }

    if (strlen(cmd) >= COMMAND_HISTORY_ENTRY_SIZE) {
        return;
    }

    if (command_history_count < COMMAND_HISTORY_SIZE) {
        if (command_history[command_history_count] == NULL) {
            command_history[command_history_count] = kmalloc(COMMAND_HISTORY_ENTRY_SIZE);
        }
        if (command_history[command_history_count] != NULL) {
            strncpy(command_history[command_history_count], cmd, COMMAND_HISTORY_ENTRY_SIZE - 1u);
            command_history[command_history_count][COMMAND_HISTORY_ENTRY_SIZE - 1u] = '\0';
            command_history_count++;
        }
    } else {
        for (int i = 1; i < COMMAND_HISTORY_SIZE; i++) {
            if (command_history[i] != NULL) {
                strncpy(command_history[i - 1], command_history[i], COMMAND_HISTORY_ENTRY_SIZE - 1u);
                command_history[i - 1][COMMAND_HISTORY_ENTRY_SIZE - 1u] = '\0';
            }
        }
        if (command_history[COMMAND_HISTORY_SIZE - 1] != NULL) {
            strncpy(command_history[COMMAND_HISTORY_SIZE - 1], cmd, COMMAND_HISTORY_ENTRY_SIZE - 1u);
            command_history[COMMAND_HISTORY_SIZE - 1][COMMAND_HISTORY_ENTRY_SIZE - 1u] = '\0';
        }
    }

    command_history_index = -1;
}

static void restore_snapshot_input(void);

static void load_history_command(int index) {
    if (index < 0 || index >= command_history_count) {
        return;
    }

    ensure_input_buffer(strlen(command_history[index]));
    strcpy(input_buf, command_history[index]);
    input_len = strlen(input_buf);
    render_input_line(input_buf);
}

static void apply_history_position(void) {
    if (command_history_index < 0) {
        restore_snapshot_input();
        return;
    }

    load_history_command(command_history_index);
    vga_set_cursor(input_start_cursor + (uint16_t)input_len);
}

static void restore_snapshot_input(void) {
    if (current_input_snapshot == NULL) {
        input_len = 0;
        if (input_buf != NULL) {
            input_buf[0] = '\0';
        }
        return;
    }
    ensure_input_buffer(strlen(current_input_snapshot));
    strcpy(input_buf, current_input_snapshot);
    input_len = strlen(input_buf);
    render_input_line(input_buf);
    vga_set_cursor(input_start_cursor + (uint16_t)input_len);
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

    ensure_input_buffer(0);
    if (input_buf != NULL) {
        input_buf[0] = '\0';
    }

    input_start_cursor = 0;

    command_history_count = 0;
    command_history_index = -1;
    command_history_scroll_offset = 0;
    top_buffer_count = 0;
    bottom_buffer_count = 0;

    if (current_input_snapshot == NULL) {
        current_input_snapshot = kmalloc(256u);
    }
    if (current_input_snapshot != NULL) {
        current_input_snapshot[0] = '\0';
    }

    last_terminal_scancode = 0;
    last_terminal_key_ms = 0;
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

    size_t name_cap = 256u;
    char *name = kmalloc(name_cap);
    if (name == NULL) {
        return -1;
    }
    size_t i = 0u;
    while (*cmdline && *cmdline != ' ' && *cmdline != '\t' && i + 1u < name_cap) {
        name[i++] = *cmdline++;
    }
    name[i] = '\0';

    // Resolve to filesystem path
    char *path = resolve_command_path(name);
    if (!path) return 0; // not found

    // Parse arguments from the remaining part of cmdline without fixed-size args.
    char **argv = NULL;
    int argc = 0;
    int argv_cap = 8;
    argv = kmalloc((size_t)argv_cap * sizeof(char *));
    if (argv == NULL) {
        kfree(name);
        return -1;
    }
    argv[argc++] = name;
    
    // Skip whitespace after command name
    while (*cmdline && (*cmdline == ' ' || *cmdline == '\t')) cmdline++;
    
    // Parse remaining arguments into a dynamically growing vector.
    char *arg_buf = kmalloc(256u);
    size_t arg_pos = 0u;
    int in_arg = 0;

    while (*cmdline) {
        if (*cmdline == ' ' || *cmdline == '\t') {
            if (in_arg) {
                arg_buf[arg_pos] = '\0';
                char *arg_copy = kmalloc(arg_pos + 1u);
                if (arg_copy) {
                    strcpy(arg_copy, arg_buf);
                    if (argc + 1 >= argv_cap) {
                        int new_cap = argv_cap * 2;
                        char **new_argv = kmalloc((size_t)new_cap * sizeof(char *));
                        if (new_argv == NULL) {
                            kfree(arg_copy);
                            kfree(arg_buf);
                            kfree(argv);
                            kfree(name);
                            return -1;
                        }
                        memcpy(new_argv, argv, (size_t)argc * sizeof(char *));
                        kfree(argv);
                        argv = new_argv;
                        argv_cap = new_cap;
                    }
                    argv[argc++] = arg_copy;
                }
                arg_pos = 0u;
                in_arg = 0;
            }
            cmdline++;
            continue;
        }

        if (arg_pos + 1u >= 256u) {
            char *extended = kmalloc(arg_pos + 256u);
            if (extended == NULL) {
                kfree(arg_buf);
                kfree(argv);
                kfree(name);
                return -1;
            }
            memcpy(extended, arg_buf, arg_pos);
            kfree(arg_buf);
            arg_buf = extended;
        }
        arg_buf[arg_pos++] = *cmdline++;
        in_arg = 1;
    }

    if (in_arg) {
        arg_buf[arg_pos] = '\0';
        char *arg_copy = kmalloc(arg_pos + 1u);
        if (arg_copy) {
            strcpy(arg_copy, arg_buf);
            if (argc + 1 >= argv_cap) {
                int new_cap = argv_cap * 2;
                char **new_argv = kmalloc((size_t)new_cap * sizeof(char *));
                if (new_argv == NULL) {
                    kfree(arg_copy);
                    kfree(arg_buf);
                    kfree(argv);
                    kfree(name);
                    return -1;
                }
                memcpy(new_argv, argv, (size_t)argc * sizeof(char *));
                kfree(argv);
                argv = new_argv;
                argv_cap = new_cap;
            }
            argv[argc++] = arg_copy;
        }
    }

    kfree(arg_buf);
    argv[argc] = NULL;

    // Execute program with arguments
    int result = process_exec(path, argc, argv);

    // Free allocated argument copies
    for (int j = 1; j < argc; j++) {
        kfree(argv[j]);
    }
    kfree(argv);
    kfree(path);
    return result;
}

void terminal_console(void){
    if (terminal_input_locked || system_is_input_state()) {
        return;
    }

    if (process_get_current() == NULL &&
        system_get_state() != SYSTEM_STATE_TERMINAL_IDLE &&
        system_get_state() != SYSTEM_STATE_BOOT) {
        system_set_state(SYSTEM_STATE_TERMINAL_IDLE);
    }

    uint8_t scancode = keyboard_get_scancode();
    update_hot_key_state(scancode);
    hot_key_handler(scancode);

    if (!prompt_shown) print_prompt();

    if (scancode != 0x00) {
        bool is_break_code = (scancode & 0x80) != 0;

        uint32_t now = timer_millis();
        if (!is_break_code && scancode == last_terminal_scancode) {
            if (now - last_terminal_key_ms < 30u) {
                return;
            }
        }

        if (is_break_code) {
            last_terminal_scancode = 0;
        } else {
            last_terminal_scancode = scancode;
            last_terminal_key_ms = now;
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
                            size_t snapshot_len = strlen(input_buf);
                            if (current_input_snapshot == NULL || strlen(current_input_snapshot) < snapshot_len + 1u) {
                                size_t new_cap = snapshot_len + 1u;
                                if (new_cap < 256u) {
                                    new_cap = 256u;
                                }
                                char *new_snapshot = kmalloc(new_cap);
                                if (new_snapshot == NULL) {
                                    return;
                                }
                                if (current_input_snapshot != NULL) {
                                    strcpy(new_snapshot, current_input_snapshot);
                                    kfree(current_input_snapshot);
                                }
                                current_input_snapshot = new_snapshot;
                            }
                            strcpy(current_input_snapshot, input_buf);
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
                        size_t snapshot_len = strlen(input_buf);
                        if (current_input_snapshot == NULL || strlen(current_input_snapshot) < snapshot_len + 1u) {
                            size_t new_cap = snapshot_len + 1u;
                            if (new_cap < 256u) {
                                new_cap = 256u;
                            }
                            char *new_snapshot = kmalloc(new_cap);
                            if (new_snapshot == NULL) {
                                return;
                            }
                            if (current_input_snapshot != NULL) {
                                strcpy(new_snapshot, current_input_snapshot);
                                kfree(current_input_snapshot);
                            }
                            current_input_snapshot = new_snapshot;
                        }
                        strcpy(current_input_snapshot, input_buf);
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

                    size_t free_space = input_capacity - 1u - input_len;
                    if (free_space <= 0u) {
                        ensure_input_buffer(input_len + 4u);
                        free_space = input_capacity - 1u - input_len;
                    }

                    if (spaces > (int)free_space) {
                        spaces = (int)free_space;
                    }

                    for (int i = 0; i < spaces; i++) {
                        putchar(' ');
                        input_buf[input_len++] = ' ';
                    }
                    input_buf[input_len] = '\0';
                }
                /* Printable characters */
                else if (c >= 32 && c < 127) {
                    ensure_input_buffer(input_len + 1u);
                    putchar(c);
                    input_buf[input_len++] = c;
                    input_buf[input_len] = '\0';
                }
            }
        }
    }
}

/* Auto scroll when terminal overflows - keep only current screen in RAM */
void terminal_auto_scroll(void) {
    volatile uint16_t *vga = VGA_MEMORY;

    if (prompt_shown && input_start_cursor >= VGA_WIDTH) {
        input_start_cursor -= VGA_WIDTH;
    }

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