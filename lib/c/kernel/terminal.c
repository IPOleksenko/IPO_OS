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
#include <ioport.h>

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
#define SC_HOME      0x47
#define SC_END       0x4F
#define SC_DELETE    0x53
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
static size_t cursor_pos = 0;
static bool prompt_shown = false;
static uint16_t prompt_start_cursor = 0;
static uint16_t input_start_cursor = 0;
static bool terminal_suppress_external_hook = false;

static char **command_history = NULL;
static int command_history_count = 0;
static int command_history_index = -1;
static char *current_input_snapshot = NULL;
static uint8_t last_terminal_scancode = 0;
static uint32_t last_terminal_key_ms = 0;
static uint32_t last_history_action_ms = 0;

static char terminal_cwd[256] = "/";

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

static void make_abs_path(const char *rel_or_abs, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (!rel_or_abs || rel_or_abs[0] == '\0') {
        strncpy(out, terminal_cwd, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    char temp[512];
    if (rel_or_abs[0] == '/') {
        strncpy(temp, rel_or_abs, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';
    } else {
        if (strcmp(terminal_cwd, "/") == 0) {
            snprintf(temp, sizeof(temp), "/%s", rel_or_abs);
        } else {
            snprintf(temp, sizeof(temp), "%s/%s", terminal_cwd, rel_or_abs);
        }
    }
    fs_canonicalize(temp, out, out_size);
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

    if (cursor_pos > input_len) {
        cursor_pos = input_len;
    }

    uint16_t cursor_offset = input_start_cursor + (uint16_t)cursor_pos;
    if (cursor_offset >= VGA_WIDTH * VGA_HEIGHT) {
        cursor_offset = VGA_WIDTH * VGA_HEIGHT - 1;
    }
    vga_set_cursor(cursor_offset);
}

void terminal_on_external_output(void) {
    if (!prompt_shown || terminal_suppress_external_hook) {
        return;
    }

    volatile uint16_t *vga = VGA_MEMORY;
    uint16_t current_cursor = vga_get_cursor_position();
    uint16_t blank = vga_entry(0x00, VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    uint16_t end_clear = current_cursor;
    if (input_start_cursor + (uint16_t)input_len > end_clear) {
        end_clear = input_start_cursor + (uint16_t)input_len;
    }

    for (uint16_t p = prompt_start_cursor; p <= end_clear && p < VGA_WIDTH * VGA_HEIGHT; p++) {
        vga[p] = blank;
    }

    vga_set_cursor(prompt_start_cursor);
    prompt_shown = false;
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
    cursor_pos = input_len;
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
    if (current_input_snapshot == NULL) {
        input_len = 0;
        cursor_pos = 0;
        if (input_buf != NULL) {
            input_buf[0] = '\0';
        }
        render_input_line("");
        return;
    }
    ensure_input_buffer(strlen(current_input_snapshot));
    strcpy(input_buf, current_input_snapshot);
    input_len = strlen(input_buf);
    cursor_pos = input_len;
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
    
    // 1. Absolute path
    if (cmd[0] == '/') {
        strncpy(to_check, cmd, sizeof(to_check) - 1);
        to_check[sizeof(to_check) - 1] = '\0';
    } 
    // 2. Relative path with ./ or ../ or subdirectories
    else if (cmd[0] == '.' || strchr(cmd, '/')) {
        make_abs_path(cmd, to_check, sizeof(to_check));
    }
    // 3. Simple command name: try in cwd first, then in /app/
    else {
        make_abs_path(cmd, to_check, sizeof(to_check));
        fs_canonicalize(to_check, canonical, sizeof(canonical));
        if (path_resolve(canonical, &inode) == 0 && 
            ipo_fs_stat(canonical, &stat) && 
            (stat.mode & IPO_INODE_TYPE_DIR) == 0) {
            strncpy(path, canonical, 255);
            path[255] = '\0';
            return path;
        }

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

static void builtin_help(void) {
    printf("======================== IPO_OS SYSTEM HELP ========================\n");
    printf("1. BUILTIN SHELL COMMANDS & SYNTAX:\n");
    printf("  Navigation & Working Directory:\n");
    printf("    pwd\n");
    printf("      - Print absolute path of current working directory.\n");
    printf("    cd [path]\n");
    printf("      - Change working directory to absolute or relative [path].\n");
    printf("      - Variations: 'cd', 'cd ~' (go to /), 'cd ..' (parent), 'cd .' (current).\n");
    printf("\n");
    printf("  File & Directory Operations:\n");
    printf("    ls [path] | dir [path]\n");
    printf("      - List contents of directory with [DIR]/[FILE] type and size in bytes.\n");
    printf("      - If [path] omitted, lists current working directory.\n");
    printf("    cat <file> | type <file>\n");
    printf("      - Read and display text contents of <file>.\n");
    printf("    touch <file>\n");
    printf("      - Create a new empty file at specified path.\n");
    printf("    mkdir <dir>\n");
    printf("      - Create a new directory at specified path.\n");
    printf("    rm <file|dir> | del <file|dir> | rmdir <file|dir>\n");
    printf("      - Remove a file or an empty directory.\n");
    printf("    cp <src> <dst> | copy <src> <dst>\n");
    printf("      - Copy file from <src> path to <dst> path or destination directory.\n");
    printf("    mv <src> <dst> | move <src> <dst> | rename <src> <dst>\n");
    printf("      - Move or rename file or directory from <src> to <dst>.\n");
    printf("    stat <path>\n");
    printf("      - Display inode details: type, size, protection flags, link count.\n");
    printf("\n");
    printf("  Text Output & File Redirection:\n");
    printf("    echo [text] [> file | >> file]\n");
    printf("      - Output text to screen, or overwrite (>) / append (>>) to a file.\n");
    printf("\n");
    printf("  System & Power Controls:\n");
    printf("    clear | cls\n");
    printf("      - Clear terminal screen and reset VGA scrollback buffer.\n");
    printf("    reboot | restart\n");
    printf("      - Perform hardware system reboot (8042 / port 0x92 / triple fault).\n");
    printf("    shutdown | poweroff | exit | halt\n");
    printf("      - Power off machine via ACPI/APM or halt CPU execution safely.\n");
    printf("    help | ?\n");
    printf("      - Display this system documentation and reference guide.\n");
    printf("\n");
    printf("2. APPLICATION EXECUTION:\n");
    printf("  Syntax: <program> [arguments...]\n");
    printf("  - Resolves executable by searching: current directory, then /app/, then /.\n");
    printf("  - Arguments are parsed and passed as argc/argv to process entrypoint.\n");
    printf("  - Exit code of the completed process is reported upon termination.\n");
    printf("\n");
    printf("3. INTERACTIVE TERMINAL & EDITING KEYBINDINGS:\n");
    printf("  - Left / Right Arrows  : Move cursor inside current command line.\n");
    printf("  - Home / End           : Jump to beginning / end of line.\n");
    printf("  - Backspace            : Delete character before cursor with text shift.\n");
    printf("  - Delete               : Delete character under cursor with text shift.\n");
    printf("  - Tab                  : Insert 4 spaces at cursor.\n");
    printf("  - Up / Down Arrows     : History navigation (previous / next command).\n");
    printf("  - Page Up / Page Down  : Scroll terminal output buffer up / down.\n");
    printf("  - Ctrl + Shift         : Switch active keyboard layout.\n");
    printf("\n");
    printf("4. SYSTEM ARCHITECTURE & FEATURES:\n");
    printf("  - IPO_FS: Hierarchical inode-based file system on ATA IDE storage.\n");
    printf("  - Process Manager: Isolated task heap, multi-tasking and ELF execution.\n");
    printf("  - Async Engine: Cooperative kernel scheduler for background tasks.\n");
    printf("  - Non-blocking I/O: Background task output seamlessly integrates into CLI.\n");
    printf("====================================================================\n");
}

static void builtin_clear(void) {
    vga_clear(
        VGA_COLOR_WHITE,
        VGA_COLOR_BLACK,
        true,
        VGA_START_CURSOR_POSITION
    );
    print_header();
    top_buffer_count = 0;
    bottom_buffer_count = 0;
    input_start_cursor = VGA_START_CURSOR_POSITION;
    prompt_start_cursor = VGA_START_CURSOR_POSITION;
    cursor_pos = 0;
}

static void builtin_pwd(void) {
    printf("%s\n", terminal_cwd);
}

static void builtin_cd(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "~") == 0) {
        strcpy(terminal_cwd, "/");
        return;
    }
    char target[256];
    make_abs_path(argv[1], target, sizeof(target));

    struct ipo_inode stat;
    if (!ipo_fs_stat(target, &stat)) {
        printf("cd: %s: No such directory\n", argv[1]);
        return;
    }
    if ((stat.mode & IPO_INODE_TYPE_DIR) == 0) {
        printf("cd: %s: Not a directory\n", argv[1]);
        return;
    }
    strncpy(terminal_cwd, target, sizeof(terminal_cwd) - 1);
    terminal_cwd[sizeof(terminal_cwd) - 1] = '\0';
}

static void builtin_ls(int argc, char **argv) {
    char target[256];
    if (argc >= 2) {
        make_abs_path(argv[1], target, sizeof(target));
    } else {
        strncpy(target, terminal_cwd, sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
    }

    uint32_t ino;
    if (path_resolve(target, &ino) < 0) {
        printf("ls: cannot access '%s': No such file or directory\n", target);
        return;
    }
    struct ipo_inode din;
    if (!read_inode(ino, &din)) {
        printf("ls: cannot read inode for '%s'\n", target);
        return;
    }
    if ((din.mode & IPO_INODE_TYPE_DIR) == 0) {
        printf("  [FILE] %s  (%u B)\n", target, din.size);
        return;
    }

    uint32_t entries = din.size / sizeof(struct ipo_dir_entry);
    uint8_t buf[IPO_FS_BLOCK_SIZE];

    for (uint32_t e = 0; e < entries; e++) {
        uint32_t block_idx = e / (IPO_FS_BLOCK_SIZE / sizeof(struct ipo_dir_entry));
        uint32_t inblock = e % (IPO_FS_BLOCK_SIZE / sizeof(struct ipo_dir_entry));
        int phys = get_data_block_for_inode(&din, block_idx, false);
        if (phys < 0) continue;
        if (!block_read(phys, buf)) continue;
        struct ipo_dir_entry *de = (struct ipo_dir_entry *)buf + inblock;
        if (de->inode == 0 || de->name_len == 0) continue;

        struct ipo_inode ein;
        uint32_t fsize = 0;
        if (read_inode(de->inode, &ein)) {
            fsize = ein.size;
        }

        if (de->type == IPO_INODE_TYPE_DIR) {
            printf("  [DIR]  %s/\n", de->name);
        } else {
            printf("  [FILE] %s  (%u B)\n", de->name, fsize);
        }
    }
}

static void builtin_cat(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: cat <file>\n");
        return;
    }
    for (int a = 1; a < argc; a++) {
        char target[256];
        make_abs_path(argv[a], target, sizeof(target));

        struct ipo_inode stat;
        if (!ipo_fs_stat(target, &stat)) {
            printf("cat: %s: No such file or directory\n", argv[a]);
            continue;
        }
        if ((stat.mode & IPO_INODE_TYPE_DIR) != 0) {
            printf("cat: %s: Is a directory\n", argv[a]);
            continue;
        }

        int fd = ipo_fs_open(target);
        if (fd < 0) {
            printf("cat: %s: Failed to open\n", argv[a]);
            continue;
        }

        char chunk[256];
        uint32_t offset = 0;
        while (offset < stat.size) {
            uint32_t to_read = sizeof(chunk) - 1;
            if (offset + to_read > stat.size) {
                to_read = stat.size - offset;
            }
            int read_bytes = ipo_fs_read(fd, chunk, to_read, offset);
            if (read_bytes <= 0) break;
            chunk[read_bytes] = '\0';
            for (int i = 0; i < read_bytes; i++) {
                putchar(chunk[i]);
            }
            offset += read_bytes;
        }
        ipo_fs_close(fd);
    }
}

static void builtin_touch(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: touch <file>\n");
        return;
    }
    for (int a = 1; a < argc; a++) {
        char target[256];
        make_abs_path(argv[a], target, sizeof(target));

        struct ipo_inode stat;
        if (ipo_fs_stat(target, &stat)) {
            continue;
        }

        int ino = ipo_fs_create(target, IPO_INODE_TYPE_FILE);
        if (ino < 0) {
            printf("touch: cannot create '%s'\n", argv[a]);
        }
    }
}

static void builtin_mkdir(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: mkdir <directory>\n");
        return;
    }
    for (int a = 1; a < argc; a++) {
        char target[256];
        make_abs_path(argv[a], target, sizeof(target));

        int ino = ipo_fs_create(target, IPO_INODE_TYPE_DIR);
        if (ino < 0) {
            printf("mkdir: cannot create directory '%s'\n", argv[a]);
        }
    }
}

static void builtin_rm(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: rm <path>\n");
        return;
    }
    for (int a = 1; a < argc; a++) {
        char target[256];
        make_abs_path(argv[a], target, sizeof(target));

        struct ipo_inode stat;
        if (!ipo_fs_stat(target, &stat)) {
            printf("rm: cannot remove '%s': No such file or directory\n", argv[a]);
            continue;
        }

        if (!ipo_fs_delete(target)) {
            printf("rm: cannot remove '%s': Protected or non-empty directory\n", argv[a]);
        }
    }
}

static void builtin_cp(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: cp <source> <dest>\n");
        return;
    }
    char src[256];
    char dst[256];
    make_abs_path(argv[1], src, sizeof(src));
    make_abs_path(argv[2], dst, sizeof(dst));

    struct ipo_inode src_stat;
    if (!ipo_fs_stat(src, &src_stat)) {
        printf("cp: cannot stat '%s': No such file\n", argv[1]);
        return;
    }
    if ((src_stat.mode & IPO_INODE_TYPE_DIR) != 0) {
        printf("cp: copying directories is not supported\n");
        return;
    }

    struct ipo_inode dst_stat;
    if (ipo_fs_stat(dst, &dst_stat) && (dst_stat.mode & IPO_INODE_TYPE_DIR) != 0) {
        const char *slash = strrchr(src, '/');
        const char *filename = slash ? (slash + 1) : src;
        char combined[256];
        if (strcmp(dst, "/") == 0) {
            snprintf(combined, sizeof(combined), "/%s", filename);
        } else {
            snprintf(combined, sizeof(combined), "%s/%s", dst, filename);
        }
        fs_canonicalize(combined, dst, sizeof(dst));
    }

    int src_fd = ipo_fs_open(src);
    if (src_fd < 0) {
        printf("cp: cannot open source '%s'\n", argv[1]);
        return;
    }

    if (ipo_fs_stat(dst, &dst_stat)) {
        if (!ipo_fs_delete(dst)) {
            printf("cp: cannot overwrite destination '%s'\n", argv[2]);
            ipo_fs_close(src_fd);
            return;
        }
    }

    int dst_ino = ipo_fs_create(dst, IPO_INODE_TYPE_FILE);
    if (dst_ino < 0) {
        printf("cp: cannot create destination '%s'\n", argv[2]);
        ipo_fs_close(src_fd);
        return;
    }

    int dst_fd = ipo_fs_open(dst);
    if (dst_fd < 0) {
        printf("cp: cannot open destination '%s'\n", argv[2]);
        ipo_fs_close(src_fd);
        return;
    }

    uint8_t chunk[512];
    uint32_t offset = 0;
    while (offset < src_stat.size) {
        uint32_t to_copy = sizeof(chunk);
        if (offset + to_copy > src_stat.size) to_copy = src_stat.size - offset;
        int rb = ipo_fs_read(src_fd, chunk, to_copy, offset);
        if (rb <= 0) break;
        int wb = ipo_fs_write(dst_fd, chunk, rb, offset);
        if (wb != rb) {
            printf("cp: error writing destination '%s'\n", argv[2]);
            break;
        }
        offset += rb;
    }

    ipo_fs_close(src_fd);
    ipo_fs_close(dst_fd);
}

