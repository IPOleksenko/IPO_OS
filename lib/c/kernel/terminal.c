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
#include <driver/sound.h>
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
static int32_t prompt_start_cursor = 0;
static int32_t prompt_origin = 0;
static int32_t input_start_cursor = 0;
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

void terminal_scroll_up(void);
void terminal_scroll_down(void);

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
    size_t cwd_l = terminal_cwd ? strlen(terminal_cwd) : 0u;
    size_t rel_l = strlen(rel_or_abs);
    size_t temp_size = cwd_l + rel_l + 32u;
    if (temp_size < out_size) temp_size = out_size;
    char *temp = kmalloc(temp_size);
    if (!temp) return;

    if (rel_or_abs[0] == '/') {
        strncpy(temp, rel_or_abs, temp_size - 1);
        temp[temp_size - 1] = '\0';
    } else {
        if (strcmp(terminal_cwd, "/") == 0) {
            snprintf(temp, temp_size, "/%s", rel_or_abs);
        } else {
            snprintf(temp, temp_size, "%s/%s", terminal_cwd, rel_or_abs);
        }
    }
    fs_canonicalize(temp, out, out_size);
    kfree(temp);
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

static size_t previous_rendered_vis_len = 0;

static size_t terminal_visual_offset(size_t index) {
    size_t vcol = 0;
    for (size_t i = 0; i < index && i < input_len; i++) {
        if (input_buf[i] == '\t') {
            size_t tab_spaces = 4 - (vcol % 4);
            if (tab_spaces == 0) tab_spaces = 4;
            vcol += tab_spaces;
        } else {
            vcol += 1;
        }
    }
    return vcol;
}

static void render_input_line(size_t previous_len) {
    (void)previous_len;

    size_t vis_cursor = terminal_visual_offset(cursor_pos);
    size_t vis_len = terminal_visual_offset(input_len);

    /* If cursor is off the bottom of the screen, scroll up */
    while (input_start_cursor + (int32_t)vis_cursor >= VGA_WIDTH * VGA_HEIGHT) {
        terminal_auto_scroll();
    }
    /* If text tail exceeds bottom of screen, scroll up */
    while (input_start_cursor + (int32_t)vis_len >= VGA_WIDTH * VGA_HEIGHT) {
        terminal_auto_scroll();
    }

    volatile uint16_t *vga = VGA_MEMORY;

    /* Draw prompt prefix if visible */
    size_t cwd_len = strlen(terminal_cwd);
    int32_t p_cur = prompt_start_cursor;
    for (size_t i = 0; i < cwd_len; i++) {
        if (p_cur >= VGA_START_CURSOR_POSITION && p_cur < VGA_WIDTH * VGA_HEIGHT) {
            vga[p_cur] = vga_entry((unsigned char)terminal_cwd[i], PROMPT_FG, VGA_COLOR_BLACK);
        }
        p_cur++;
    }
    if (p_cur >= VGA_START_CURSOR_POSITION && p_cur < VGA_WIDTH * VGA_HEIGHT) {
        vga[p_cur] = vga_entry('>', PROMPT_FG, VGA_COLOR_BLACK);
    }
    p_cur++;
    if (p_cur >= VGA_START_CURSOR_POSITION && p_cur < VGA_WIDTH * VGA_HEIGHT) {
        vga[p_cur] = vga_entry(' ', PROMPT_FG, VGA_COLOR_BLACK);
    }

    /* Draw text: expand '\t' into visual tab spacing (no circle glyph) */
    size_t vcol = 0;
    for (size_t i = 0; i < input_len; i++) {
        if (input_buf[i] == '\t') {
            size_t tab_spaces = 4 - (vcol % 4);
            if (tab_spaces == 0) tab_spaces = 4;
            for (size_t s = 0; s < tab_spaces; s++) {
                int32_t offset = input_start_cursor + (int32_t)(vcol + s);
                if (offset >= VGA_START_CURSOR_POSITION && offset < VGA_WIDTH * VGA_HEIGHT) {
                    vga[offset] = vga_entry(' ', INPUT_FG, VGA_COLOR_BLACK);
                }
            }
            vcol += tab_spaces;
        } else {
            int32_t offset = input_start_cursor + (int32_t)vcol;
            if (offset >= VGA_START_CURSOR_POSITION && offset < VGA_WIDTH * VGA_HEIGHT) {
                vga[offset] = vga_entry((unsigned char)input_buf[i], INPUT_FG, VGA_COLOR_BLACK);
            }
            vcol += 1;
        }
    }

    /* Clear any old characters if line became shorter */
    for (size_t i = vcol; i < previous_rendered_vis_len; i++) {
        int32_t offset = input_start_cursor + (int32_t)i;
        if (offset >= VGA_START_CURSOR_POSITION && offset < VGA_WIDTH * VGA_HEIGHT) {
            vga[offset] = vga_entry(' ', INPUT_FG, VGA_COLOR_BLACK);
        }
    }
    previous_rendered_vis_len = vcol;

    /* Position cursor */
    int32_t cur = input_start_cursor + (int32_t)vis_cursor;
    if (cur < VGA_START_CURSOR_POSITION) {
        cur = VGA_START_CURSOR_POSITION;
    }
    if (cur >= VGA_WIDTH * VGA_HEIGHT) {
        cur = VGA_WIDTH * VGA_HEIGHT - 1;
    }
    vga_set_cursor((uint16_t)cur);
    vga_show_cursor();
}