static void builtin_mv(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: mv <source> <dest>\n");
        return;
    }
    char src[256];
    char dst[256];
    make_abs_path(argv[1], src, sizeof(src));
    make_abs_path(argv[2], dst, sizeof(dst));

    if (!ipo_fs_rename(src, dst)) {
        printf("mv: failed to move '%s' to '%s'\n", argv[1], argv[2]);
    }
}

static void builtin_stat(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: stat <path>\n");
        return;
    }
    for (int a = 1; a < argc; a++) {
        char target[256];
        make_abs_path(argv[a], target, sizeof(target));

        uint32_t inode_num = 0;
        if (path_resolve(target, &inode_num) < 0) {
            printf("stat: cannot resolve '%s'\n", argv[a]);
            continue;
        }

        struct ipo_inode st;
        if (!read_inode(inode_num, &st)) {
            printf("stat: cannot read inode %u\n", inode_num);
            continue;
        }

        printf("  Path:      %s\n", target);
        printf("  Inode:     %u\n", inode_num);
        printf("  Type:      %s\n", (st.mode & IPO_INODE_TYPE_DIR) ? "Directory" : "Regular File");
        printf("  Protected: %s\n", (st.mode & IPO_INODE_FLAG_PROTECTED) ? "Yes" : "No");
        printf("  Size:      %u bytes\n", st.size);
        printf("  Links:     %u\n", st.links_count);
    }
}

static void builtin_echo(int argc, char **argv, const char *cmdline) {
    const char *append_pos = strstr(cmdline, ">>");
    const char *write_pos = strchr(cmdline, '>');
    if (write_pos) {
        bool append = (append_pos != NULL && append_pos == write_pos);
        const char *file_part = append ? (write_pos + 2) : (write_pos + 1);
        while (*file_part == ' ' || *file_part == '\t') file_part++;
        if (*file_part) {
            char target[256];
            char fn[256];
            int fnp = 0;
            while (*file_part && *file_part != ' ' && *file_part != '\t' && *file_part != '\n' && fnp < 255) {
                fn[fnp++] = *file_part++;
            }
            fn[fnp] = '\0';
            make_abs_path(fn, target, sizeof(target));

            const char *text_start = cmdline;
            while (*text_start && (*text_start == ' ' || *text_start == '\t')) text_start++;
            if (strncmp(text_start, "echo", 4) == 0) text_start += 4;
            while (*text_start && (*text_start == ' ' || *text_start == '\t')) text_start++;

            size_t text_len = (size_t)(write_pos - text_start);
            while (text_len > 0 && (text_start[text_len - 1] == ' ' || text_start[text_len - 1] == '\t')) {
                text_len--;
            }
            char *text_buf = kmalloc(text_len + 2);
            if (text_buf) {
                memcpy(text_buf, text_start, text_len);
                text_buf[text_len] = '\n';
                text_buf[text_len + 1] = '\0';
                if (!ipo_fs_write_text(target, text_buf, append)) {
                    printf("echo: failed to write to %s\n", fn);
                }
                kfree(text_buf);
            }
            return;
        }
    }

    for (int i = 1; i < argc; i++) {
        printf("%s%s", argv[i], (i + 1 < argc) ? " " : "");
    }
    putchar('\n');
}