void terminal_on_external_output(void) {
    if (!prompt_shown || terminal_suppress_external_hook) {
        return;
    }

    volatile uint16_t *vga = VGA_MEMORY;
    uint16_t blank = vga_entry(0x00, VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    int32_t start_clear = prompt_start_cursor >= VGA_START_CURSOR_POSITION ? prompt_start_cursor : VGA_START_CURSOR_POSITION;
    int32_t end_clear = input_start_cursor + (int32_t)input_len;
    for (int32_t p = start_clear; p <= end_clear && p < VGA_WIDTH * VGA_HEIGHT; p++) {
        vga[p] = blank;
    }

    if (prompt_start_cursor >= VGA_START_CURSOR_POSITION && prompt_start_cursor < VGA_WIDTH * VGA_HEIGHT) {
        vga_set_cursor((uint16_t)prompt_start_cursor);
    } else {
        vga_set_cursor(VGA_START_CURSOR_POSITION);
    }
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

    size_t old_len = input_len;
    ensure_input_buffer(strlen(command_history[index]));
    strcpy(input_buf, command_history[index]);
    input_len = strlen(input_buf);
    cursor_pos = input_len;
    render_input_line(old_len);
}

static void apply_history_position(void) {
    if (command_history_index < 0) {
        restore_snapshot_input();
        return;
    }

    load_history_command(command_history_index);
}

static void restore_snapshot_input(void) {
    size_t old_len = input_len;
    if (current_input_snapshot == NULL) {
        input_len = 0;
        cursor_pos = 0;
        if (input_buf != NULL) {
            input_buf[0] = '\0';
        }
        render_input_line(old_len);
        return;
    }
    ensure_input_buffer(strlen(current_input_snapshot));
    strcpy(input_buf, current_input_snapshot);
    input_len = strlen(input_buf);
    cursor_pos = input_len;
    render_input_line(old_len);
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

/* Scroll down - restore next line from history if available */
void terminal_scroll_down(void) {
    if (bottom_buffer_count == 0) {
        return;
    }

    volatile uint16_t *vga = VGA_MEMORY;

    uint16_t top = terminal_top_row();
    uint16_t rows = terminal_rows();

    if (rows <= 1) {
        return;
    }

    if (prompt_shown) {
        prompt_start_cursor -= VGA_WIDTH;
        input_start_cursor -= VGA_WIDTH;
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

/* Return to present - restore current output when user starts typing */
void terminal_return_to_present(void) {
    while (bottom_buffer_count > 0) {
        terminal_scroll_down();
    }
    vga_show_cursor();
}

int terminal_get_top_buffer_count(void) {
    return top_buffer_count;
}

int terminal_get_bottom_buffer_count(void) {
    return bottom_buffer_count;
}

/* Scroll up - show previous line from history */
void terminal_scroll_up(void) {
    if (top_buffer_count == 0) {
        return;
    }

    volatile uint16_t *vga = VGA_MEMORY;

    uint16_t top = terminal_top_row();
    uint16_t rows = terminal_rows();

    if (rows <= 1) {
        return;
    }

    if (prompt_shown) {
        prompt_start_cursor += VGA_WIDTH;
        input_start_cursor += VGA_WIDTH;
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

    size_t cmd_len = strlen(cmd);
    size_t cwd_len = terminal_cwd ? strlen(terminal_cwd) : 0u;
    size_t buf_size = cmd_len + cwd_len + 32u;
    if (buf_size < 256u) {
        buf_size = 256u;
    }

    char *to_check = kmalloc(buf_size);
    char *canonical = kmalloc(buf_size);
    char *path = kmalloc(buf_size);

    if (!to_check || !canonical || !path) {
        if (to_check) kfree(to_check);
        if (canonical) kfree(canonical);
        if (path) kfree(path);
        return NULL;
    }

    uint32_t inode;
    struct ipo_inode stat;

    // Absolute path
    if (cmd[0] == '/') {
        strncpy(to_check, cmd, buf_size - 1);
        to_check[buf_size - 1] = '\0';
    } 
    // Relative path with ./ or ../ or subdirectories
    else if (cmd[0] == '.' || strchr(cmd, '/')) {
        make_abs_path(cmd, to_check, buf_size);
    }
    // Simple command name: try in cwd first, then in /app/
    else {
        make_abs_path(cmd, to_check, buf_size);
        fs_canonicalize(to_check, canonical, buf_size);
        if (path_resolve(canonical, &inode) == 0 && 
            ipo_fs_stat(canonical, &stat) && 
            (stat.mode & IPO_INODE_TYPE_DIR) == 0) {
            strncpy(path, canonical, buf_size - 1);
            path[buf_size - 1] = '\0';
            kfree(to_check);
            kfree(canonical);
            return path;
        }

        snprintf(to_check, buf_size, "/app/%s", cmd);
    }

    // Canonicalize to handle .., ., //, etc
    fs_canonicalize(to_check, canonical, buf_size);

    // Try to resolve and verify it's a file (not directory)
    if (path_resolve(canonical, &inode) == 0 && 
        ipo_fs_stat(canonical, &stat) && 
        (stat.mode & IPO_INODE_TYPE_DIR) == 0) {
        strncpy(path, canonical, buf_size - 1);
        path[buf_size - 1] = '\0';
        kfree(to_check);
        kfree(canonical);
        return path;
    }

    kfree(to_check);
    kfree(canonical);
    kfree(path);
    return NULL;
}

static void builtin_help(void) {
    printf("======================= IPO_OS SYSTEM HELP =======================\n");
    printf("\n");
    printf("1. BUILTIN SHELL COMMANDS & SYNTAX\n");
    printf("  [Navigation]\n");
    printf("    pwd\n");
    printf("      - Print absolute path of current working directory.\n");
    printf("    cd [path | ~ | .. | .]\n");
    printf("      - Change working directory to target path, root (~), or parent (..).\n");
    printf("\n");
    printf("  [File & Directory Operations]\n");
    printf("    ls [path] | dir [path]\n");
    printf("      - List directory contents showing item type and file size.\n");
    printf("    cat <file...>\n");
    printf("      - Display contents of one or multiple files sequentially.\n");
    printf("    touch <file...>\n");
    printf("      - Create one or multiple new empty files.\n");
    printf("    mkdir <dir...>\n");
    printf("      - Create one or multiple new directories.\n");
    printf("    rm <path...> | del <path...> | rmdir <path...>\n");
    printf("      - Delete one or multiple files or empty directories.\n");
    printf("    cp <src> <dst> | copy <src> <dst>\n");
    printf("      - Copy file to destination path or into destination directory.\n");
    printf("    mv <src> <dst> | move <src> <dst> | rename <src> <dst>\n");
    printf("      - Move or rename a file or directory.\n");
    printf("    stat <path...>\n");
    printf("      - Display inode number, type, size, protection and links count.\n");
    printf("\n");
    printf("  [Process & Task Management]\n");
    printf("    ps | tasks | procs\n");
    printf("      - List all active and background processes with PID, state, memory.\n");
    printf("    kill <pid>\n");
    printf("      - Terminate a running or background process and free its resources.\n");
    printf("    killall\n");
    printf("      - Terminate all active processes and stop background async tasks.\n");
    printf("\n");
    printf("  [Output & Redirection]\n");
    printf("    echo [text] [> file | >> file]\n");
    printf("      - Print text to stdout, overwrite to file (>), or append (>>).\n");
    printf("\n");
    printf("  [System & Power Controls]\n");
    printf("    clear | cls\n");
    printf("      - Clear terminal screen and reset VGA scrollback buffer.\n");
    printf("    reboot | restart\n");
    printf("      - Reboot hardware via 8042 controller, port 0x92 or triple fault.\n");
    printf("    shutdown | poweroff | exit | halt\n");
    printf("      - Power off machine via ACPI/APM or halt CPU execution safely.\n");
    printf("    help | ?\n");
    printf("      - Display this system documentation and syntax reference.\n");
    printf("\n");
    printf("  [Disk & Memory Info]\n");
    printf("    df | diskinfo\n");
    printf("      - Show filesystem space: total, used, free blocks, inodes, FS metadata.\n");
    printf("    meminfo | free\n");
    printf("      - Show kernel heap stats: total, used, free, block count, overhead.\n");
    printf("\n");
    printf("2. APPLICATION EXECUTION (Launch Methods & Resolution)\n");
    printf("  IPO_OS executes Position Independent Executables (.bin) via four methods:\n");
    printf("  [Method 1: Direct Name (Automatic PATH Resolution)]\n");
    printf("    Syntax: <command> [arguments...]\n");
    printf("    - Searches current directory, /app directory, then root directory.\n");
    printf("  [Method 2: Explicit Relative or Absolute Path]\n");
    printf("    Syntax: <path/to/binary> [arguments...]\n");
    printf("    - Resolves absolute paths starting with '/' or relative paths.\n");
    printf("  [Method 3: Batch Startup Script]\n");
    printf("    - Reads commands line-by-line from '/autorun' during kernel boot.\n");
    printf("\n");
    printf("3. TERMINAL SHORTCUTS & KEYBOARD CONTROLS\n");
    printf("    - Ctrl + C            : Instant cancel/abort of active task or queue.\n");
    printf("    - Page Up / Page Down : Scroll terminal output 5 lines up / down.\n");
    printf("    - Up / Down Arrows    : Command history navigation or line scroll.\n");
    printf("    - Left / Right Arrows : Move cursor across current line.\n");
    printf("    - Home / End          : Move cursor to beginning / end of line.\n");
    printf("============================================================================\n");
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
    previous_rendered_vis_len = 0;
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

static void builtin_ps(void) {
    process_list_print();
}

static void builtin_kill(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: kill <pid>\n");
        return;
    }
    const char *str = argv[1];
    uint32_t pid = 0;
    while (*str >= '0' && *str <= '9') {
        pid = pid * 10 + (uint32_t)(*str - '0');
        str++;
    }
    if (pid == 0 || *str != '\0') {
        printf("kill: invalid PID '%s'\n", argv[1]);
        return;
    }
    process_kill_by_pid(pid);
}

static void builtin_killall(void) {
    process_kill_all();
}

/* ---------------------------------------------------------------
 * Helper: count used/total blocks by scanning the block bitmap
 * --------------------------------------------------------------- */
static void fs_count_blocks(uint32_t *out_used, uint32_t *out_total) {
    *out_total = 0;
    *out_used  = 0;
    if (!fs_mounted) return;

    /* data_blocks_start is the first LBA of data area;
       total data blocks = fs_size_blocks - data_blocks_start */
    uint32_t data_start = sb.data_blocks_start;
    uint32_t total = (sb.fs_size_blocks > data_start)
                     ? sb.fs_size_blocks - data_start : 0;
    *out_total = total;

    uint32_t used = 0;
    for (uint32_t i = 0; i < total; i++) {
        if (bitmap_get(sb.block_bitmap_start, i)) {
            used++;
        }
    }
    *out_used = used;
}

/* ---------------------------------------------------------------
 * Helper: count used/total inodes by scanning the inode bitmap
 * --------------------------------------------------------------- */
static void fs_count_inodes(uint32_t *out_used, uint32_t *out_total) {
    *out_total = sb.inode_count;
    uint32_t used = 0;
    for (uint32_t i = 0; i < sb.inode_count; i++) {
        if (bitmap_get(sb.inode_bitmap_start, i)) {
            used++;
        }
    }
    *out_used = used;
}

static void builtin_df(void) {
    if (!fs_mounted) {
        printf("df: no filesystem mounted\n");
        return;
    }

    uint32_t blk_used, blk_total;
    fs_count_blocks(&blk_used, &blk_total);

    uint32_t ino_used, ino_total;
    fs_count_inodes(&ino_used, &ino_total);

    uint32_t blk_free = (blk_total >= blk_used) ? blk_total - blk_used : 0;

    uint32_t bs   = sb.block_size;           /* bytes per block */
    uint32_t used_kb = (blk_used  * bs) / 1024;
    uint32_t free_kb = (blk_free  * bs) / 1024;
    uint32_t total_kb = (blk_total * bs) / 1024;
    uint32_t used_pct = blk_total ? (blk_used * 100u) / blk_total : 0;

    printf("Filesystem : IPO_FS\n");
    printf("Block size : %u bytes\n", bs);
    printf("Blocks     : %u total, %u used, %u free\n",
           blk_total, blk_used, blk_free);
    printf("Space      : %u KB total, %u KB used, %u KB free  (%u%% used)\n",
           total_kb, used_kb, free_kb, used_pct);
    printf("Inodes     : %u total, %u used, %u free\n",
           ino_total, ino_used,
           (ino_total >= ino_used) ? ino_total - ino_used : 0);
    printf("FS size    : %u blocks (%u KB)\n",
           sb.fs_size_blocks, (sb.fs_size_blocks * bs) / 1024);
    printf("Start LBA  : %u\n", fs_start_lba);
}

static void builtin_meminfo(void) {
    kmalloc_stats_t st;
    kmalloc_get_stats(&st);

    size_t heap_avail  = (st.heap_total > st.heap_used)
                         ? st.heap_total - st.heap_used : 0;
    size_t real_free   = st.free_bytes + heap_avail;
    size_t header_overhead = (st.alloc_blocks + st.free_blocks) * st.block_header;
    size_t used_pct = st.heap_total
                      ? (st.heap_used * 100u) / st.heap_total : 0;

    printf("=== Memory (Kernel Heap) ===\n");
    printf("Heap start : 0x%x\n", 0x1000000);
    printf("Heap total : %u KB  (%u MB)\n",
           (uint32_t)(st.heap_total / 1024),
           (uint32_t)(st.heap_total / 1024 / 1024));
    printf("Heap used  : %u KB  (watermark, %u%% of capacity)\n",
           (uint32_t)(st.heap_used / 1024), (uint32_t)used_pct);
    printf("Heap avail : %u KB  (never-touched tail)\n",
           (uint32_t)(heap_avail / 1024));
    printf("\n");
    printf("Alloc'd    : %u KB  in %u block(s)\n",
           (uint32_t)(st.alloc_bytes / 1024), (uint32_t)st.alloc_blocks);
    printf("Freed      : %u KB  in %u block(s)\n",
           (uint32_t)(st.free_bytes / 1024), (uint32_t)st.free_blocks);
    printf("Total free : %u KB  (freed blocks + untouched tail)\n",
           (uint32_t)(real_free / 1024));
    printf("Overhead   : %u bytes  (%u block headers)\n",
           (uint32_t)header_overhead,
           (uint32_t)(st.alloc_blocks + st.free_blocks));
    printf("Block hdr  : %u bytes\n", (uint32_t)st.block_header);
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
    prompt_origin = (int32_t)vga_get_cursor_position();
    if (prompt_origin < VGA_START_CURSOR_POSITION || prompt_origin >= VGA_WIDTH * VGA_HEIGHT) {
        prompt_origin = VGA_START_CURSOR_POSITION;
        vga_set_cursor((uint16_t)prompt_origin);
    }
    prompt_start_cursor = prompt_origin;
    for (const char *p = terminal_cwd; *p; p++) {
        putchar_color(*p, PROMPT_FG, VGA_COLOR_BLACK);
    }
    putchar_color('>', PROMPT_FG, VGA_COLOR_BLACK);
    putchar(' ');
    prompt_shown = true;
    input_start_cursor = (int32_t)vga_get_cursor_position();
    input_len = 0;
    cursor_pos = 0;
    previous_rendered_vis_len = 0;
    if (input_buf != NULL) {
        input_buf[0] = '\0';
    }
    terminal_suppress_external_hook = false;
    terminal_apply_pending_input();
}

static char *pending_terminal_input = NULL;
static bool pending_terminal_auto_exec = false;

void terminal_inject_input(const char *text, bool auto_execute) {
    if (pending_terminal_input != NULL) {
        kfree(pending_terminal_input);
        pending_terminal_input = NULL;
    }
    if (text != NULL) {
        size_t len = strlen(text);
        pending_terminal_input = kmalloc(len + 1u);
        if (pending_terminal_input != NULL) {
            memcpy(pending_terminal_input, text, len + 1u);
            pending_terminal_auto_exec = auto_execute;
        }
    }
}

void terminal_apply_pending_input(void) {
    if (pending_terminal_input == NULL || !prompt_shown) {
        return;
    }
    char *text = pending_terminal_input;
    bool auto_exec = pending_terminal_auto_exec;
    pending_terminal_input = NULL;

    size_t len = strlen(text);
    ensure_input_buffer(len + 1u);
    if (input_buf != NULL) {
        memcpy(input_buf, text, len + 1u);
        input_len = len;
        cursor_pos = len;
        render_input_line(0);

        if (auto_exec) {
            terminal_suppress_external_hook = true;
            uint16_t cur_end = input_start_cursor + (uint16_t)terminal_visual_offset(input_len);
            if (cur_end >= VGA_WIDTH * VGA_HEIGHT) {
                cur_end = VGA_WIDTH * VGA_HEIGHT - 1;
            }
            vga_set_cursor(cur_end);
            putchar('\n');

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
    }
    kfree(text);
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
    } else if (strcmp(name, "ps") == 0 || strcmp(name, "tasks") == 0 || strcmp(name, "procs") == 0) {
        builtin_ps();
        builtin_handled = 1;
    } else if (strcmp(name, "kill") == 0) {
        builtin_kill(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "killall") == 0) {
        builtin_killall();
        builtin_handled = 1;
    } else if (strcmp(name, "df") == 0 || strcmp(name, "diskinfo") == 0) {
        builtin_df();
        builtin_handled = 1;
    } else if (strcmp(name, "meminfo") == 0 || strcmp(name, "free") == 0) {
        builtin_meminfo();
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

    if (!prompt_shown) print_prompt();
    terminal_apply_pending_input();

    if (scancode != 0x00) {
        update_hot_key_state(scancode);

        /* 1. Dispatch application hotkeys (highest priority) and system hotkeys */
        if (keyboard_dispatch_hotkey(scancode)) {
            return;
        }

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

        /* 2. Default Ctrl + C: clear current command input and request interrupt */
        if (!is_break_code && keyboard_is_ctrl_pressed() && scancode == 0x2E) {
            sound_stop();
            system_request_interrupt();
            terminal_return_to_present();
            printf("^C\n");
            input_len = 0;
            cursor_pos = 0;
            if (input_buf) input_buf[0] = '\0';
            print_prompt();
            return;
        }

        /* Keybindings:
         * Left / Right arrows: cursor movement in current line
         * Up / Down arrows: command history navigation
         * PageUp / PageDown: screen scrolling
         * Home / End / Delete: in-line editing
         */
        if (!is_break_code) {
            if (scancode == SC_PAGE_UP) {
                for (int i = 0; i < 5 && top_buffer_count > 0; i++) {
                    terminal_scroll_up();
                }
                return;
            }
            if (scancode == SC_PAGE_DOWN) {
                for (int i = 0; i < 5 && bottom_buffer_count > 0; i++) {
                    terminal_scroll_down();
                }
                return;
            }
            if (scancode == SC_ARROW_UP) {
                if (bottom_buffer_count > 0) {
                    terminal_scroll_up();
                    return;
                }
                if (cursor_pos >= VGA_WIDTH) {
                    cursor_pos -= VGA_WIDTH;
                    render_input_line(input_len);
                    return;
                }
                if (command_history_count > 0) {
                    uint32_t now = timer_millis();
                    if (now - last_history_action_ms < 150u) {
                        return;
                    }
                    last_history_action_ms = now;

                    terminal_return_to_present();

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
                if (bottom_buffer_count > 0) {
                    terminal_scroll_down();
                    return;
                }
                if (cursor_pos + VGA_WIDTH <= input_len) {
                    cursor_pos += VGA_WIDTH;
                    render_input_line(input_len);
                    return;
                }
                if (command_history_index >= 0) {
                    uint32_t now = timer_millis();
                    if (now - last_history_action_ms < 150u) {
                        return;
                    }
                    last_history_action_ms = now;

                    terminal_return_to_present();

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
                terminal_return_to_present();
                if (cursor_pos > 0) {
                    cursor_pos--;
                    render_input_line(input_len);
                }
                return;
            }
            if (scancode == SC_ARROW_RIGHT) {
                terminal_return_to_present();
                if (cursor_pos < input_len) {
                    cursor_pos++;
                    render_input_line(input_len);
                }
                return;
            }
            if (scancode == SC_HOME) {
                terminal_return_to_present();
                cursor_pos = 0;
                render_input_line(input_len);
                return;
            }
            if (scancode == SC_END) {
                terminal_return_to_present();
                cursor_pos = input_len;
                render_input_line(input_len);
                return;
            }
            if (scancode == SC_DELETE) {
                terminal_return_to_present();
                if (cursor_pos < input_len) {
                    size_t old_len = input_len;
                    memmove(&input_buf[cursor_pos], &input_buf[cursor_pos + 1], input_len - cursor_pos);
                    input_len--;
                    input_buf[input_len] = '\0';
                    render_input_line(old_len);
                }
                return;
            }
        }

        if (!is_break_code) {
            char c = get_char(scancode);
            if (c != 0x00) {
                // Return to present when user starts typing
                terminal_return_to_present();
                
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
                        size_t old_len = input_len;
                        memmove(&input_buf[cursor_pos - 1], &input_buf[cursor_pos], input_len - cursor_pos + 1);
                        cursor_pos--;
                        input_len--;
                        input_buf[input_len] = '\0';
                        render_input_line(old_len);
                    }
                }
                else if (c == '\t') {
                    ensure_input_buffer(input_len + 1u);
                    size_t old_len = input_len;
                    memmove(&input_buf[cursor_pos + 1], &input_buf[cursor_pos], input_len - cursor_pos + 1);
                    input_buf[cursor_pos] = '\t';
                    cursor_pos++;
                    input_len++;
                    input_buf[input_len] = '\0';
                    render_input_line(old_len);
                }
                /* Printable characters */
                else if (c >= 32 && c < 127) {
                    ensure_input_buffer(input_len + 1u);
                    size_t old_len = input_len;
                    memmove(&input_buf[cursor_pos + 1], &input_buf[cursor_pos], input_len - cursor_pos + 1);
                    input_buf[cursor_pos] = c;
                    cursor_pos++;
                    input_len++;
                    input_buf[input_len] = '\0';
                    render_input_line(old_len);
                }
            }
        }
    }
}

/* Auto scroll when terminal overflows - keep only current screen in RAM */
void terminal_auto_scroll(void) {
    volatile uint16_t *vga = VGA_MEMORY;

    if (prompt_shown) {
        prompt_start_cursor -= VGA_WIDTH;
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