static void builtin_reboot(void) {
    printf("Rebooting system...\n");
    for (volatile int i = 0; i < 500000; i++) io_wait();

    // 8042 Keyboard Controller reset
    while (inb(0x64) & 0x02) io_wait();
    outb(0x64, 0xFE);

    // Fast A20 / reset port 0x92
    uint8_t temp = inb(0x92);
    outb(0x92, (temp | 0x01));

    // Fallback: Triple fault by loading empty IDT
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile("lidt %0; int3" : : "m"(null_idt));

    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void builtin_shutdown(void) {
    printf("Shutting down...\n");

    // QEMU modern ACPI shutdown
    outw(0x604, 0x2000);
    // QEMU older / Bochs ACPI shutdown
    outw(0xB004, 0x2000);
    // VirtualBox shutdown
    outw(0x4004, 0x3400);
    // APM shutdown
    outw(0x5307, 0x0001);

    printf("System halted. You may safely turn off your computer.\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void terminal_initialize(void) {
    vga_clear(
        VGA_COLOR_WHITE,
        VGA_COLOR_BLACK,
        true,
        VGA_START_CURSOR_POSITION
    );

    print_header();

    /* Flush any scancodes that accumulated during boot */
    keyboard_flush_hardware();
    keyboard_flush_queue();

    input_len = 0;
    cursor_pos = 0;
    prompt_shown = false;
    prompt_start_cursor = 0;
    terminal_suppress_external_hook = false;

    ensure_input_buffer(0);
    if (input_buf != NULL) {
        input_buf[0] = '\0';
    }

    input_start_cursor = 0;

    command_history_count = 0;
    command_history_index = -1;
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
    terminal_suppress_external_hook = true;
    prompt_start_cursor = vga_get_cursor_position();
    for (const char *p = terminal_cwd; *p; p++) {
        putchar_color(*p, PROMPT_FG, VGA_COLOR_BLACK);
    }
    putchar_color('>', PROMPT_FG, VGA_COLOR_BLACK);
    putchar(' ');
    prompt_shown = true;
    input_start_cursor = vga_get_cursor_position();
    render_input_line(input_buf);
    terminal_suppress_external_hook = false;
}

int try_execute_command(const char *cmdline) {
    if (!cmdline) return -1;

    // Extract command name (first token)
    const char *p = cmdline;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return 0;

    size_t name_cap = 256u;
    char *name = kmalloc(name_cap);
    if (name == NULL) {
        return -1;
    }
    size_t i = 0u;
    while (*p && *p != ' ' && *p != '\t' && i + 1u < name_cap) {
        name[i++] = *p++;
    }
    name[i] = '\0';

    // Parse arguments from cmdline
    char **argv = NULL;
    int argc = 0;
    int argv_cap = 8;
    argv = kmalloc((size_t)argv_cap * sizeof(char *));
    if (argv == NULL) {
        kfree(name);
        return -1;
    }
    argv[argc++] = name;

    while (*p && (*p == ' ' || *p == '\t')) p++;

    char *arg_buf = kmalloc(256u);
    size_t arg_pos = 0u;
    int in_arg = 0;

    while (*p) {
        if (*p == ' ' || *p == '\t') {
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
                            for (int j = 0; j < argc; j++) kfree(argv[j]);
                            kfree(argv);
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
            p++;
            continue;
        }

        if (arg_pos + 1u >= 256u) {
            char *extended = kmalloc(arg_pos + 256u);
            if (extended == NULL) {
                kfree(arg_buf);
                for (int j = 0; j < argc; j++) kfree(argv[j]);
                kfree(argv);
                return -1;
            }
            memcpy(extended, arg_buf, arg_pos);
            kfree(arg_buf);
            arg_buf = extended;
        }
        arg_buf[arg_pos++] = *p++;
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
                    for (int j = 0; j < argc; j++) kfree(argv[j]);
                    kfree(argv);
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

    // Check Builtins
    int builtin_handled = 0;
    if (strcmp(name, "help") == 0 || strcmp(name, "?") == 0) {
        builtin_help();
        builtin_handled = 1;
    } else if (strcmp(name, "clear") == 0 || strcmp(name, "cls") == 0) {
        builtin_clear();
        builtin_handled = 1;
    } else if (strcmp(name, "pwd") == 0) {
        builtin_pwd();
        builtin_handled = 1;
    } else if (strcmp(name, "cd") == 0) {
        builtin_cd(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "ls") == 0 || strcmp(name, "dir") == 0) {
        builtin_ls(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "cat") == 0 || strcmp(name, "type") == 0) {
        builtin_cat(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "touch") == 0) {
        builtin_touch(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "mkdir") == 0) {
        builtin_mkdir(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "rm") == 0 || strcmp(name, "del") == 0 || strcmp(name, "rmdir") == 0) {
        builtin_rm(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "cp") == 0 || strcmp(name, "copy") == 0) {
        builtin_cp(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "mv") == 0 || strcmp(name, "move") == 0 || strcmp(name, "rename") == 0) {
        builtin_mv(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "stat") == 0) {
        builtin_stat(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "echo") == 0) {
        builtin_echo(argc, argv, cmdline);
        builtin_handled = 1;
    } else if (strcmp(name, "reboot") == 0 || strcmp(name, "restart") == 0) {
        builtin_reboot();
        builtin_handled = 1;
    } else if (strcmp(name, "shutdown") == 0 || strcmp(name, "poweroff") == 0 ||
               strcmp(name, "exit") == 0 || strcmp(name, "halt") == 0) {
        builtin_shutdown();
        builtin_handled = 1;
    }

    if (builtin_handled) {
        for (int j = 0; j < argc; j++) {
            kfree(argv[j]);
        }
        kfree(argv);
        return 1000; // Special code for builtin success
    }

    // Resolve to filesystem path for external executables
    char *path = resolve_command_path(name);
    if (!path) {
        for (int j = 0; j < argc; j++) {
            kfree(argv[j]);
        }
        kfree(argv);
        return 0; // not found
    }

    // Execute program with arguments
    int result = process_exec(path, argc, argv);

    // Free allocated argument copies
    for (int j = 0; j < argc; j++) {
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

        /* Keybindings:
         * Left / Right arrows: cursor movement in current line
         * Up / Down arrows: command history navigation
         * PageUp / PageDown: screen scrolling
         * Home / End / Delete: in-line editing
         */
        if (!is_break_code) {
            if (scancode == SC_PAGE_UP) {
                scroll_up();
                return;
            }
            if (scancode == SC_PAGE_DOWN) {
                scroll_down();
                return;
            }
            if (scancode == SC_ARROW_UP) {
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
                    } else if (command_history_index > 0) {
                        command_history_index--;
                    }

                    apply_history_position();
                }
                return;
            }
            if (scancode == SC_ARROW_DOWN) {
                if (command_history_index >= 0) {
                    uint32_t now = timer_millis();
                    if (now - last_history_action_ms < 150u) {
                        return;
                    }
                    last_history_action_ms = now;

                    return_to_present();

                    if (command_history_index < command_history_count - 1) {
                        command_history_index++;
                        apply_history_position();
                    } else {
                        command_history_index = -1;
                        restore_snapshot_input();
                        if (current_input_snapshot != NULL) {
                            current_input_snapshot[0] = '\0';
                        }
                    }
                }
                return;
            }
            if (scancode == SC_ARROW_LEFT) {
                return_to_present();
                if (cursor_pos > 0) {
                    cursor_pos--;
                    render_input_line(input_buf);
                }
                return;
            }
            if (scancode == SC_ARROW_RIGHT) {
                return_to_present();
                if (cursor_pos < input_len) {
                    cursor_pos++;
                    render_input_line(input_buf);
                }
                return;
            }
            if (scancode == SC_HOME) {
                return_to_present();
                cursor_pos = 0;
                render_input_line(input_buf);
                return;
            }
            if (scancode == SC_END) {
                return_to_present();
                cursor_pos = input_len;
                render_input_line(input_buf);
                return;
            }
            if (scancode == SC_DELETE) {
                return_to_present();
                if (cursor_pos < input_len) {
                    memmove(&input_buf[cursor_pos], &input_buf[cursor_pos + 1], input_len - cursor_pos);
                    input_len--;
                    input_buf[input_len] = '\0';
                    render_input_line(input_buf);
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
                    terminal_suppress_external_hook = true;
                    uint16_t cur_end = input_start_cursor + (uint16_t)input_len;
                    if (cur_end >= VGA_WIDTH * VGA_HEIGHT) {
                        cur_end = VGA_WIDTH * VGA_HEIGHT - 1;
                    }
                    vga_set_cursor(cur_end);
                    putchar('\n');

                    input_buf[input_len] = '\0';

                    if (input_len > 0) {
                        push_command_history(input_buf);
                        int exec = try_execute_command(input_buf);
                        if (exec == 0) {
                            printf("Command not found: %s\n", input_buf);
                        } else if (exec < 0) {
                            printf("Execution failed (error %d): %s\n", exec, input_buf);
                        } else if (exec != 1000) {
                            int ret = process_get_exit_code();
                            printf("Return value: %d\n", ret);
                        }
                    }

                    /* Reset buffer and show prompt */
                    input_len = 0;
                    cursor_pos = 0;
                    input_buf[0] = '\0';
                    if (current_input_snapshot != NULL) {
                        current_input_snapshot[0] = '\0';
                    }
                    command_history_index = -1;
                    prompt_shown = false;
                    terminal_suppress_external_hook = false;
                    print_prompt();
                }
                /* Handle backspace */
                else if (c == '\b' || c == 127) {
                    if (cursor_pos > 0) {
                        memmove(&input_buf[cursor_pos - 1], &input_buf[cursor_pos], input_len - cursor_pos + 1);
                        cursor_pos--;
                        input_len--;
                        input_buf[input_len] = '\0';
                        render_input_line(input_buf);
                    }
                }
                else if (c == '\t') {
                    ensure_input_buffer(input_len + 4u);
                    memmove(&input_buf[cursor_pos + 4], &input_buf[cursor_pos], input_len - cursor_pos + 1);
                    for (int s = 0; s < 4; s++) {
                        input_buf[cursor_pos + s] = ' ';
                    }
                    cursor_pos += 4;
                    input_len += 4;
                    input_buf[input_len] = '\0';
                    render_input_line(input_buf);
                }
                /* Printable characters */
                else if (c >= 32 && c < 127) {
                    ensure_input_buffer(input_len + 1u);
                    memmove(&input_buf[cursor_pos + 1], &input_buf[cursor_pos], input_len - cursor_pos + 1);
                    input_buf[cursor_pos] = c;
                    cursor_pos++;
                    input_len++;
                    input_buf[input_len] = '\0';
                    render_input_line(input_buf);
                }
            }
        }
    }
}

/* Auto scroll when terminal overflows - keep only current screen in RAM */
void terminal_auto_scroll(void) {
    volatile uint16_t *vga = VGA_MEMORY;

    if (prompt_shown) {
        if (input_start_cursor >= VGA_WIDTH) {
            input_start_cursor -= VGA_WIDTH;
        }
        if (prompt_start_cursor >= VGA_WIDTH) {
            prompt_start_cursor -= VGA_WIDTH;
        }
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