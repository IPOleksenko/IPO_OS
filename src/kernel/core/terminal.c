#include <kernel/terminal.h>

#include <vga.h>
#include <driver/input/keyboard.h>
#include <driver/input/keymap/keymap.h>
#include <driver/input/keymap/dynamic_keymap.h>
#include <kernel/driver.h>
#include <file_system/ipo_fs.h>
#include <kernel/process.h>
#include <memory/kmalloc.h>
#include <system/timer.h>
#include <kernel/async.h>
#include <system/state.h>
#include <driver/sound.h>
#include <driver/ata/ata.h>
#include <ioport.h>
#include <system/run_cmd.h>
#include <wm.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <net/net.h>
#include <net/loopback.h>
#include <kernel/device.h>
#include <kernel/smp.h>
#include <memory/memory_map.h>
#include <driver/net/pci.h>
#include <driver/net/rtl8139.h>
#include <driver/storage/block_dev.h>
#include <driver/storage/partition.h>
#include <storage/storage_pool.h>
#include <kernel/kmod.h>

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
static int command_history_capacity = 0;
static int command_history_count = 0;
static int command_history_index = -1;
static uint32_t s_terminal_init_count = 0;
static char *s_serial_cmd_buf = NULL;
static size_t s_serial_cmd_cap = 0;
static size_t s_serial_cmd_len = 0;
static char *pending_terminal_input = NULL;
static bool pending_terminal_auto_exec = false;
static char *current_input_snapshot = NULL;
static uint8_t last_terminal_scancode = 0;
static uint32_t last_terminal_key_ms = 0;
static uint32_t last_history_action_ms = 0;

static char *terminal_cwd = NULL;

/* Replace terminal_cwd with a heap copy of 'path'.  Never fails silently —
 * on allocation failure the old value is preserved. */
static void terminal_cwd_set(const char *path) {
    if (!path || !*path) path = "/";
    size_t len = strlen(path);
    char *buf = kmalloc(len + 1u);
    if (!buf) return;   /* keep old value on OOM */
    memcpy(buf, path, len + 1u);
    kfree(terminal_cwd);
    terminal_cwd = buf;
}

/* Ensure terminal_cwd is initialised (called once from terminal_init). */
static void terminal_cwd_init(void) {
    if (!terminal_cwd) terminal_cwd_set("/");
}
#define SCROLL_HISTORY_SIZE TERMINAL_SCROLL_HISTORY_SIZE
#define SCROLL_HISTORY_CLEAR_THRESHOLD (SCROLL_HISTORY_SIZE * 3 / 4)
#define SCROLL_HISTORY_KEEP_SIZE (SCROLL_HISTORY_SIZE / 2)
static uint16_t (*terminal_top_buffer)[VGA_WIDTH] = NULL;
static uint16_t (*terminal_bottom_buffer)[VGA_WIDTH] = NULL;

/* Scroll state */
static int top_buffer_count = 0;
static int bottom_buffer_count = 0;

static inline void init_scroll_buffer(uint16_t (*buf)[VGA_WIDTH]) {
    if (!buf) return;
    uint16_t blank = vga_entry(' ', VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    for (size_t i = 0; i < SCROLL_HISTORY_SIZE * VGA_WIDTH; i++) {
        ((uint16_t *)buf)[i] = blank;
    }
}

static inline uint16_t (*active_top_buffer(void))[VGA_WIDTH] {
    if (process_is_batch_active() && process_get_separate_windows()) {
        process_t *fg = process_get_foreground();
        if (fg && fg->scroll_top_buffer) {
            return (uint16_t (*)[VGA_WIDTH])fg->scroll_top_buffer;
        }
    }
    if (!terminal_top_buffer) {
        terminal_top_buffer = (uint16_t (*)[VGA_WIDTH])kmalloc(SCROLL_HISTORY_SIZE * VGA_WIDTH * sizeof(uint16_t));
        if (terminal_top_buffer) {
            init_scroll_buffer(terminal_top_buffer);
        }
    }
    return terminal_top_buffer;
}

static inline int *active_top_count(void) {
    if (process_is_batch_active() && process_get_separate_windows()) {
        process_t *fg = process_get_foreground();
        if (fg) {
            return &fg->scroll_top_count;
        }
    }
    return &top_buffer_count;
}

static inline uint16_t (*active_bottom_buffer(void))[VGA_WIDTH] {
    if (process_is_batch_active() && process_get_separate_windows()) {
        process_t *fg = process_get_foreground();
        if (fg && fg->scroll_bottom_buffer) {
            return (uint16_t (*)[VGA_WIDTH])fg->scroll_bottom_buffer;
        }
    }
    if (!terminal_bottom_buffer) {
        terminal_bottom_buffer = (uint16_t (*)[VGA_WIDTH])kmalloc(SCROLL_HISTORY_SIZE * VGA_WIDTH * sizeof(uint16_t));
        if (terminal_bottom_buffer) {
            init_scroll_buffer(terminal_bottom_buffer);
        }
    }
    return terminal_bottom_buffer;
}

static inline int *active_bottom_count(void) {
    if (process_is_batch_active() && process_get_separate_windows()) {
        process_t *fg = process_get_foreground();
        if (fg) {
            return &fg->scroll_bottom_count;
        }
    }
    return &bottom_buffer_count;
}

void terminal_scroll_up(void);
void terminal_scroll_down(void);

// Terminal drawing area (below header)
static inline uint16_t terminal_top_row(void) {
    return VGA_START_CURSOR_POSITION / VGA_WIDTH;
}

static inline uint16_t terminal_rows(void) {
    return VGA_HEIGHT - terminal_top_row();
}

static char *make_abs_path_dyn(const char *rel_or_abs) {
    size_t cwd_l = terminal_cwd ? strlen(terminal_cwd) : 0u;
    size_t rel_l = rel_or_abs ? strlen(rel_or_abs) : 0u;
    size_t temp_size = cwd_l + rel_l + 32u;
    if (temp_size < 256u) temp_size = 256u;

    char *temp = kmalloc(temp_size);
    if (!temp) return NULL;

    if (!rel_or_abs || rel_or_abs[0] == '\0') {
        strncpy(temp, (terminal_cwd && terminal_cwd[0]) ? terminal_cwd : "/", temp_size - 1u);
        temp[temp_size - 1u] = '\0';
    } else if (rel_or_abs[0] == '/') {
        strncpy(temp, rel_or_abs, temp_size - 1u);
        temp[temp_size - 1u] = '\0';
    } else {
        if (strcmp(terminal_cwd, "/") == 0) {
            snprintf(temp, temp_size, "/%s", rel_or_abs);
        } else {
            snprintf(temp, temp_size, "%s/%s", terminal_cwd, rel_or_abs);
        }
    }

    char *out = kmalloc(temp_size);
    if (!out) {
        kfree(temp);
        return NULL;
    }
    fs_canonicalize(temp, out, temp_size);
    kfree(temp);
    return out;
}

static char *path_combine_and_canonicalize(const char *dir, const char *file) {
    if (!dir) return make_abs_path_dyn(file);
    if (!file) return make_abs_path_dyn(dir);
    size_t needed = strlen(dir) + strlen(file) + 32u;
    char *combined = kmalloc(needed);
    if (!combined) return NULL;
    if (strcmp(dir, "/") == 0) {
        snprintf(combined, needed, "/%s", file);
    } else {
        snprintf(combined, needed, "%s/%s", dir, file);
    }
    char *out = kmalloc(needed);
    if (!out) {
        kfree(combined);
        return NULL;
    }
    fs_canonicalize(combined, out, needed);
    kfree(combined);
    return out;
}

void print_header(void) {
    volatile uint16_t* vga = VGA_MEMORY;

    const char *os_name = "IPO_OS";
    const char *created_by = "Created by IPOleksenko";

    uint8_t os_name_length = (uint8_t)strlen(os_name);
    uint8_t created_by_length = (uint8_t)strlen(created_by);

    for (uint8_t i = 0; i < os_name_length; i++) {
        vga[i] = vga_entry(os_name[i], VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    }

    for (uint8_t i = os_name_length; i < VGA_WIDTH - created_by_length; i++) {
        vga[i] = vga_entry(' ', VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    }

    for (uint8_t i = 0; i < created_by_length; i++) {
        vga[VGA_WIDTH - created_by_length + i] = vga_entry(created_by[i], VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    }

    terminal_render_language_bar();
}

static uint32_t lang_bar_offset = 0;
static uint32_t lang_bar_last_tick_ms = 0;

static void render_bar_string_to_vga(uint16_t row_start, const char *str, size_t str_len) {
    volatile uint16_t *vga = VGA_MEMORY;
    size_t col = 0;
    size_t i = 0;
    while (i < str_len && col < (size_t)VGA_WIDTH) {
        size_t bytes = 1;
        uint8_t glyph = utf8_to_vga_glyph(&str[i], str_len - i, &bytes);
        vga[row_start + col] = vga_entry(glyph, VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        col++;
        i += bytes;
    }
    while (col < (size_t)VGA_WIDTH) {
        vga[row_start + col] = vga_entry(' ', VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        col++;
    }
}

void terminal_render_language_bar(void) {
    uint32_t count = dynamic_keymap_get_enabled_count();
    uint16_t row1_start = VGA_WIDTH;

    /* Display language bar only if there are more than 1 enabled layouts in the system */
    if (count <= 1) {
        volatile uint16_t *vga = VGA_MEMORY;
        for (size_t col = 0; col < (size_t)VGA_WIDTH; col++) {
            vga[row1_start + col] = vga_entry(' ', VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        }
        return;
    }

    const char *lang_name = dynamic_keymap_get_name();
    if (lang_name == NULL || lang_name[0] == '\0') {
        lang_name = "Unknown";
    }
    size_t len = strlen(lang_name);

    if (len + 12 <= (size_t)VGA_WIDTH) {
        /* Fits in 80 columns: display formatted cleanly */
        size_t bar_cap = len + 32u;
        char *bar_text = (char *)kmalloc(bar_cap);
        if (bar_text) {
            int prefix_len = snprintf(bar_text, bar_cap, "Keyboard Layout: %s", lang_name);
            if (prefix_len < 0) prefix_len = 0;
            render_bar_string_to_vga(row1_start, bar_text, (size_t)prefix_len);
            kfree(bar_text);
        }
    } else {
        /* Longer than 80 characters: infinite circular marquee! */
        const char *sep = "   ***   ";
        size_t sep_len = strlen(sep);
        size_t loop_len = len + sep_len;

        char *marquee_buf = (char *)kmalloc(VGA_WIDTH + 1);
        if (marquee_buf) {
            size_t col = 0;
            for (col = 0; col < (size_t)VGA_WIDTH; col++) {
                size_t idx = (lang_bar_offset + col) % loop_len;
                if (idx < len) {
                    marquee_buf[col] = lang_name[idx];
                } else {
                    marquee_buf[col] = sep[idx - len];
                }
            }
            marquee_buf[col] = '\0';
            render_bar_string_to_vga(row1_start, marquee_buf, col);
            kfree(marquee_buf);
        }
    }
}

void terminal_language_bar_tick(void) {
    if (dynamic_keymap_get_enabled_count() <= 1) {
        return;
    }

    const char *lang_name = dynamic_keymap_get_name();
    if (lang_name == NULL) return;
    size_t len = strlen(lang_name);

    if (len + 12 <= (size_t)VGA_WIDTH) {
        return;
    }

    uint32_t now = timer_millis();
    if (now - lang_bar_last_tick_ms < 180u) {
        return;
    }
    lang_bar_last_tick_ms = now;

    const char *sep = "   ***   ";
    size_t loop_len = len + strlen(sep);
    lang_bar_offset = (lang_bar_offset + 1u) % (uint32_t)loop_len;

    terminal_render_language_bar();
}

/* Read a line from VGA screen */
static void read_line_from_vga(uint16_t row, uint16_t *buffer) {
    volatile uint16_t* vga = VGA_MEMORY;
    uint16_t offset = row * VGA_WIDTH;
    
    for (uint16_t col = 0; col < VGA_WIDTH; col++) {
        uint16_t val = vga[offset + col];
        uint8_t ch = val & 0xFF;
        uint8_t attr = (uint8_t)(val >> 8);
        if (ch == VGA_CURSOR_GLYPH_SLOT || ch == 0x00) {
            ch = ' ';
        }
        if (attr == 0 || (attr & 0x0F) == ((attr >> 4) & 0x0F)) {
            attr = 0x07;
        }
        buffer[col] = ((uint16_t)attr << 8) | ch;
    }
}

/* Write a line to VGA screen with preserved colors */
static void write_line_to_vga(uint16_t row, const uint16_t *buffer) {
    volatile uint16_t* vga = VGA_MEMORY;
    uint16_t offset = row * VGA_WIDTH;
    
    for (uint16_t col = 0; col < VGA_WIDTH; col++) {
        uint16_t val = buffer[col];
        uint8_t ch = val & 0xFF;
        uint8_t attr = (uint8_t)(val >> 8);
        if (ch == VGA_CURSOR_GLYPH_SLOT || ch == 0x00) {
            ch = ' ';
        }
        if (attr == 0 || (attr & 0x0F) == ((attr >> 4) & 0x0F)) {
            attr = 0x07;
        }
        vga[offset + col] = ((uint16_t)attr << 8) | ch;
    }
}

static size_t previous_rendered_vis_len = 0;

static size_t terminal_visual_offset(size_t index) {
    size_t vcol = 0;
    for (size_t i = 0; i < index && i < input_len; i++) {
        unsigned char uc = (unsigned char)input_buf[i];
        if ((uc & 0xC0) == 0x80) {
            continue; /* Skip UTF-8 continuation bytes */
        }
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

    /* Draw text */
    size_t vcol = 0;
    size_t i = 0;
    while (i < input_len) {
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
            i++;
        } else {
            unsigned char b1 = (unsigned char)input_buf[i];
            size_t bytes = 1;
            uint8_t glyph = utf8_to_vga_glyph(&input_buf[i], input_len - i, &bytes);

            int32_t offset = input_start_cursor + (int32_t)vcol;
            if (offset >= VGA_START_CURSOR_POSITION && offset < VGA_WIDTH * VGA_HEIGHT) {
                vga[offset] = vga_entry(glyph, INPUT_FG, VGA_COLOR_BLACK);
            }
            vcol += 1;
            i += bytes;
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
    uint16_t blank = vga_entry(' ', VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    int32_t start_clear = prompt_start_cursor >= VGA_START_CURSOR_POSITION ? prompt_start_cursor : VGA_START_CURSOR_POSITION;
    int32_t vis_extra = (int32_t)(previous_rendered_vis_len > input_len ? previous_rendered_vis_len : input_len);
    int32_t end_clear = input_start_cursor + vis_extra + 1;
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
        memset(new_buf, 0, new_capacity);
        if (input_buf != NULL) {
            memcpy(new_buf, input_buf, input_len + 1u);
            kfree(input_buf);
        }
        input_buf = new_buf;
        input_capacity = new_capacity;
    }
}

void terminal_clear_command_history(void) {
    if (command_history != NULL) {
        for (int i = 0; i < command_history_count; i++) {
            if (command_history[i] != NULL) {
                kfree(command_history[i]);
                command_history[i] = NULL;
            }
        }
        kfree(command_history);
        command_history = NULL;
        command_history_capacity = 0;
    }
    command_history_count = 0;
    command_history_index = -1;
}

static void ensure_history_storage(void) {
    if (command_history != NULL) {
        return;
    }

    command_history_capacity = 16;
    command_history = (char **)kmalloc(command_history_capacity * sizeof(char *));
    if (command_history == NULL) {
        command_history_capacity = 0;
        return;
    }

    memset(command_history, 0, command_history_capacity * sizeof(char *));
}

static void load_command_history_from_file(void) {
    ensure_history_storage();
    command_history_index = -1;
}

static void push_command_history(const char *cmd) {
    if (!cmd || !cmd[0]) {
        return;
    }

    ensure_history_storage();
    if (!command_history) {
        return;
    }

    /* Expand history capacity if full */
    if (command_history_count >= command_history_capacity) {
        int new_cap = (command_history_capacity == 0) ? 16 : (command_history_capacity * 2);
        char **new_history = (char **)kmalloc(new_cap * sizeof(char *));
        if (!new_history) {
            return;
        }
        if (command_history && command_history_count > 0) {
            memcpy(new_history, command_history, command_history_count * sizeof(char *));
            kfree(command_history);
        }
        memset(new_history + command_history_count, 0, (new_cap - command_history_count) * sizeof(char *));
        command_history = new_history;
        command_history_capacity = new_cap;
    }

    size_t len = strlen(cmd);
    char *entry = (char *)kmalloc(len + 1u);
    if (!entry) {
        return;
    }
    memcpy(entry, cmd, len + 1u);

    command_history[command_history_count++] = entry;
    command_history_index = -1;
}

static void restore_snapshot_input(void);

static void load_history_command(int index) {
    if (index < 0 || index >= command_history_count) {
        return;
    }

    size_t old_len = input_len;
    ensure_input_buffer(strlen(command_history[index]));
    if (input_buf != NULL) {
        strcpy(input_buf, command_history[index]);
        input_len = strlen(input_buf);
        cursor_pos = input_len;
        render_input_line(old_len);
    }
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
    if (input_buf != NULL) {
        strcpy(input_buf, current_input_snapshot);
        input_len = strlen(input_buf);
        cursor_pos = input_len;
        render_input_line(old_len);
    }
}

static void trim_top_buffer(void) {
    int *top_cnt = active_top_count();
    uint16_t (*top_buf)[VGA_WIDTH] = active_top_buffer();

    if (!top_buf || !top_cnt || *top_cnt < SCROLL_HISTORY_CLEAR_THRESHOLD) {
        return;
    }

    int remove_count = *top_cnt - SCROLL_HISTORY_KEEP_SIZE;
    if (remove_count <= 0) {
        return;
    }

    memmove(
        top_buf,
        &top_buf[remove_count],
        (size_t)SCROLL_HISTORY_KEEP_SIZE * VGA_WIDTH * sizeof(uint16_t)
    );

    *top_cnt = SCROLL_HISTORY_KEEP_SIZE;
}

static void save_top_line(uint16_t row) {
    int *top_cnt = active_top_count();
    uint16_t (*top_buf)[VGA_WIDTH] = active_top_buffer();

    if (!top_buf || !top_cnt) {
        return;
    }

    if (*top_cnt >= SCROLL_HISTORY_SIZE) {
        trim_top_buffer();
    }

    if (*top_cnt < SCROLL_HISTORY_SIZE) {
        read_line_from_vga(row, top_buf[*top_cnt]);
        (*top_cnt)++;
    }
}

/* Scroll down - restore next line from history if available */
void terminal_scroll_down(void) {
    int *bot_cnt = active_bottom_count();
    int *top_cnt = active_top_count();
    uint16_t (*bot_buf)[VGA_WIDTH] = active_bottom_buffer();
    uint16_t (*top_buf)[VGA_WIDTH] = active_top_buffer();

    if (!bot_buf || !top_buf || !bot_cnt || !top_cnt || *bot_cnt == 0) {
        return;
    }

    volatile uint16_t *vga = VGA_MEMORY;

    vga_cursor_erase();

    uint16_t top = terminal_top_row();
    uint16_t rows = terminal_rows();

    if (rows <= 1) {
        return;
    }

    if (prompt_shown) {
        prompt_start_cursor -= VGA_WIDTH;
        input_start_cursor -= VGA_WIDTH;
    }

    if (*top_cnt < SCROLL_HISTORY_SIZE) {
        read_line_from_vga(top, top_buf[*top_cnt]);
        (*top_cnt)++;
    }

    for (uint16_t r = 0; r < rows - 1; r++) {
        uint16_t src_offset = (top + r + 1) * VGA_WIDTH;
        uint16_t dst_offset = (top + r) * VGA_WIDTH;

        for (uint16_t c = 0; c < VGA_WIDTH; c++) {
            uint16_t val = vga[src_offset + c];
            if ((val & 0xFF) == VGA_CURSOR_GLYPH_SLOT) {
                val = ((val >> 8) << 8) | ' ';
            }
            vga[dst_offset + c] = val;
        }
    }

    write_line_to_vga(top + rows - 1, bot_buf[*bot_cnt - 1]);
    (*bot_cnt)--;

    if (process_is_batch_active() && process_get_separate_windows()) {
        process_t *fg = process_get_foreground();
        if (fg && fg->text_vram_backup) {
            memcpy(fg->text_vram_backup, (const void *)vga, 80 * 25 * sizeof(uint16_t));
        }
    }

    if (*bot_cnt == 0) {
        vga_show_cursor();
    }
}

/* Return to present - restore current output when user starts typing */
void terminal_return_to_present(void) {
    while (*active_bottom_count() > 0) {
        terminal_scroll_down();
    }
    vga_show_cursor();
}

int terminal_get_top_buffer_count(void) {
    return *active_top_count();
}

int terminal_get_bottom_buffer_count(void) {
    return *active_bottom_count();
}

/* Scroll up - show previous line from history */
void terminal_scroll_up(void) {
    int *top_cnt = active_top_count();
    int *bot_cnt = active_bottom_count();
    uint16_t (*top_buf)[VGA_WIDTH] = active_top_buffer();
    uint16_t (*bot_buf)[VGA_WIDTH] = active_bottom_buffer();

    if (!top_buf || !bot_buf || !top_cnt || !bot_cnt || *top_cnt == 0) {
        return;
    }

    volatile uint16_t *vga = VGA_MEMORY;

    vga_cursor_erase();

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

    if (*bot_cnt < SCROLL_HISTORY_SIZE) {
        read_line_from_vga(top + rows - 1, bot_buf[*bot_cnt]);
        (*bot_cnt)++;
    }

    for (uint16_t r = rows - 1; r > 0; r--) {
        uint16_t src_offset = (top + r - 1) * VGA_WIDTH;
        uint16_t dst_offset = (top + r) * VGA_WIDTH;

        for (uint16_t c = 0; c < VGA_WIDTH; c++) {
            uint16_t val = vga[src_offset + c];
            if ((val & 0xFF) == VGA_CURSOR_GLYPH_SLOT) {
                val = ((val >> 8) << 8) | ' ';
            }
            vga[dst_offset + c] = val;
        }
    }

    write_line_to_vga(top, top_buf[*top_cnt - 1]);
    (*top_cnt)--;

    if (process_is_batch_active() && process_get_separate_windows()) {
        process_t *fg = process_get_foreground();
        if (fg && fg->text_vram_backup) {
            memcpy(fg->text_vram_backup, (const void *)vga, 80 * 25 * sizeof(uint16_t));
        }
    }
}

const char *terminal_get_cwd(void) {
    if (!terminal_cwd) terminal_cwd_init();
    return terminal_cwd ? terminal_cwd : "/";
}

int terminal_set_cwd(const char *path) {
    if (!path || !*path) return -1;
    char *target = make_abs_path_dyn(path);
    if (!target) return -1;
    uint32_t ino;
    struct ipo_inode st;
    if (path_resolve(target, &ino) < 0 || !ipo_fs_stat(target, &st) || (st.mode & IPO_INODE_TYPE_DIR) == 0) {
        kfree(target);
        return -1;
    }
    terminal_cwd_set(target);
    kfree(target);
    return 0;
}

char* resolve_command_path(const char *cmd) {
    if (!cmd || !cmd[0]) return NULL;

    size_t cmd_len = strlen(cmd);
    size_t cwd_len = terminal_cwd ? strlen(terminal_cwd) : 0u;
    size_t buf_size = cmd_len + cwd_len + 64u;
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
    } 
    // Relative path with ./ or ../ or subdirectories
    else if (cmd[0] == '.' || strchr(cmd, '/')) {
        char *abs = make_abs_path_dyn(cmd);
        if (abs) {
            strncpy(to_check, abs, buf_size - 1);
            to_check[buf_size - 1] = '\0';
            kfree(abs);
        } else {
            to_check[0] = '\0';
        }
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
    }
    // Simple command name: search in order: cwd, /app
    else {
        static const char * const search_paths[] = {
            NULL, /* indicates cwd */
            "/applications",
            "/app",
            NULL
        };

        for (int i = 0; search_paths[i] != NULL || i == 0; i++) {
            if (search_paths[i] == NULL && i == 0) {
                char *abs = make_abs_path_dyn(cmd);
                if (abs) {
                    strncpy(to_check, abs, buf_size - 1);
                    to_check[buf_size - 1] = '\0';
                    kfree(abs);
                } else {
                    to_check[0] = '\0';
                }
            } else if (search_paths[i] != NULL) {
                snprintf(to_check, buf_size, "%s/%s", search_paths[i], cmd);
            } else {
                break;
            }

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
        }
    }

    kfree(to_check);
    kfree(canonical);
    kfree(path);
    return NULL;
}

bool terminal_is_builtin(const char *name) {
    if (!name || !*name) return false;
    static const char * const builtins[] = {
        "help",
        "clear",
        "pwd",
        "cd",
        "ls",
        "cat",
        "touch",
        "mkdir",
        "rm",
        "cp",
        "mv",
        "stat",
        "echo",
        "reboot",
        "shutdown",
        "reinstall_fs",
        "ps",
        "kill",
        "killall",
        "run",
        "df",
        "free",
        "drivers",
        "symbols",
        "storage",
        "keymap",
        "return",
        "ifconfig",
        "hardware",
        "startwm",
        "stopwm",
        NULL
    };
    for (int i = 0; builtins[i] != NULL; i++) {
        if (strcmp(name, builtins[i]) == 0) return true;
    }
    return false;
}

static void builtin_help(void) {
    printf("======================= IPO_OS SYSTEM HELP =======================\n");
    printf("\n");
    printf("1. BUILTIN SHELL COMMANDS & SYNTAX\n");
    printf("  [Navigation & Paths]\n");
    printf("    pwd\n");
    printf("      - Print absolute path of current working directory.\n");
    printf("    cd [path]\n");
    printf("      - Change working directory to target path.\n");
    printf("\n");
    printf("  [File & Directory Operations]\n");
    printf("    ls [path]\n");
    printf("      - List directory contents showing item type and file size.\n");
    printf("    cat <file...>\n");
    printf("      - Display contents of one or multiple files sequentially.\n");
    printf("    touch <file...>\n");
    printf("      - Create one or multiple new empty files.\n");
    printf("    mkdir <dir...>\n");
    printf("      - Create one or multiple new directories.\n");
    printf("    rm <path...>\n");
    printf("      - Delete one or multiple files or empty directories.\n");
    printf("    cp <src> <dst>\n");
    printf("      - Copy file to destination path or into destination directory.\n");
    printf("    mv <src> <dst>\n");
    printf("      - Move or rename a file or directory.\n");
    printf("    stat <path...>\n");
    printf("      - Display inode number, type, size, protection and extents info.\n");
    printf("\n");
    printf("  [Process & Task Management]\n");
    printf("    ps\n");
    printf("      - List all active and background processes with PID, state, memory.\n");
    printf("    kill <pid>\n");
    printf("      - Terminate a running or background process and free its resources.\n");
    printf("    killall\n");
    printf("      - Terminate all active processes and stop background async tasks.\n");
    printf("    run <prog> [args] [OP <prog> [args] ...]\n");
    printf("      - Launch executables with multitasking batch operators:\n");
    printf("          &   - run each program in a separate workspace\n");
    printf("                (separate text buffers / separate startwm workspaces).\n");
    printf("          &&  - run programs asynchronously in a single shared workspace\n");
    printf("                (main terminal / same startwm desktop with all windows open).\n");
    printf("          |   - wait for left program to complete and pass return code to right.\n");
    printf("          ;   - wait until program completes, then run next program.\n");
    printf("        Switch workspaces: Ctrl + Page Up / Ctrl + Page Down; exit: Ctrl + C.\n");
    printf("    startwm\n");
    printf("      - Enter VGA Mode 13h (320x200) and start the window manager compositor\n");
    printf("        as a background async task. Returns to the shell immediately.\n");
    printf("        The [Close] button in the bottom-left of the screen always exits.\n");
    printf("    stopwm\n");
    printf("      - Stop the WM compositor, destroy all windows, return to text mode.\n");
    printf("\n");
    printf("  [Output & Redirection]\n");
    printf("    echo [text] [> file | >> file]\n");
    printf("      - Print text to stdout, overwrite to file (>), or append (>>).\n");
    printf("\n");
    printf("  [Networking]\n");
    printf("    ifconfig [<new_ip> [netmask] [gateway] [dns]]\n");
    printf("      - Query or configure device IP address, netmask, gateway and MAC.\n");
    printf("\n");
    printf("  [Drivers & Kernel Modules]\n");
    printf("    drivers [list | info <id/name> | load <path> | unload <id/name> | status | symbols [filter]]\n");
    printf("      - Manage kernel drivers and dynamic runtime modules (KMOD ABI 0x0100).\n");
    printf("    symbols [filter]\n");
    printf("      - Inspect all exported kernel function symbols, memory addresses, and OS entry points.\n");
    printf("\n");
    printf("  [Hardware Resources & Infrastructure]\n");
    printf("    hardware [overview | list | cpu | memory | pci | storage | network | usb | refresh | info <id> | status]\n");
    printf("      - Inspect hardware resources scanned directly from hardware buses & tables:\n");
    printf("          overview (default) : Summary of CPU, RAM, PCI, ATA storage, network, and device registry.\n");
    printf("          list               : Table of all discovered hardware & ACPI devices with IDs and resources.\n");
    printf("          cpu                : CPU vendor/brand/features, logical cores table, and SMP status.\n");
    printf("          memory             : BIOS E820 physical memory map and managed memory pools.\n");
    printf("          pci                : Enumerated PCI bus devices (B:S.F, Vendor:Device, Class:Subclass, IRQ).\n");
    printf("          storage            : Detected ATA/IDE drives (model, serial, firmware, capacity) and pool size.\n");
    printf("          network            : Physical PCI network controllers (MAC, link, speed) and resources.\n");
    printf("          usb                : Discovered PCI USB host controllers and resources.\n");
    printf("          refresh            : Re-enumerate hardware buses (SMP, E820, PCI, ATA, ACPI) and rebuild registry.\n");
    printf("          info <id>          : Detailed hardware resource report for device <id> from 'hardware list'.\n");
    printf("          status             : System hardware health and device count breakdown by state and type.\n");
    printf("    storage [list | status | usage | check | create [uuid] [dev] | add <dev> | recover | save]\n");
    printf("      - Manage multi-disk storage pool, block devices, partitions, superblocks, and WAL journal:\n");
    printf("          list (default)     : Show storage pool status and scan all physical block devices & partitions.\n");
    printf("          status             : Show unified storage pool state, superblock LBAs, WAL journal, and members.\n");
    printf("          usage              : Show volume space usage, active filesystem stats, and member drive breakdown.\n");
    printf("          check              : Verify sector 0, primary/backup superblock CRCs, partitions, and WAL journal.\n");
    printf("          create [uuid] [dev]: Create a persistent storage pool with optional UUID on target block device.\n");
    printf("          add <dev>          : Attach a physical block device (by hardware serial/name or index) to the pool.\n");
    printf("          recover            : Scan on-disk WAL journal and resolve uncommitted/interrupted transactions.\n");
    printf("          save               : Synchronize primary and backup superblock metadata to all online member disks.\n");
    printf("\n");
    printf("  [Disk & Memory Info]\n");
    printf("    df\n");
    printf("      - Show storage pool capacity: total, used, free blocks, inodes, drives.\n");
    printf("    free\n");
    printf("      - Show kernel heap stats: total, used, free, block count, overhead.\n");
    printf("    keymap [list | on <id/name> | off <id/name> | rm <id/name>]\n");
    printf("      - Manage keyboard layouts: list registered layouts, enable (on), disable (off), or remove (rm).\n");
    printf("    return [on | off]\n");
    printf("      - Toggle display of process exit return value (Return value: X).\n");
    printf("\n");
    printf("  [System & Power Controls]\n");
    printf("    clear\n");
    printf("      - Clear terminal screen and reset VGA scrollback buffer.\n");
    printf("    reboot\n");
    printf("      - Reboot hardware via 8042 controller, port 0x92 or triple fault.\n");
    printf("    shutdown\n");
    printf("      - Power off machine via ACPI/APM or halt CPU execution safely.\n");
    printf("    reinstall_fs\n");
    printf("      - Completely reinstall filesystem on disk (wipes FS and resets to defaults).\n");
    printf("    help\n");
    printf("      - Display this full system documentation and command reference.\n");
    printf("\n");
    printf("2. FILE & DIRECTORY PATH SYNTAX\n");
    printf("  IPO_OS uses unified path resolution across all shell commands and applications:\n");
    printf("  - Absolute paths : Start with '/'.\n");
    printf("  - Relative paths : Relative to current working directory.\n");
    printf("  - Current dir    : '.' refers to current directory.\n");
    printf("  - Parent dir     : '..' ascends to parent directory.\n");
    printf("  - Root           : '~' represents the root directory '/'.\n");
    printf("  - Redundant '/'  : Multiple slashes '//', '///' are automatically collapsed to a single '/'.\n");
    printf("  - Paths w/spaces : Wrap path in quotes or escape with '\\'.\n");
    printf("\n");
    printf("3. APPLICATION EXECUTION (Launch Methods & Resolution)\n");
    printf("  IPO_OS executes Position Independent Executables via three methods:\n");
    printf("  [Method 1: Direct Name (Automatic PATH Resolution)]\n");
    printf("    Syntax: <command> [arguments...]\n");
    printf("    - Searches current directory, /applications directory, then root directory.\n");
    printf("  [Method 2: Explicit Path]\n");
    printf("    Syntax: <path/to/application> [arguments...]\n");
    printf("    - Execute application specifying a path.\n");
    printf("  [Method 3: Batch Startup Script]\n");
    printf("    - Reads commands line-by-line from '/autorun' during kernel boot.\n");
    printf("\n");
    printf("4. TERMINAL SHORTCUTS & KEYBOARD CONTROLS\n");
    printf("   - Ctrl + Page Up         : Switch to previous multitasking workspace.\n");
    printf("   - Ctrl + Page Down       : Switch to next multitasking workspace.\n");
    printf("   - Ctrl + Left/Right Shift: Cycle keyboard layout backwards / forwards.\n");
    printf("   - Alt + Left/Right Shift : Cycle keyboard layout backwards / forwards.\n");
    printf("   - Ctrl + C               : Interrupt active task or exit a finished workspace.\n");
    printf("   - Page Up / Page Down    : Scroll terminal output up / down.\n");
    printf("   - Up / Down Arrows       : Command history navigation.\n");
    printf("   - Left / Right Arrows    : Move cursor across current line.\n");
    printf("   - Home / End             : Move cursor to beginning / end of line.\n");
    printf("\n");
    printf("5. WINDOW MANAGER SHORTCUTS & CONTROLS\n");
    printf("   - Tab                    : Switch focus to next window.\n");
    printf("   - Shift + Tab            : Switch focus to previous window.\n");
    printf("   - Ctrl + W               : Close currently focused window.\n");
    printf("   - Ctrl + Arrow Keys      : Move focused window.\n");
    printf("   - Ctrl + Page Up         : Switch to previous multitasking workspace.\n");
    printf("   - Ctrl + Page Down       : Switch to next multitasking workspace.\n");
    printf("   - F2                     : Cycle active keyboard layout.\n");
    printf("   - Ctrl + Left/Right Shift: Cycle keyboard layout backwards / forwards.\n");
    printf("   - Alt + Left/Right Shift : Cycle keyboard layout backwards / forwards.\n");
    printf("   - Window Buttons         : Minimize, Maximize/Restore, Close [X].\n");
    printf("   - Window Titlebar Drag   : Move window.\n");
    printf("   - Window Border Drag     : Resize window.\n");
    printf("   - Taskbar [Close]        : Exit foreground application or stop WM.\n");
    printf("   - Taskbar Tabs           : Click to focus/restore/minimize; [x] or right-click to close.\n");
    printf("   - Taskbar Language Box   : Left click: next layout; right click: previous layout.\n");
    printf("\n");
    printf("6. SYSTEM STATUS & LANGUAGE BAR\n");
    printf("  [Top Language Bar Operation]\n");
    printf("    - Status bar displays currently active keyboard layout below system header.\n");
    printf("    - Automatically hidden when only one layout is enabled in the system.\n");
    printf("    - Automatically visible when two or more layouts are enabled.\n");
    printf("    - Updates dynamically in real time upon switching or enabling/disabling layouts.\n");
}

void builtin_clear(void) {
    vga_clear(
        VGA_COLOR_WHITE,
        VGA_COLOR_BLACK,
        true,
        VGA_START_CURSOR_POSITION
    );
    print_header();

    if (terminal_top_buffer) {
        init_scroll_buffer(terminal_top_buffer);
    }
    if (terminal_bottom_buffer) {
        init_scroll_buffer(terminal_bottom_buffer);
    }

    uint16_t (*top_buf)[VGA_WIDTH] = active_top_buffer();
    uint16_t (*bot_buf)[VGA_WIDTH] = active_bottom_buffer();
    if (top_buf && top_buf != terminal_top_buffer) {
        init_scroll_buffer(top_buf);
    }
    if (bot_buf && bot_buf != terminal_bottom_buffer) {
        init_scroll_buffer(bot_buf);
    }

    *active_top_count() = 0;
    *active_bottom_count() = 0;
    top_buffer_count = 0;
    bottom_buffer_count = 0;
    input_start_cursor = VGA_START_CURSOR_POSITION;
    prompt_start_cursor = VGA_START_CURSOR_POSITION;
    cursor_pos = 0;
    previous_rendered_vis_len = 0;
}

void terminal_clear_buffer(void) {
    builtin_clear();
    prompt_shown = false;
    input_len = 0;
    cursor_pos = 0;
    if (input_buf != NULL) {
        input_buf[0] = '\0';
    }
}

static void builtin_pwd(void) {
    printf("%s\n", terminal_cwd);
}

static void builtin_cd(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "~") == 0) {
        terminal_cwd_set("/");
        return;
    }
    char *combined_arg = NULL;
    const char *path_arg = argv[1];
    if (argc > 2) {
        size_t total_len = 0;
        for (int i = 1; i < argc; i++) {
            total_len += strlen(argv[i]) + 1;
        }
        combined_arg = (char *)kmalloc(total_len + 1);
        if (combined_arg) {
            combined_arg[0] = '\0';
            for (int i = 1; i < argc; i++) {
                if (i > 1) strcat(combined_arg, " ");
                strcat(combined_arg, argv[i]);
            }
            path_arg = combined_arg;
        }
    }
    char *target = make_abs_path_dyn(path_arg);
    if (!target) {
        if (combined_arg) kfree(combined_arg);
        printf("cd: out of memory\n");
        return;
    }
    struct ipo_inode stat;
    if (!ipo_fs_stat(target, &stat)) {
        printf("cd: %s: No such directory\n", path_arg);
        if (combined_arg) kfree(combined_arg);
        kfree(target);
        return;
    }
    if ((stat.mode & IPO_INODE_TYPE_DIR) == 0) {
        printf("cd: %s: Not a directory\n", path_arg);
        if (combined_arg) kfree(combined_arg);
        kfree(target);
        return;
    }
    terminal_cwd_set(target);
    if (combined_arg) kfree(combined_arg);
    kfree(target);
}

static void builtin_ls(int argc, char **argv) {
    char *target;
    if (argc >= 2) {
        target = make_abs_path_dyn(argv[1]);
    } else {
        target = make_abs_path_dyn(terminal_cwd ? terminal_cwd : "/");
    }
    if (!target) { printf("ls: out of memory\n"); return; }

    uint32_t ino;
    if (path_resolve(target, &ino) < 0) {
        printf("ls: cannot access '%s': No such file or directory\n", target);
        kfree(target);
        return;
    }
    struct ipo_inode din;
    if (!read_inode(ino, &din)) {
        printf("ls: cannot read inode for '%s'\n", target);
        kfree(target);
        return;
    }
    if ((din.mode & IPO_INODE_TYPE_DIR) == 0) {
        printf("  [FILE] %s  (%u B)\n", target, din.size);
        kfree(target);
        return;
    }
    kfree(target);

    uint64_t offset = 0;
    while (offset < din.size) {
        struct ipo_dir_entry hdr;
        if (inode_read_bytes(&din, &hdr, sizeof(hdr), offset) != (int)sizeof(hdr)) break;
        if (hdr.rec_len < sizeof(hdr)) break;
        if (hdr.inode != 0) {
            char *name = (char *)kmalloc(hdr.name_len + 1);
            if (name) {
                if (inode_read_bytes(&din, name, hdr.name_len, offset + sizeof(hdr)) == (int)hdr.name_len) {
                    name[hdr.name_len] = '\0';
                    struct ipo_inode ein;
                    uint32_t fsize = 0;
                    if (read_inode(hdr.inode, &ein)) {
                        fsize = (uint32_t)ein.size;
                    }
                    if (hdr.type == IPO_INODE_TYPE_DIR) {
                        printf("  [DIR]  %s/\n", name);
                    } else {
                        printf("  [FILE] %s  (%u B)\n", name, fsize);
                    }
                }
                kfree(name);
            }
        }
        offset += hdr.rec_len;
    }
}

static void builtin_cat(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: cat <file>\n");
        return;
    }
    for (int a = 1; a < argc; a++) {
        char *target = make_abs_path_dyn(argv[a]);
        if (!target) continue;

        struct ipo_inode stat;
        if (!ipo_fs_stat(target, &stat)) {
            printf("cat: %s: No such file or directory\n", argv[a]);
            kfree(target);
            continue;
        }
        if ((stat.mode & IPO_INODE_TYPE_DIR) != 0) {
            printf("cat: %s: Is a directory\n", argv[a]);
            kfree(target);
            continue;
        }

        int fd = ipo_fs_open(target);
        if (fd < 0) {
            printf("cat: %s: Failed to open\n", argv[a]);
            kfree(target);
            continue;
        }

        size_t chunk_cap = 4096;
        char *chunk = (char *)kmalloc(chunk_cap + 1);
        if (!chunk) {
            ipo_fs_close(fd);
            kfree(target);
            continue;
        }

        uint32_t offset = 0;
        while (offset < stat.size) {
            uint32_t to_read = (uint32_t)chunk_cap;
            if (offset + to_read > stat.size) {
                to_read = stat.size - offset;
            }
            int read_bytes = ipo_fs_read(fd, chunk, to_read, offset);
            if (read_bytes <= 0) break;
            chunk[read_bytes] = '\0';
            for (int i = 0; i < read_bytes; i++) {
                putchar(chunk[i]);
            }
            offset += (uint32_t)read_bytes;
        }
        kfree(chunk);
        ipo_fs_close(fd);
        kfree(target);
    }
}

static void builtin_touch(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: touch <file>\n");
        return;
    }
    for (int a = 1; a < argc; a++) {
        char *target = make_abs_path_dyn(argv[a]);
        if (!target) continue;

        struct ipo_inode stat;
        if (ipo_fs_stat(target, &stat)) {
            kfree(target);
            continue;
        }

        int ino = ipo_fs_create(target, IPO_INODE_TYPE_FILE);
        if (ino < 0) {
            printf("touch: cannot create '%s'\n", argv[a]);
        }
        kfree(target);
    }
}

static void builtin_mkdir(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: mkdir <directory>\n");
        return;
    }
    for (int a = 1; a < argc; a++) {
        char *target = make_abs_path_dyn(argv[a]);
        if (!target) continue;

        int ino = ipo_fs_create(target, IPO_INODE_TYPE_DIR);
        if (ino < 0) {
            printf("mkdir: cannot create directory '%s'\n", argv[a]);
        }
        kfree(target);
    }
}

static void builtin_rm(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: rm <path>\n");
        return;
    }
    for (int a = 1; a < argc; a++) {
        char *target = make_abs_path_dyn(argv[a]);
        if (!target) continue;

        struct ipo_inode stat;
        if (!ipo_fs_stat(target, &stat)) {
            printf("rm: cannot remove '%s': No such file or directory\n", argv[a]);
            kfree(target);
            continue;
        }

        if (!ipo_fs_delete(target)) {
            printf("rm: cannot remove '%s': Protected or non-empty directory\n", argv[a]);
        }
        kfree(target);
    }
}

static void builtin_cp(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: cp <source> <dest>\n");
        return;
    }
    char *src = make_abs_path_dyn(argv[1]);
    char *dst = make_abs_path_dyn(argv[2]);
    if (!src || !dst) {
        if (src) kfree(src);
        if (dst) kfree(dst);
        return;
    }

    struct ipo_inode src_stat;
    if (!ipo_fs_stat(src, &src_stat)) {
        printf("cp: cannot stat '%s': No such file\n", argv[1]);
        kfree(src);
        kfree(dst);
        return;
    }
    if ((src_stat.mode & IPO_INODE_TYPE_DIR) != 0) {
        printf("cp: copying directories is not supported\n");
        kfree(src);
        kfree(dst);
        return;
    }

    struct ipo_inode dst_stat;
    if (ipo_fs_stat(dst, &dst_stat) && (dst_stat.mode & IPO_INODE_TYPE_DIR) != 0) {
        const char *slash = strrchr(src, '/');
        const char *filename = slash ? (slash + 1) : src;
        char *expanded_dst = path_combine_and_canonicalize(dst, filename);
        if (expanded_dst) {
            kfree(dst);
            dst = expanded_dst;
        }
    }

    int src_fd = ipo_fs_open(src);
    if (src_fd < 0) {
        printf("cp: cannot open source '%s'\n", argv[1]);
        kfree(src);
        kfree(dst);
        return;
    }

    if (ipo_fs_stat(dst, &dst_stat)) {
        if (!ipo_fs_delete(dst)) {
            printf("cp: cannot overwrite destination '%s'\n", argv[2]);
            ipo_fs_close(src_fd);
            kfree(src);
            kfree(dst);
            return;
        }
    }

    int dst_ino = ipo_fs_create(dst, IPO_INODE_TYPE_FILE);
    if (dst_ino < 0) {
        printf("cp: cannot create destination '%s'\n", argv[2]);
        ipo_fs_close(src_fd);
        kfree(src);
        kfree(dst);
        return;
    }

    int dst_fd = ipo_fs_open(dst);
    if (dst_fd < 0) {
        printf("cp: cannot open destination '%s'\n", argv[2]);
        ipo_fs_close(src_fd);
        kfree(src);
        kfree(dst);
        return;
    }

    size_t chunk_cap = 4096;
    uint8_t *chunk = (uint8_t *)kmalloc(chunk_cap);
    if (!chunk) {
        ipo_fs_close(src_fd);
        ipo_fs_close(dst_fd);
        kfree(src);
        kfree(dst);
        return;
    }

    uint32_t offset = 0;
    while (offset < src_stat.size) {
        uint32_t to_copy = (uint32_t)chunk_cap;
        if (offset + to_copy > src_stat.size) to_copy = src_stat.size - offset;
        int rb = ipo_fs_read(src_fd, chunk, to_copy, offset);
        if (rb <= 0) break;
        int wb = ipo_fs_write(dst_fd, chunk, rb, offset);
        if (wb != rb) {
            printf("cp: error writing destination '%s'\n", argv[2]);
            break;
        }
        offset += (uint32_t)rb;
    }

    kfree(chunk);
    ipo_fs_close(src_fd);
    ipo_fs_close(dst_fd);
    kfree(src);
    kfree(dst);
}

static void builtin_mv(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: mv <source> <dest>\n");
        return;
    }
    char *src = make_abs_path_dyn(argv[1]);
    char *dst = make_abs_path_dyn(argv[2]);
    if (!src || !dst) {
        if (src) kfree(src);
        if (dst) kfree(dst);
        return;
    }

    struct ipo_inode dst_stat;
    if (ipo_fs_stat(dst, &dst_stat) && (dst_stat.mode & IPO_INODE_TYPE_DIR) != 0) {
        const char *slash = strrchr(src, '/');
        const char *filename = slash ? (slash + 1) : src;
        char *expanded_dst = path_combine_and_canonicalize(dst, filename);
        if (expanded_dst) {
            kfree(dst);
            dst = expanded_dst;
        }
    }

    if (!ipo_fs_rename(src, dst)) {
        printf("mv: failed to move '%s' to '%s'\n", argv[1], argv[2]);
    }

    kfree(src);
    kfree(dst);
}

static void builtin_stat(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: stat <path>\n");
        return;
    }
    for (int a = 1; a < argc; a++) {
        char *target = make_abs_path_dyn(argv[a]);
        if (!target) continue;

        uint32_t inode_num = 0;
        if (path_resolve(target, &inode_num) < 0) {
            printf("stat: cannot resolve '%s'\n", argv[a]);
            kfree(target);
            continue;
        }

        struct ipo_inode st;
        if (!read_inode(inode_num, &st)) {
            printf("stat: cannot read inode %u\n", inode_num);
            kfree(target);
            continue;
        }

        printf("  Path:      %s\n", target);
        printf("  Protected: %s\n", (st.mode & IPO_INODE_FLAG_PROTECTED) ? "Yes" : "No");
        printf("  Size:      %u bytes\n", st.size);
        printf("  Links:     %u\n", st.links_count);
        kfree(target);
    }
}

static void builtin_echo(int argc, char **argv) {
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

static void builtin_reinstall_fs(void) {
    if (!fs_mounted) {
        printf("reinstall_fs: no filesystem mounted\n");
        return;
    }

    printf("=== Reinstalling Filesystem (IPO_FS) ===\n");
    printf("WARNING: This will completely erase all data on the filesystem!\n");
    printf("System directories (/applications, /autorun) will be re-initialized.\n\n");

    /* 1. Kill all running processes */
    printf("[1/5] Terminating all processes...\n");
    process_kill_all();

    /* 2. Close all open file descriptors */
    printf("[2/5] Closing all file descriptors...\n");
    if (fds) {
        for (uint32_t i = 0; i < fds_capacity; i++) {
            if (fds[i].used && i >= 3) {
                fds[i].used = 0;
                fds[i].inode = 0;
                fds[i].offset = 0;
                fds[i].flags = 0;
            }
        }
    }

    /* 3. Compute format parameters (same logic as kernel boot ensure_fs_mounted) */
    printf("[3/5] Computing filesystem parameters...\n");

    uint64_t pool_capacity = ata_get_pool_capacity();
    if (pool_capacity == 0) {
        printf("reinstall: disk not detected\n");
        return;
    }
    if (pool_capacity < 10) {
        printf("reinstall: disk too small for filesystem (< 5 KB)\n");
        return;
    }

    /* Use the current fs_start_lba if the filesystem was already mounted,
       otherwise fall back to the adaptive partition offset logic. */
    uint64_t start_lba = fs_start_lba;
    uint64_t available_sectors = pool_capacity;

    if (start_lba > 0 && start_lba < pool_capacity) {
        available_sectors = pool_capacity - start_lba;
    } else {
        /* Adaptive partition offset (same as kernel boot):
         * - Disks > 1 MB use standard partition offset 2048
         * - Disks <= 1 MB use LBA 0 (whole-disk / floppy mode) */
        start_lba = 0;
        if (pool_capacity > 2048 + 10) {
            start_lba = 2048;
            available_sectors = pool_capacity - 2048;
        }
    }

    uint64_t total_blocks = available_sectors;
    uint64_t total_inodes = total_blocks / 16;
    if (total_inodes == 0) total_inodes = 1;

    printf("  Storage Pool : %u sectors\n", (uint32_t)pool_capacity);
    printf("  Start LBA    : %u\n", (uint32_t)start_lba);
    printf("  Total Blocks : %u\n", (uint32_t)total_blocks);
    printf("  Total Inodes : %u\n", (uint32_t)total_inodes);

    /* 4. Unmount, format, remount */
    printf("[4/5] Formatting filesystem...\n");
    fs_mounted = false;

    if (!ipo_fs_format(start_lba, total_blocks, total_inodes)) {
        printf("reinstall_fs: ipo_fs_format FAILED\n");
        printf("WARNING: Filesystem is now unmounted. Reboot recommended.\n");
        return;
    }

    if (!ipo_fs_mount(start_lba)) {
        printf("reinstall_fs: ipo_fs_mount FAILED after format\n");
        printf("WARNING: Filesystem formatted but not mounted. Reboot recommended.\n");
        return;
    }

    /* 5. Reset terminal state */
    printf("[5/5] Resetting terminal state...\n");
    terminal_cwd_set("/");

    printf("\n=== Filesystem Reinstallation Complete ===\n");
    printf("Fresh IPO_FS successfully reinstalled at LBA %u.\n", (uint32_t)start_lba);
    printf("  %u blocks, %u inodes available.\n",
           (uint32_t)total_blocks, (uint32_t)total_inodes);
    printf("Note: /applications and /autorun have been recreated.\n");
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
    uint32_t data_start = (uint32_t)sb.data_blocks_start;
    uint32_t total = (sb.fs_size_blocks > data_start)
                     ? (uint32_t)(sb.fs_size_blocks - data_start) : 0;
    *out_total = total;
    if (total == 0) return;

    *out_used = bitmap_count_set(sb.block_bitmap_start, total);
}

/* ---------------------------------------------------------------
 * Helper: count used/total inodes by scanning the inode bitmap
 * --------------------------------------------------------------- */
static void fs_count_inodes(uint32_t *out_used, uint32_t *out_total) {
    *out_total = (uint32_t)sb.inode_count;
    *out_used  = 0;
    if (!fs_mounted || sb.inode_count == 0) return;

    *out_used = bitmap_count_set(sb.inode_bitmap_start, sb.inode_count);
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

    uint64_t pool_cap = ata_get_pool_capacity();
    uint32_t dev_cnt = ata_get_device_count();

    printf("Storage Pool : %u physical ATA drive(s), %u sectors pool capacity\n",
           dev_cnt, (uint32_t)pool_cap);
    printf("Filesystem   : IPO_FS\n");
    printf("Block size   : %u bytes\n", bs);
    printf("Blocks       : %u total, %u used, %u free\n",
           blk_total, blk_used, blk_free);
    printf("Space        : %u KB total, %u KB used, %u KB free  (%u%% used)\n",
           total_kb, used_kb, free_kb, used_pct);
    printf("Inodes       : %u total, %u used, %u free\n",
           ino_total, ino_used,
           (ino_total >= ino_used) ? ino_total - ino_used : 0);
    printf("Partition    : Start LBA %u, Total %u blocks (%u KB)\n",
           (uint32_t)fs_start_lba, (uint32_t)sb.fs_size_blocks, (uint32_t)((sb.fs_size_blocks * bs) / 1024));
}

static void builtin_keymap(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: keymap [list | on <id/name> | off <id/name> | rm <id/name>]\n");
        return;
    }

    if (strcmp(argv[1], "list") == 0) {
        uint32_t total = dynamic_keymap_get_count();
        uint32_t active = dynamic_keymap_get_active_index();
        printf("=== Registered Keyboard Layouts (%u) ===\n", total);
        for (uint32_t i = 0; i < total; i++) {
            const char *name = dynamic_keymap_get_slot_name(i);
            bool en = dynamic_keymap_is_slot_enabled(i);
            bool is_act = (i == active);
            printf("  [%u] %s [%s]%s\n",
                   i,
                   name ? name : "Unknown",
                   en ? "enabled" : "disabled",
                   is_act ? " (active)" : "");
        }
        return;
    }

    const char *action = argv[1];
    if (argc < 3) {
        printf("Usage: keymap [list | on <id/name> | off <id/name> | rm <id/name>]\n");
        return;
    }

    const char *target = argv[2];
    if (strcmp(action, "off") == 0) {
        int res = dynamic_keymap_disable(target);
        if (res == 0) {
            printf("Layout '%s' disabled.\n", target);
        } else {
            printf("Failed to disable layout '%s' (not found).\n", target);
        }
    } else if (strcmp(action, "on") == 0) {
        int res = dynamic_keymap_enable(target);
        if (res == 0) {
            printf("Layout '%s' enabled.\n", target);
        } else {
            printf("Failed to enable layout '%s' (not found).\n", target);
        }
    } else if (strcmp(action, "rm") == 0) {
        int res = dynamic_keymap_remove(target);
        if (res == 0) {
            printf("Layout '%s' removed.\n", target);
        } else {
            printf("Failed to remove layout '%s' (not found or base layout).\n", target);
        }
    } else {
        printf("Unknown action '%s'. Usage: keymap [list | on <id/name> | off <id/name> | rm <id/name>]\n", action);
    }
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

static bool terminal_show_return_value = true;

static void builtin_show_return(int argc, char **argv) {
    if (argc >= 2) {
        if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "1") == 0 || strcmp(argv[1], "enable") == 0) {
            terminal_show_return_value = true;
        } else if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "0") == 0 || strcmp(argv[1], "disable") == 0) {
            terminal_show_return_value = false;
        } else {
            terminal_show_return_value = !terminal_show_return_value;
        }
    } else {
        terminal_show_return_value = !terminal_show_return_value;
    }
    printf("Return value display: %s\n", terminal_show_return_value ? "ON" : "OFF");
}

bool terminal_get_show_return_value(void) {
    return terminal_show_return_value;
}

void terminal_initialize(void) {
    s_terminal_init_count++;
    terminal_cwd_init();

    vga_init_font_cache();

    builtin_clear();

    /* Flush any scancodes that accumulated during boot */
    keyboard_flush_hardware();
    keyboard_flush_queue();
    keyboard_flush_app_queue();
    keyboard_clear_key_state();

    input_len = 0;
    cursor_pos = 0;
    prompt_shown = false;
    prompt_start_cursor = 0;
    terminal_suppress_external_hook = false;

    ensure_input_buffer(0);
    if (input_buf != NULL) {
        memset(input_buf, 0, input_capacity);
    }

    input_start_cursor = 0;

    /* Erase past commands history buffer (especially on repeated initialization) */
    terminal_clear_command_history();

    /* Allocate or clear scrollback buffers */
    if (!terminal_top_buffer) {
        terminal_top_buffer = (uint16_t (*)[VGA_WIDTH])kmalloc(SCROLL_HISTORY_SIZE * VGA_WIDTH * sizeof(uint16_t));
    }
    if (terminal_top_buffer) {
        init_scroll_buffer(terminal_top_buffer);
    }

    if (!terminal_bottom_buffer) {
        terminal_bottom_buffer = (uint16_t (*)[VGA_WIDTH])kmalloc(SCROLL_HISTORY_SIZE * VGA_WIDTH * sizeof(uint16_t));
    }
    if (terminal_bottom_buffer) {
        init_scroll_buffer(terminal_bottom_buffer);
    }

    top_buffer_count = 0;
    bottom_buffer_count = 0;

    if (current_input_snapshot == NULL) {
        current_input_snapshot = kmalloc(256u);
    }
    if (current_input_snapshot != NULL) {
        memset(current_input_snapshot, 0, 256u);
    }

    last_terminal_scancode = 0;
    last_terminal_key_ms = 0;
    last_history_action_ms = 0;

    /* Erase pending injected commands and serial input buffers */
    if (pending_terminal_input != NULL) {
        kfree(pending_terminal_input);
        pending_terminal_input = NULL;
    }
    pending_terminal_auto_exec = false;

    s_serial_cmd_len = 0;
    if (s_serial_cmd_buf != NULL) {
        s_serial_cmd_buf[0] = '\0';
    }

    load_command_history_from_file();
    printf("[terminal] Terminal interface initialized (call #%u, VGA 80x25 text mode, font cache ready).\n", s_terminal_init_count);
}

/* Print the command prompt */
static void print_prompt(void) {
    terminal_suppress_external_hook = true;
    vga_hide_cursor();

    uint16_t cur_pos = vga_get_cursor_position();
    if ((cur_pos % VGA_WIDTH) != 0) {
        /* External output didn't end with a newline; start prompt on next line */
        putchar('\n');
    }

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
    previous_rendered_vis_len = 0;

    /* Clear the remainder of the prompt line to avoid leftover artifacts from previous screen content */
    volatile uint16_t *vga = VGA_MEMORY;
    int32_t line_end = ((input_start_cursor / VGA_WIDTH) + 1) * VGA_WIDTH;
    if (line_end > VGA_WIDTH * VGA_HEIGHT) {
        line_end = VGA_WIDTH * VGA_HEIGHT;
    }
    for (int32_t off = input_start_cursor; off < line_end; off++) {
        vga[off] = vga_entry(' ', INPUT_FG, VGA_COLOR_BLACK);
    }

    if (input_buf != NULL && input_len > 0) {
        /* Restore existing user input on the new line! */
        render_input_line(0);
    } else {
        input_len = 0;
        cursor_pos = 0;
        if (input_buf != NULL) {
            input_buf[0] = '\0';
        }
        vga_set_cursor((uint16_t)input_start_cursor);
        vga_show_cursor();
    }
    terminal_suppress_external_hook = false;
    terminal_apply_pending_input();
}

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
            vga_hide_cursor();
            putchar('\n');

            push_command_history(input_buf);
            int exec = try_execute_command(input_buf);
            if (exec == 0) {
                printf("Command not found: %s\n", input_buf);
            } else if (exec < 0) {
                if (process_get_exit_code() != 126) {
                    printf("Execution failed (error %d): %s\n", exec, input_buf);
                }
            } else if (exec != 1000) {
                if (terminal_show_return_value) {
                    process_print_last_return_value();
                }
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

static void builtin_ifconfig(int argc, char **argv) {
    net_if_t *netif = net_get_interface();
    if (!netif) {
        printf("ifconfig: no network interface available\n");
        return;
    }

    if (argc >= 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            printf("Usage: ifconfig [<ip> [netmask] [gateway] [dns]]\n");
            return;
        }

        ip4_addr_t new_ip = 0;
        if (!str_to_ip(argv[1], &new_ip)) {
            printf("ifconfig: invalid IP address '%s'\n", argv[1]);
            return;
        }

        ip4_addr_t netmask = 0;
        if (argc >= 3) {
            if (!str_to_ip(argv[2], &netmask)) {
                printf("ifconfig: invalid netmask '%s'\n", argv[2]);
                return;
            }
        }

        ip4_addr_t gateway = 0;
        if (argc >= 4) {
            if (!str_to_ip(argv[3], &gateway)) {
                printf("ifconfig: invalid gateway '%s'\n", argv[3]);
                return;
            }
        }

        ip4_addr_t dns = 0;
        if (argc >= 5) {
            if (!str_to_ip(argv[4], &dns)) {
                printf("ifconfig: invalid DNS '%s'\n", argv[4]);
                return;
            }
        }

        net_set_ip(new_ip, netmask, gateway, dns);
        printf("[ifconfig] Network settings updated.\n");
    }

    char *ip_str = ip_to_str_dyn(netif->ip);
    char *mask_str = ip_to_str_dyn(netif->netmask);
    char *gw_str = ip_to_str_dyn(netif->gateway);
    char *dns_str = ip_to_str_dyn(netif->dns);

    printf("eth0: flags=<%s> mtu 1500\n", netif->link_up ? "UP,RUNNING" : "DOWN");
    printf("      inet %s  netmask %s\n", ip_str ? ip_str : "Unknown", mask_str ? mask_str : "Unknown");
    printf("      gateway %s  dns %s\n", gw_str ? gw_str : "Unknown", dns_str ? dns_str : "Unknown");
    printf("      ether %02x:%02x:%02x:%02x:%02x:%02x\n",
           netif->mac.mac[0], netif->mac.mac[1], netif->mac.mac[2],
           netif->mac.mac[3], netif->mac.mac[4], netif->mac.mac[5]);

    if (ip_str) kfree(ip_str);
    if (mask_str) kfree(mask_str);
    if (gw_str) kfree(gw_str);
    if (dns_str) kfree(dns_str);
}

static void builtin_hardware(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "overview") == 0) {
        printf("================ IPO_OS Hardware Resources Overview ================\n");
        char *vendor = smp_get_cpu_vendor_dyn();
        char *brand = smp_get_cpu_brand_dyn();
        printf("  CPU:       %u core(s) detected (%u active online)\n",
               smp_get_cpu_count(), smp_get_active_cpu_count());
        printf("             Vendor: %s, Model: %s\n",
               (vendor && vendor[0]) ? vendor : "Unknown",
               (brand && brand[0]) ? brand : "Unknown");
        if (vendor) kfree(vendor);
        if (brand) kfree(brand);

        uint64_t total_ram = memory_map_get_total_ram();
        char *size_buf = format_size_dynamic(total_ram);
        printf("  Memory:    Total RAM: %s (%u usable regions)\n",
               size_buf ? size_buf : "Unknown", memory_map_get_region_count());
        if (size_buf) kfree(size_buf);

        printf("  PCI:       %u PCI device(s) detected on bus\n", pci_get_count());
        printf("  Storage:   %u ATA/IDE drive(s) present, pool capacity: %u MB\n",
               ata_get_device_count(), (uint32_t)(ata_get_pool_capacity() / 2048ull));

        uint32_t net_devs = device_get_count_by_type(DEVICE_TYPE_NETWORK);
        printf("  Network:   %u network interface(s) registered\n", net_devs);

        printf("  Registry:  Total devices: %u (ACTIVE: %u, INITIALIZED: %u)\n",
               device_get_count(),
               device_get_count_by_state(DEVICE_STATE_ACTIVE),
               device_get_count_by_state(DEVICE_STATE_INITIALIZED));
        return;
    }

    if (strcmp(argv[1], "list") == 0) {
        printf(" ID  TYPE     STATE        NAME         RESOURCES            DESCRIPTION\n");
        printf(" -----------------------------------------------------------------------------\n");
        const device_node_t *curr = device_get_list();
        while (curr) {
            printf(" %2u  ", curr->id);
            print_padded(device_type_to_string(curr->type), 9);
            print_padded(device_state_to_string(curr->state), 13);
            print_padded((curr->name && curr->name[0]) ? curr->name : "Unknown", 13);

            size_t res_size = 64;
            char *res_str = (char *)kmalloc(res_size);
            if (res_str) {
                if (curr->res.bus_type == BUS_TYPE_PCI) {
                    snprintf(res_str, res_size, "PCI %02x:%02x.%x",
                             curr->res.pci_bus, curr->res.pci_slot, curr->res.pci_func);
                } else if (curr->res.io_base != 0) {
                    if (curr->res.irq > 0 && curr->res.irq < 255) {
                        snprintf(res_str, res_size, "IO 0x%x, IRQ %u", curr->res.io_base, curr->res.irq);
                    } else {
                        snprintf(res_str, res_size, "IO 0x%x", curr->res.io_base);
                    }
                } else if (curr->res.mmio_base != 0) {
                    snprintf(res_str, res_size, "MMIO 0x%x", curr->res.mmio_base);
                } else if (curr->res.irq > 0 && curr->res.irq < 255) {
                    snprintf(res_str, res_size, "IRQ %u", curr->res.irq);
                } else {
                    snprintf(res_str, res_size, "-");
                }
                print_padded(res_str, 21);
                kfree(res_str);
            } else {
                print_padded("-", 21);
            }

            printf("%s\n", (curr->description && curr->description[0]) ? curr->description : "Unknown");
            curr = curr->next;
        }
        return;
    }

    if (strcmp(argv[1], "cpu") == 0) {
        printf("=== Central Processing Unit (CPU & SMP) ===\n");
        char *vendor = smp_get_cpu_vendor_dyn();
        char *brand = smp_get_cpu_brand_dyn();
        char *features = smp_get_cpu_features_str_dyn();
        uint32_t fam = 0, mod = 0, step = 0;
        smp_get_cpu_model_info(&fam, &mod, &step);

        printf("  Vendor:           %s\n", (vendor && vendor[0]) ? vendor : "Unknown");
        printf("  Model / Brand:    %s\n", (brand && brand[0]) ? brand : "Unknown");
        printf("  Family/Model/Stp: Family %u, Model %u, Stepping %u\n", fam, mod, step);
        printf("  Detected Cores:   %u logical core(s)\n", smp_get_cpu_count());
        printf("  Active Cores:     %u online core(s)\n", smp_get_active_cpu_count());
        printf("  CPUID Features:   %s\n", (features && features[0]) ? features : "Unknown");
        if (vendor) kfree(vendor);
        if (brand) kfree(brand);
        if (features) kfree(features);

        printf("\n  Logical Cores Table:\n");
        printf("  CORE  LAPIC  ROLE     STATUS   TICKS       STACK TOP   STACK BASE\n");
        printf("  --------------------------------------------------------------------\n");
        for (uint32_t i = 0; i < smp_get_cpu_count(); i++) {
            const cpu_info_t *c = smp_get_cpu_info(i);
            if (!c) continue;
            printf("  %-4u  %-5u  %-7s  %-7s  %-10u  0x%08x  0x%08x\n",
                   c->cpu_id, c->lapic_id,
                   c->is_bsp ? "BSP" : "AP",
                   c->is_online ? "ONLINE" : "OFFLINE",
                   c->ticks,
                   c->stack_top, c->stack_base);
        }

        const smp_architecture_status_t *as = smp_get_architecture_status();
        printf("\n  SMP Architectural Components Status:\n");
        #define SMP_STAT_STR(s) ((s) == SMP_STATUS_ACTIVE ? "ACTIVE" : ((s) == SMP_STATUS_INACTIVE ? "INACTIVE" : "BLOCKED"))
        printf("    CPUID Capability Detection:    %s\n", SMP_STAT_STR(as->cpu_capability_detection));
        printf("    ACPI MADT Enumeration:         %s\n", SMP_STAT_STR(as->acpi_madt_enumeration));
        printf("    MP Table Discovery Fallback:   %s\n", SMP_STAT_STR(as->mp_table_fallback));
        printf("    Per-CPU Stacks & Structures:   %s\n", SMP_STAT_STR(as->per_cpu_structures));
        printf("    Atomic Bus-Locked Spinlocks:   %s\n", SMP_STAT_STR(as->atomic_spinlocks));
        printf("    AP Startup Sequence (ICR):     %s\n", SMP_STAT_STR(as->ap_boot_sequence));
        printf("    Driver Subsystem Reentrancy:   %s\n", SMP_STAT_STR(as->driver_reentrancy));
        printf("    IPI Cross-Core Rescheduling:   %s\n", SMP_STAT_STR(as->ipi_cross_core_reschedule));
        printf("    Preemptive Multi-Core Sched:   %s\n", SMP_STAT_STR(as->cross_core_scheduler));
        #undef SMP_STAT_STR
        return;
    }

    if (strcmp(argv[1], "memory") == 0) {
        printf("=== Physical Memory & Address Map (BIOS E820) ===\n");
        uint64_t total = memory_map_get_total_ram();
        char *size_buf = format_size_dynamic(total);
        printf("  Total Detected RAM: %s (%llu bytes)\n", size_buf ? size_buf : "Unknown", total);
        if (size_buf) kfree(size_buf);

        uint32_t raw_e820_count = memory_map_get_raw_e820_count();
        if (raw_e820_count > 0) {
            printf("\n  Raw BIOS E820 Memory Map (%u entries):\n", raw_e820_count);
            printf("  #   BASE ADDRESS              LENGTH          TYPE\n");
            printf("  --------------------------------------------------------------\n");
            for (uint32_t i = 0; i < raw_e820_count; i++) {
                const e820_entry_t *e = memory_map_get_raw_e820_entry(i);
                if (!e) continue;
                char *esize = format_size_dynamic(e->length);
                size_t range_cap = 48;
                char *range_str = (char *)kmalloc(range_cap);
                printf("  %-2u  ", i);
                if (range_str) {
                    snprintf(range_str, range_cap, "0x%08llx - 0x%08llx", e->base, e->base + e->length);
                    print_padded(range_str, 26);
                    kfree(range_str);
                } else {
                    printf("0x%08llx - 0x%08llx  ", e->base, e->base + e->length);
                }
                printf("%-14s  %s\n",
                       esize ? esize : "Unknown",
                       memory_map_e820_type_name(e->type));
                if (esize) kfree(esize);
            }
        }

        printf("\n  Allocatable & Managed Memory Pools (%u pools):\n", memory_map_get_region_count());
        printf("  #   BASE ADDRESS  POOL SIZE     RESERVED      STATUS\n");
        printf("  ---------------------------------------------------------\n");
        for (uint32_t i = 0; i < memory_map_get_region_count(); i++) {
            const memory_region_t *r = memory_map_get_region(i);
            if (!r) continue;
            char *rsize = format_size_dynamic((uint64_t)r->size);
            char *used_str = format_size_dynamic((uint64_t)r->used);
            printf("  %-2u  0x%08x    %-12s  %-12s  %s\n",
                   i, r->base, rsize ? rsize : "Unknown",
                   used_str ? used_str : "Unknown",
                   r->is_usable ? "ACTIVE" : "INITIALIZED");
            if (rsize) kfree(rsize);
            if (used_str) kfree(used_str);
        }
        return;
    }

    if (strcmp(argv[1], "pci") == 0) {
        printf("=== Peripheral Component Interconnect (PCI Bus) ===\n");
        uint32_t count = pci_get_count();
        printf("  Discovered %u PCI device(s):\n\n", count);
        printf("  B:S.F   VENDOR:DEV   VENDOR                  DEVICE / MODEL [CLASS:SUB]\n");
        printf("  -------------------------------------------------------------------------\n");
        for (uint32_t i = 0; i < count; i++) {
            const pci_device_t *p = pci_get_device(i);
            if (!p) continue;
            printf("  %02x:%02x.%x  %04x:%04x     ", p->bus, p->slot, p->func, p->vendor_id, p->device_id);
            print_padded((p->vendor_name && p->vendor_name[0]) ? p->vendor_name : "Unknown", 24);
            printf("%s [%02x:%02x]",
                   (p->device_name && p->device_name[0]) ? p->device_name : "Unknown",
                   p->class_code, p->subclass);
            if (p->irq > 0 && p->irq < 255) {
                printf(" (IRQ %u)", p->irq);
            }
            putchar('\n');
        }
        return;
    }

    if (strcmp(argv[1], "storage") == 0) {
        printf("=== ATA/IDE Storage Subsystem ===\n");
        uint32_t count = ata_get_device_count();
        printf("  Detected ATA Drive(s): %u\n", count);
        uint64_t pool_sec = ata_get_pool_capacity();
        uint32_t def_sec_sz = 512u;
        if (count > 0 && ata_get_device(0) && ata_get_device(0)->sector_size > 0) {
            def_sec_sz = ata_get_device(0)->sector_size;
        }
        char *pool_sz = format_size_dynamic(pool_sec * (uint64_t)def_sec_sz);
        printf("  Multi-Drive Storage Pool Capacity: %s (%llu sectors)\n\n", pool_sz ? pool_sz : "Unknown", pool_sec);
        if (pool_sz) kfree(pool_sz);

        printf("  DRIVE  CHANNEL  ROLE    MODEL                     SERIAL           FIRMWARE   CAPACITY\n");
        printf("  ---------------------------------------------------------------------------------------\n");
        for (uint32_t i = 0; i < count; i++) {
            ata_device_t *d = ata_get_device(i);
            if (!d || !d->present) continue;
            uint32_t sec_sz = d->sector_size ? d->sector_size : def_sec_sz;
            char *cap_buf = format_size_dynamic(d->capacity_sectors * (uint64_t)sec_sz);
            printf("  %-5u  %-7u  %-6s  ", i, d->channel, d->drive ? "Slave" : "Master");
            print_padded((d->model && d->model[0]) ? d->model : "Unknown", 26);
            print_padded((d->serial && d->serial[0]) ? d->serial : "Unknown", 17);
            print_padded((d->firmware && d->firmware[0]) ? d->firmware : "Unknown", 11);
            printf("%s\n", cap_buf ? cap_buf : "Unknown");
            if (cap_buf) kfree(cap_buf);
        }
        return;
    }

    if (strcmp(argv[1], "network") == 0) {
        printf("=== Network Interfaces & Controllers ===\n");
        uint32_t net_pci_count = 0;
        uint32_t count = pci_get_count();
        for (uint32_t i = 0; i < count; i++) {
            const pci_device_t *p = pci_get_device(i);
            if (!p || p->class_code != 0x02) continue;
            net_pci_count++;
            printf("  Controller #%u: %s [0x%04X:0x%04X], Class 0x%02X:0x%02X\n",
                   net_pci_count,
                   (p->device_name && p->device_name[0]) ? p->device_name : "Unknown",
                   p->vendor_id, p->device_id, p->class_code, p->subclass);
            printf("     Vendor:    %s\n", (p->vendor_name && p->vendor_name[0]) ? p->vendor_name : "Unknown");
            printf("     Location:  PCI %02x:%02x.%x, IRQ: %u\n",
                   p->bus, p->slot, p->func, p->irq);
            for (size_t b = 0; b < p->bar_count; b++) {
                if (p->bar[b] == 0) continue;
                if (p->bar[b] & 1) {
                    printf("     BAR[%u]:    I/O Port 0x%x\n", (unsigned)b, (unsigned int)(p->bar[b] & ~3u));
                } else {
                    printf("     BAR[%u]:    MMIO 0x%08x\n", (unsigned)b, (unsigned int)(p->bar[b] & ~0xFu));
                }
            }

            if (p->vendor_id == RTL8139_VENDOR_ID && p->device_id == RTL8139_DEVICE_ID && rtl8139_is_present()) {
                const uint8_t *m = rtl8139_get_mac();
                if (m) {
                    printf("     MAC:       %02x:%02x:%02x:%02x:%02x:%02x\n",
                           m[0], m[1], m[2], m[3], m[4], m[5]);
                } else {
                    printf("     MAC:       Unknown\n");
                }
                uint16_t io = (p->bar_count > 0) ? (uint16_t)(p->bar[0] & ~3u) : 0;
                if (io != 0) {
                    uint8_t msr = inb(io + 0x58);
                    bool link_up = (msr & 0x04) == 0;
                    int speed = (msr & 0x08) ? 10 : 100;
                    printf("     Hardware:  Link %s (%d Mbps)\n", link_up ? "UP" : "DOWN", speed);
                }
            }
        }
        if (net_pci_count == 0) {
            printf("  No physical PCI network controllers detected.\n");
        }
        return;
    }

    if (strcmp(argv[1], "usb") == 0) {
        printf("=== Universal Serial Bus (USB Subsystem) ===\n");
        uint32_t count = pci_get_count();
        int usb_found = 0;
        for (uint32_t i = 0; i < count; i++) {
            const pci_device_t *p = pci_get_device(i);
            if (!p) continue;
            if (p->class_code == 0x0C && p->subclass == 0x03) {
                usb_found++;
                printf("  Controller #%d: %s [0x%04X:0x%04X], Class 0x%02X:0x%02X, ProgIF 0x%02X\n",
                       usb_found,
                       (p->device_name && p->device_name[0]) ? p->device_name : "Unknown",
                       p->vendor_id, p->device_id, p->class_code, p->subclass, p->prog_if);
                printf("    Location:  PCI %02x:%02x.%x\n", p->bus, p->slot, p->func);
                printf("    Vendor:    %s\n", (p->vendor_name && p->vendor_name[0]) ? p->vendor_name : "Unknown");
                printf("    IRQ:       %u\n", p->irq);
                for (size_t b = 0; b < p->bar_count; b++) {
                    if (p->bar[b] == 0) continue;
                    if (p->bar[b] & 1) {
                        printf("    BAR[%u]:    0x%08x (I/O Port 0x%x)\n", (unsigned)b, p->bar[b], (unsigned)(p->bar[b] & ~3u));
                    } else {
                        printf("    BAR[%u]:    0x%08x (MMIO 0x%08x)\n", (unsigned)b, p->bar[b], (unsigned)(p->bar[b] & ~0xFu));
                    }
                }
                uint16_t pci_cmd = pci_read_word(p->bus, p->slot, p->func, 0x04);
                printf("    Status:    %s\n", (pci_cmd & 0x07u) ? "ACTIVE" : "INITIALIZED");
            }
        }
        if (usb_found == 0) {
            printf("  No USB host controllers detected on PCI bus.\n");
        }
        return;
    }

    if (strcmp(argv[1], "refresh") == 0) {
        printf("[hardware] Rescanning hardware buses...\n");
        int total = device_rescan();
        printf("[hardware] Rescan complete. Registry updated: %d registered devices (ACTIVE: %u, INITIALIZED: %u).\n",
               total,
               device_get_count_by_state(DEVICE_STATE_ACTIVE),
               device_get_count_by_state(DEVICE_STATE_INITIALIZED));
        return;
    }

    if (strcmp(argv[1], "info") == 0) {
        if (argc < 3) {
            printf("Usage: hardware info <device_id>\n");
            return;
        }
        uint32_t id = (uint32_t)atoi(argv[2]);
        const device_node_t *dev = device_find_by_id(id);
        if (!dev) {
            printf("hardware: device ID %u not found. Run 'hardware list' for available IDs.\n", id);
            return;
        }
        printf("=== Hardware Device Detail: ID %u ===\n", dev->id);
        printf("  Canonical Name:     %s\n", (dev->name && dev->name[0]) ? dev->name : "Unknown");
        printf("  Category / Type:    %s\n", device_type_to_string(dev->type));
        printf("  Operational State:  %s\n", device_state_to_string(dev->state));
        printf("  Vendor:             %s\n", (dev->vendor && dev->vendor[0]) ? dev->vendor : "Unknown");
        printf("  Model:              %s\n", (dev->model && dev->model[0]) ? dev->model : "Unknown");
        printf("  Description:        %s\n", (dev->description && dev->description[0]) ? dev->description : "Unknown");
        printf("  Bus Interface:      %s\n", bus_type_to_string(dev->res.bus_type));
        printf("  Associated Driver:  %s\n", (dev->driver_name && dev->driver_name[0]) ? dev->driver_name : "Unknown");
        if (dev->res.bus_type == BUS_TYPE_PCI) {
            printf("  PCI Coordinates:    Bus %u, Slot %u, Func %u\n",
                   dev->res.pci_bus, dev->res.pci_slot, dev->res.pci_func);
            printf("  PCI IDs:            Vendor 0x%04x, Device 0x%04x\n",
                   dev->res.vendor_id, dev->res.device_id);
            printf("  PCI Class:          0x%02x, Subclass 0x%02x, ProgIF 0x%02x\n",
                   dev->res.pci_class, dev->res.pci_subclass, dev->res.pci_progif);
            for (size_t b = 0; b < dev->res.bar_count; b++) {
                if (dev->res.bar[b] != 0) {
                    printf("  BAR[%u]:             0x%08x\n", (uint32_t)b, dev->res.bar[b]);
                }
            }
        }
        if (dev->res.io_base != 0) {
            printf("  I/O Base Port:      0x%x\n", dev->res.io_base);
        }
        if (dev->res.mmio_base != 0) {
            printf("  MMIO Base Address:  0x%08x (Size: %u bytes)\n",
                   dev->res.mmio_base, dev->res.mmio_size);
        }
        if (dev->res.irq > 0 && dev->res.irq < 255) {
            printf("  IRQ Line:           %u\n", dev->res.irq);
        }
        if (dev->res.size_bytes != 0 && dev->type != DEVICE_TYPE_CPU) {
            char *sz = format_size_dynamic(dev->res.size_bytes);
            printf("  Capacity / Size:    %s (%llu bytes)\n", sz ? sz : "Unknown", dev->res.size_bytes);
            if (sz) kfree(sz);
        }
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        printf("=== Hardware Health & Subsystem Status ===\n");
        uint32_t err_count = device_get_count_by_state(DEVICE_STATE_ERROR);
        uint32_t dis_count = device_get_count_by_state(DEVICE_STATE_DISABLED);
        uint32_t cpus_online = smp_get_active_cpu_count();
        uint32_t cpus_total = smp_get_cpu_count();

        const char *health_str = "HEALTHY";
        if (err_count > 0) {
            health_str = "ERROR (Hardware errors present)";
        } else if (cpus_online < cpus_total) {
            health_str = "DEGRADED (Secondary CPU core(s) offline)";
        } else if (dis_count > 0) {
            health_str = "DEGRADED (Disabled devices present)";
        }
        printf("  System Hardware Status: %s\n", health_str);
        printf("  Total Registered:       %u devices\n", device_get_count());
        printf("    - ACTIVE:             %u\n", device_get_count_by_state(DEVICE_STATE_ACTIVE));
        printf("    - INITIALIZED:        %u\n", device_get_count_by_state(DEVICE_STATE_INITIALIZED));
        printf("    - DISCOVERED:         %u\n", device_get_count_by_state(DEVICE_STATE_DISCOVERED));
        printf("    - DISABLED:           %u\n", device_get_count_by_state(DEVICE_STATE_DISABLED));
        printf("    - ERROR:              %u\n", device_get_count_by_state(DEVICE_STATE_ERROR));
        printf("  Subsystem Device Counts:\n");
        printf("    - CPU Cores:          %u\n", device_get_count_by_type(DEVICE_TYPE_CPU));
        printf("    - Memory Pools:       %u\n", device_get_count_by_type(DEVICE_TYPE_MEMORY));
        printf("    - PCI Devices:        %u\n", device_get_count_by_type(DEVICE_TYPE_PCI));
        printf("    - Storage Drives:     %u\n", device_get_count_by_type(DEVICE_TYPE_STORAGE));
        printf("    - Network Adapters:   %u\n", device_get_count_by_type(DEVICE_TYPE_NETWORK));
        printf("    - Input Devices:      %u\n", device_get_count_by_type(DEVICE_TYPE_INPUT));
        printf("    - Audio Devices:      %u\n", device_get_count_by_type(DEVICE_TYPE_AUDIO));
        printf("    - Video Adapters:     %u\n", device_get_count_by_type(DEVICE_TYPE_VIDEO));
        printf("    - System Controllers: %u\n", device_get_count_by_type(DEVICE_TYPE_SYSTEM));
        printf("    - USB Controllers:    %u\n", device_get_count_by_type(DEVICE_TYPE_USB));
        printf("    - Unknown / Other:    %u\n", device_get_count_by_type(DEVICE_TYPE_UNKNOWN));
        return;
    }

    printf("hardware: unknown subcommand '%s'.\n", argv[1]);
    printf("Usage: hardware [overview | list | cpu | memory | pci | storage | network | usb | refresh | info <id> | status]\n");
}

static void builtin_storage(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "list") == 0) {
        storage_pool_print_status();
        printf("\n=== Physical Block Devices & Partitions ===\n");
        block_device_t *bdev = block_dev_get_list();
        if (!bdev) {
            printf("  No block devices registered in system.\n");
        } else {
            while (bdev) {
                partition_table_t ptable;
                partition_scan(bdev, &ptable);
                partition_print_table(&ptable);
                partition_free_table(&ptable);
                bdev = bdev->next;
            }
        }
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        storage_pool_print_status();
        return;
    }

    if (strcmp(argv[1], "usage") == 0) {
        storage_pool_print_usage();
        return;
    }

    if (strcmp(argv[1], "check") == 0) {
        storage_pool_check();
        return;
    }

    if (strcmp(argv[1], "create") == 0) {
        const char *uuid = (argc > 2) ? argv[2] : NULL;
        const char *dev = (argc > 3) ? argv[3] : NULL;
        if (dev) {
            printf("Creating persistent storage pool '%s' on device '%s'...\n", uuid ? uuid : "default", dev);
        } else if (uuid) {
            printf("Creating persistent storage pool '%s'...\n", uuid);
        } else {
            printf("Creating persistent storage pool...\n");
        }
        int res = storage_pool_create_ex(uuid, dev);
        if (res == 0) {
            printf("Storage pool successfully created and persisted to disk.\n");
        } else if (res == -2) {
            printf("Failed to create storage pool: target block device not found or empty.\n");
        } else {
            printf("Failed to create storage pool (error %d).\n", res);
        }
        return;
    }

    if (strcmp(argv[1], "add") == 0) {
        if (argc < 3) {
            printf("Usage: storage add <block_device>\n");
            return;
        }
        block_device_t *bdev = block_dev_get(argv[2]);
        if (!bdev) {
            printf("storage: block device '%s' not found.\n", argv[2]);
            return;
        }
        int res = storage_pool_add_device(bdev);
        if (res == 0) {
            printf("Successfully attached '%s' to storage pool.\n", bdev->name ? bdev->name : argv[2]);
        } else {
            printf("Failed to add '%s' to storage pool (error %d).\n", argv[2], res);
        }
        return;
    }

    if (strcmp(argv[1], "recover") == 0) {
        printf("Executing manual storage pool crash recovery...\n");
        int count = storage_pool_recover();
        if (count >= 0) {
            printf("Recovery finished: %d interrupted transaction(s) resolved.\n", count);
        } else {
            printf("Recovery failed (primary member unavailable).\n");
        }
        return;
    }

    if (strcmp(argv[1], "save") == 0) {
        int res = storage_pool_save_metadata();
        if (res == 0) {
            printf("Storage pool metadata successfully synchronized to disk.\n");
        } else {
            printf("Failed to save storage pool metadata.\n");
        }
        return;
    }

    printf("storage: unknown subcommand '%s'.\n", argv[1]);
    printf("Usage: storage [list | status | usage | check | create [uuid] [dev] | add <dev> | recover | save]\n");
}

static void builtin_symbols(int argc, char **argv) {
    const char *filter = (argc >= 2) ? argv[1] : NULL;
    size_t count = 0;
    const kernel_sym_entry_t *syms = kmod_get_symbols(&count);
    if (!syms || count == 0) {
        printf("symbols: No exported kernel symbols found.\n");
        return;
    }
    printf("=== Exported Kernel Symbols (%u total) ===\n", (uint32_t)count);
    size_t shown = 0;
    for (size_t i = 0; i < count; i++) {
        if (!filter || strstr(syms[i].name, filter)) {
            printf("  %p  %s\n", syms[i].addr, syms[i].name);
            shown++;
        }
    }
    if (filter) {
        printf("Found %u symbol(s) matching '%s'.\n", (uint32_t)shown, filter);
    }
}

static void builtin_drivers(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "list") == 0) {
        printf("=== Loaded Drivers & Dynamic Modules ===\n");
        printf("  Loaded Dynamic Drivers:  %u active\n", driver_count_kernel() + driver_count_user());
        printf("  Dynamic Modules (KMOD):  %u loaded (ABI 0x%04x)\n\n", kmod_count(), KMOD_ABI_VERSION);
        driver_print_list();
        printf("\n");
        kmod_print_list();
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        size_t ksym_cnt = 0;
        kmod_get_symbols(&ksym_cnt);
        printf("=== Driver Subsystem Status ===\n");
        printf("  Loaded Dynamic Drivers:  %u active\n", driver_count_kernel() + driver_count_user());
        printf("  Dynamic Modules (KMOD):  %u loaded\n", kmod_count());
        printf("  KMOD ABI Version:        0x%04x (v1.0)\n", KMOD_ABI_VERSION);
        printf("  Exported Kernel Symbols: %u functions\n", (uint32_t)ksym_cnt);
        printf("  KMOD Executable Format:  IKMD Header, Relocatable 32-bit ELF payload\n");
        return;
    }

    if (strcmp(argv[1], "info") == 0) {
        if (argc < 3) {
            printf("Usage: drivers info <id|name>\n");
            return;
        }
        uint32_t id = 0;
        for (int i = 0; argv[2][i] >= '0' && argv[2][i] <= '9'; i++) {
            id = id * 10 + (argv[2][i] - '0');
        }
        kmod_t *mod = (id > 0) ? kmod_find_by_id(id) : kmod_find_by_name(argv[2]);
        if (mod) {
            printf("=== Dynamic Driver Module: %s ===\n", (mod->name && mod->name[0]) ? mod->name : "Unknown");
            printf("  Module ID:   %u\n", mod->id);
            printf("  Author:      %s\n", (mod->author && mod->author[0]) ? mod->author : "Unknown");
            printf("  Version:     %s\n", (mod->version && mod->version[0]) ? mod->version : "Unknown");
            printf("  Description: %s\n", (mod->description && mod->description[0]) ? mod->description : "Unknown");
            printf("  ABI Version: 0x%04x\n", mod->abi_version);
            printf("  Memory Base: %p (%u bytes)\n", mod->image_base, (uint32_t)mod->image_size);
            printf("  References:  %u\n", mod->ref_count);
            printf("  Status:      %s\n", (mod->flags & KMOD_FLAG_ACTIVE) ? "ACTIVE" : "LOADED");
            return;
        }

        driver_t *d = driver_find(argv[2]);
        if (d) {
            printf("=== %s Driver: %s ===\n",
                   (d->flags & DRIVER_FLAG_KERNEL) ? "Built-in Kernel" : "Userland Resident",
                   (d->name && d->name[0]) ? d->name : "Unknown");
            printf("  Description: %s\n", (d->description && d->description[0]) ? d->description : "Unknown");
            printf("  Type:        %s\n", (d->flags & DRIVER_FLAG_KERNEL) ? "Kernel Core" : "Userland Process");
            if (d->owner && process_is_valid((process_t *)d->owner)) {
                process_t *p = (process_t *)d->owner;
                printf("  Owner PID:   %u (%s)\n", p->pid, (p->name && p->name[0]) ? p->name : "Unknown");
            }
            printf("  Flags:       0x%08x (%s)\n",
                   d->flags, (d->flags & DRIVER_FLAG_ACTIVE) ? "ACTIVE" : "REGISTERED");
            printf("  Hooks:       ");
            if (d->on_tick) printf("Timer ");
            if (d->on_key) printf("Keyboard ");
            if (d->on_command) printf("Command ");
            if (d->on_char_output) printf("CharOut ");
            if (d->read || d->write) printf("I/O ");
            putchar('\n');
            return;
        }

        printf("drivers: Module or driver '%s' not found.\n", argv[2]);
        return;
    }

    if (strcmp(argv[1], "load") == 0) {
        if (argc < 3) {
            printf("Usage: drivers load <path/to/driver.kmd>\n");
            return;
        }
        kmod_t *mod = NULL;
        int res = kmod_load_file(argv[2], &mod);
        if (res == 0 && mod) {
            printf("drivers: Successfully loaded module '%s' (ID %u, %u bytes @ %p).\n",
                   mod->name, mod->id, (uint32_t)mod->image_size, mod->image_base);
        } else {
            printf("drivers: Failed to load module from '%s' (error code %d).\n", argv[2], res);
        }
        return;
    }

    if (strcmp(argv[1], "unload") == 0) {
        if (argc < 3) {
            printf("Usage: drivers unload <id|name>\n");
            return;
        }
        uint32_t id = 0;
        for (int i = 0; argv[2][i] >= '0' && argv[2][i] <= '9'; i++) {
            id = id * 10 + (argv[2][i] - '0');
        }
        int res = (id > 0) ? kmod_unload(id) : kmod_unload_by_name(argv[2]);
        if (res == 0) {
            printf("drivers: Successfully unloaded module '%s'.\n", argv[2]);
        } else {
            printf("drivers: Failed to unload module '%s' (error code %d).\n", argv[2], res);
        }
        return;
    }

    if (strcmp(argv[1], "symbols") == 0 || strcmp(argv[1], "syms") == 0) {
        builtin_symbols(argc - 1, argv + 1);
        return;
    }

    printf("drivers: unknown subcommand '%s'.\n", argv[1]);
    printf("Usage: drivers [list | info <id/name> | load <path> | unload <id/name> | status]\n");
}

static bool parse_command_redirection(const char *cmdline, char **out_cmd, char **out_file, bool *out_append) {
    if (out_cmd) *out_cmd = NULL;
    if (out_file) *out_file = NULL;
    if (out_append) *out_append = false;
    if (!cmdline) return false;

    const char *p = cmdline;
    char in_quote = '\0';
    const char *redir_pos = NULL;
    bool is_append = false;

    while (*p) {
        char c = *p;
        if (in_quote) {
            if (c == in_quote) {
                in_quote = '\0';
            } else if (c == '\\' && (*(p + 1) == in_quote || *(p + 1) == '\\')) {
                p++;
            }
        } else {
            if (c == '"' || c == '\'') {
                in_quote = c;
            } else if (c == '\\' && *(p + 1) != '\0') {
                p++;
            } else if (c == '>') {
                redir_pos = p;
                if (*(p + 1) == '>') {
                    is_append = true;
                } else {
                    is_append = false;
                }
                break;
            }
        }
        p++;
    }

    if (!redir_pos) return false;

    /* Extract clean command portion before redirection */
    size_t cmd_len = (size_t)(redir_pos - cmdline);
    while (cmd_len > 0 && (cmdline[cmd_len - 1] == ' ' || cmdline[cmd_len - 1] == '\t')) {
        cmd_len--;
    }
    char *clean_cmd = (char *)kmalloc(cmd_len + 1);
    if (!clean_cmd) return false;
    memcpy(clean_cmd, cmdline, cmd_len);
    clean_cmd[cmd_len] = '\0';

    /* Extract target filename after > or >> */
    const char *fn_start = is_append ? (redir_pos + 2) : (redir_pos + 1);
    while (*fn_start == ' ' || *fn_start == '\t') fn_start++;

    if (*fn_start == '\0') {
        kfree(clean_cmd);
        return false;
    }

    size_t fn_cap = strlen(fn_start) + 1;
    char *target_fn = (char *)kmalloc(fn_cap);
    if (!target_fn) {
        kfree(clean_cmd);
        return false;
    }

    size_t fn_pos = 0;
    const char *f = fn_start;

    if (*f == '"' || *f == '\'') {
        char q = *f++;
        while (*f && *f != q) {
            if (*f == '\\' && (*(f + 1) == q || *(f + 1) == '\\')) {
                f++;
            }
            target_fn[fn_pos++] = *f++;
        }
        if (*f == q) f++;
    } else {
        /* Unquoted filename: take until newline/carriage return, unescaping backslashes */
        while (*f && *f != '\n' && *f != '\r') {
            if (*f == '\\' && *(f + 1) != '\0') {
                f++;
            }
            target_fn[fn_pos++] = *f++;
        }
        while (fn_pos > 0 && (target_fn[fn_pos - 1] == ' ' || target_fn[fn_pos - 1] == '\t')) {
            fn_pos--;
        }
    }
    target_fn[fn_pos] = '\0';

    if (fn_pos == 0) {
        kfree(clean_cmd);
        kfree(target_fn);
        return false;
    }

    if (out_cmd) *out_cmd = clean_cmd;
    else kfree(clean_cmd);

    if (out_file) *out_file = target_fn;
    else kfree(target_fn);

    if (out_append) *out_append = is_append;
    return true;
}

int try_execute_command(const char *cmdline) {
    if (!cmdline) return -1;

    char *clean_cmd = NULL;
    char *redir_file = NULL;
    bool append_mode = false;

    if (parse_command_redirection(cmdline, &clean_cmd, &redir_file, &append_mode)) {
        stdout_redirect_start();
        int res = try_execute_command(clean_cmd);
        char *redir_buf = NULL;
        size_t redir_len = 0;
        stdout_redirect_end(&redir_buf, &redir_len);

        char *target_path = make_abs_path_dyn(redir_file);

        if (!ipo_fs_write_text(target_path ? target_path : redir_file, redir_buf ? redir_buf : "", append_mode)) {
            printf("redirection: failed to write to '%s'\n", redir_file);
        }

        if (target_path) kfree(target_path);
        if (redir_buf) kfree(redir_buf);
        kfree(clean_cmd);
        kfree(redir_file);
        return res;
    }

    int argc = 0;
    char **argv = NULL;
    if (cmdline_split(cmdline, &argc, &argv) <= 0 || argc == 0 || !argv || !argv[0]) {
        if (argv) cmdline_free_args(argc, argv);
        return 0;
    }
    const char *name = argv[0];

    // Check Builtins
    int builtin_handled = 0;
    if (strcmp(name, "help") == 0) {
        builtin_help();
        builtin_handled = 1;
    } else if (strcmp(name, "clear") == 0) {
        builtin_clear();
        builtin_handled = 1;
    } else if (strcmp(name, "pwd") == 0) {
        builtin_pwd();
        builtin_handled = 1;
    } else if (strcmp(name, "cd") == 0) {
        builtin_cd(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "ls") == 0) {
        builtin_ls(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "cat") == 0) {
        builtin_cat(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "touch") == 0) {
        builtin_touch(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "mkdir") == 0) {
        builtin_mkdir(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "rm") == 0) {
        builtin_rm(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "cp") == 0) {
        builtin_cp(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "mv") == 0) {
        builtin_mv(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "stat") == 0) {
        builtin_stat(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "echo") == 0) {
        builtin_echo(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "reboot") == 0) {
        builtin_reboot();
        builtin_handled = 1;
    } else if (strcmp(name, "shutdown") == 0) {
        builtin_shutdown();
        builtin_handled = 1;
    } else if (strcmp(name, "reinstall_fs") == 0) {
        builtin_reinstall_fs();
        builtin_handled = 1;
    } else if (strcmp(name, "ps") == 0) {
        builtin_ps();
        builtin_handled = 1;
    } else if (strcmp(name, "kill") == 0) {
        builtin_kill(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "killall") == 0) {
        builtin_killall();
        builtin_handled = 1;
    } else if (strcmp(name, "df") == 0) {
        builtin_df();
        builtin_handled = 1;
    } else if (strcmp(name, "free") == 0) {
        builtin_meminfo();
        builtin_handled = 1;
    } else if (strcmp(name, "drivers") == 0) {
        builtin_drivers(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "symbols") == 0) {
        builtin_symbols(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "storage") == 0) {
        builtin_storage(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "keymap") == 0) {
        builtin_keymap(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "return") == 0) {
        builtin_show_return(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "ifconfig") == 0) {
        builtin_ifconfig(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "hardware") == 0) {
        builtin_hardware(argc, argv);
        builtin_handled = 1;
    } else if (strcmp(name, "run") == 0) {
        /* Find the part of cmdline after "run " */
        const char *run_args = cmdline;
        while (*run_args == ' ' || *run_args == '\t') run_args++;
        if (strncmp(run_args, "run", 3) == 0) run_args += 3;
        while (*run_args == ' ' || *run_args == '\t') run_args++;
        int res = run_cmd_execute(run_args);
        cmdline_free_args(argc, argv);
        if (res < 0) return res;
        process_set_last_exit_code(res);
        return 1000;
    } else if (strcmp(name, "startwm") == 0) {
        /* Start the WM video-mode session */
        if (!wm_session_active()) {
            wm_session_start();
        }
        if (argc >= 2) {
            char *sub_path = resolve_command_path(argv[1]);
            if (sub_path) {
                process_exec(sub_path, argc - 1, argv + 1);
                kfree(sub_path);
            } else {
                printf("startwm: '%s' not found\n", argv[1]);
            }
        }
        builtin_handled = 1;
    } else if (strcmp(name, "stopwm") == 0) {
        /* Stop the WM session and return to text mode */
        wm_session_stop();
        builtin_handled = 1;
    } else if (driver_dispatch_command(name, argc, argv) == 0) {
        builtin_handled = 1;
    }

    if (builtin_handled) {
        process_set_last_exit_code(0);
        cmdline_free_args(argc, argv);
        return 1000; // Special code for builtin success
    }

    // Resolve to filesystem path for external executables
    char *path = resolve_command_path(name);
    if (!path) {
        cmdline_free_args(argc, argv);
        return 0; // not found
    }

    // Execute program with arguments (supports ELF, scripts, and flat binaries natively)
    int result = universal_process_execute(path, argc, argv);

    // Free allocated argument copies
    cmdline_free_args(argc, argv);
    kfree(path);
    return result;
}

void terminal_poll_serial_commands(void) {
    static bool serial_polling = false;
    if (serial_polling) return;
    serial_polling = true;

    static bool serial_wm_prompt_shown = false;

    /* When WM session is active, show '> ' prompt via serial once per input cycle */
    if (wm_session_active() && !serial_wm_prompt_shown && s_serial_cmd_len == 0) {
        serial_printf("> ");
        serial_wm_prompt_shown = true;
    }
    /* If WM session ended, reset prompt state */
    if (!wm_session_active()) {
        serial_wm_prompt_shown = false;
    }

    int ch;
    while ((ch = serial_getc()) >= 0) {
        if (ch == '\n' || ch == '\r') {
            serial_putc('\n');
            if (s_serial_cmd_len > 0 && s_serial_cmd_buf != NULL) {
                s_serial_cmd_buf[s_serial_cmd_len] = '\0';
                serial_printf("[serial] exec: %s\n", s_serial_cmd_buf);

                push_command_history(s_serial_cmd_buf);

                int exec = try_execute_command(s_serial_cmd_buf);
                if (exec == 0) {
                    serial_printf("Command not found: %s\n", s_serial_cmd_buf);
                    if (!wm_session_active()) {
                        printf("Command not found: %s\n", s_serial_cmd_buf);
                    }
                } else if (exec < 0) {
                    if (process_get_exit_code() != 126) {
                        serial_printf("Execution failed (error %d): %s\n", exec, s_serial_cmd_buf);
                        if (!wm_session_active()) {
                            printf("Execution failed (error %d): %s\n", exec, s_serial_cmd_buf);
                        }
                    }
                } else if (exec != 1000) {
                    if (terminal_get_show_return_value()) {
                        if (!wm_session_active()) {
                            process_print_last_return_value();
                        }
                    }
                }

                s_serial_cmd_len = 0;
                if (s_serial_cmd_buf != NULL) {
                    s_serial_cmd_buf[0] = '\0';
                }
                serial_wm_prompt_shown = false;

                if (!wm_session_active()) {
                    print_prompt();
                }
            } else {
                serial_wm_prompt_shown = false;
                if (!wm_session_active()) {
                    print_prompt();
                }
            }
        } else if (ch == 0x08 || ch == 0x7F) {
            /* Backspace / DEL */
            if (s_serial_cmd_len > 0) {
                s_serial_cmd_len--;
                if (s_serial_cmd_buf != NULL) {
                    s_serial_cmd_buf[s_serial_cmd_len] = '\0';
                }
                serial_putc('\b');
                serial_putc(' ');
                serial_putc('\b');
            }
        } else if (ch == 0x03) {
            /* Ctrl+C over serial */
            serial_printf("^C\n");
            sound_stop();
            system_request_interrupt();
            if (wm_session_active()) {
                wm_session_stop();
                print_prompt();
            }
            s_serial_cmd_len = 0;
            if (s_serial_cmd_buf) s_serial_cmd_buf[0] = '\0';
            serial_wm_prompt_shown = false;
        } else if (ch >= 0x20 || ch == '\t') {
            if (s_serial_cmd_buf == NULL || s_serial_cmd_len + 2u > s_serial_cmd_cap) {
                size_t new_cap = (s_serial_cmd_cap == 0) ? 256u : s_serial_cmd_cap * 2u;
                char *new_buf = (char *)kmalloc(new_cap);
                if (new_buf != NULL) {
                    if (s_serial_cmd_buf != NULL && s_serial_cmd_len > 0) {
                        memcpy(new_buf, s_serial_cmd_buf, s_serial_cmd_len);
                        kfree(s_serial_cmd_buf);
                    }
                    s_serial_cmd_buf = new_buf;
                    s_serial_cmd_cap = new_cap;
                }
            }
            if (s_serial_cmd_buf != NULL && s_serial_cmd_len + 1u < s_serial_cmd_cap) {
                s_serial_cmd_buf[s_serial_cmd_len++] = (char)ch;
                s_serial_cmd_buf[s_serial_cmd_len] = '\0';
                serial_putc((char)ch); /* echo */
            }
        }
    }

    serial_polling = false;
}

void terminal_console(void){
    /* Poll and process serial commands in all states, including active WM */
    terminal_poll_serial_commands();

    if (terminal_input_locked || system_is_input_state() || system_get_state() == SYSTEM_STATE_PROCESS_RUNNING) {
        return;
    }

    if (process_get_current() == NULL &&
        system_get_state() != SYSTEM_STATE_TERMINAL_IDLE &&
        system_get_state() != SYSTEM_STATE_BOOT) {
        system_set_state(SYSTEM_STATE_TERMINAL_IDLE);
    }

    /* When the WM session is active, run the compositor directly and return.
       Must happen BEFORE driver_dispatch_tick() and terminal_language_bar_tick()
       to prevent text-mode VGA writes (cursor blink, language bar rendering)
       from corrupting the Mode 13h CRTC registers. */
    if (wm_session_active()) {
        wm_compositor_tick();
        return;
    }

    terminal_language_bar_tick();
    driver_dispatch_tick();

    uint8_t scancode = keyboard_get_scancode();

    if (!prompt_shown) print_prompt();
    terminal_apply_pending_input();

    if (scancode != 0x00) {
        update_hot_key_state(scancode);

        bool is_break_code = (scancode & 0x80) != 0;

        /* Check external driver key filter first */
        if (driver_dispatch_key(scancode, is_break_code)) {
            return;
        }


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

        /* Default Ctrl + C: clear current command input */
        if (!is_break_code && keyboard_is_ctrl_pressed() && scancode == 0x2E) {
            sound_stop();
            system_clear_interrupt();
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
                if (*active_top_count() > 0) {
                    terminal_scroll_up();
                }
                return;
            }
            if (scancode == SC_PAGE_DOWN) {
                if (*active_bottom_count() > 0) {
                    terminal_scroll_down();
                }
                return;
            }
            if (scancode == SC_ARROW_UP) {
                terminal_return_to_present();
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

                    if (command_history_index < 0) {
                        size_t snapshot_len = input_buf ? strlen(input_buf) : 0u;
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
                        if (input_buf != NULL) {
                            strcpy(current_input_snapshot, input_buf);
                        } else {
                            current_input_snapshot[0] = '\0';
                        }
                        command_history_index = command_history_count - 1;
                    } else if (command_history_index > 0) {
                        command_history_index--;
                    }

                    apply_history_position();
                }
                return;
            }
            if (scancode == SC_ARROW_DOWN) {
                terminal_return_to_present();
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
                    size_t step = 1;
                    while (cursor_pos >= step + 1u &&
                           ((unsigned char)input_buf[cursor_pos - step] & 0xC0) == 0x80) {
                        step++;
                    }
                    cursor_pos -= step;
                    render_input_line(input_len);
                }
                return;
            }
            if (scancode == SC_ARROW_RIGHT) {
                terminal_return_to_present();
                if (cursor_pos < input_len) {
                    size_t step = 1;
                    while (cursor_pos + step < input_len &&
                           ((unsigned char)input_buf[cursor_pos + step] & 0xC0) == 0x80) {
                        step++;
                    }
                    cursor_pos += step;
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
                    size_t del_bytes = 1;
                    while (cursor_pos + del_bytes < input_len &&
                           ((unsigned char)input_buf[cursor_pos + del_bytes] & 0xC0) == 0x80) {
                        del_bytes++;
                    }
                    size_t old_len = input_len;
                    memmove(&input_buf[cursor_pos], &input_buf[cursor_pos + del_bytes], input_len - (cursor_pos + del_bytes) + 1);
                    input_len -= del_bytes;
                    input_buf[input_len] = '\0';
                    render_input_line(old_len);
                }
                return;
            }
        }

        if (!is_break_code) {
            const char *out_str = keyboard_get_key_string(scancode);
            if (out_str != NULL && out_str[0] != '\0') {
                // Return to present when user starts typing
                terminal_return_to_present();
                
                /* Handle newline / carriage return */
                if (out_str[0] == '\n' || out_str[0] == '\r') {
                    terminal_suppress_external_hook = true;
                    size_t vis_end = terminal_visual_offset(input_len);
                    uint16_t cur_end = input_start_cursor + (uint16_t)vis_end;
                    if (cur_end >= VGA_WIDTH * VGA_HEIGHT) {
                        cur_end = VGA_WIDTH * VGA_HEIGHT - 1;
                    }
                    vga_set_cursor(cur_end);
                    vga_hide_cursor();
                    putchar('\n');

                    if (input_buf != NULL) {
                        input_buf[input_len] = '\0';
                    }

                    if (input_len > 0 && input_buf != NULL) {
                        push_command_history(input_buf);
                        keyboard_flush_hardware();
                        keyboard_flush_queue();
                        keyboard_flush_app_queue();
                        keyboard_clear_key_state();
                        int exec = try_execute_command(input_buf);
                        if (exec == 0) {
                            printf("Command not found: %s\n", input_buf);
                        } else if (exec < 0) {
                            if (process_get_exit_code() != 126) {
                                printf("Execution failed (error %d): %s\n", exec, input_buf);
                            }
                        } else if (exec != 1000) {
                            if (terminal_show_return_value) {
                                process_print_last_return_value();
                            }
                        }
                    }

                    /* Reset buffer */
                    input_len = 0;
                    cursor_pos = 0;
                    if (input_buf != NULL) {
                        input_buf[0] = '\0';
                    }
                    if (current_input_snapshot != NULL) {
                        current_input_snapshot[0] = '\0';
                    }
                    command_history_index = -1;
                    prompt_shown = false;
                    terminal_suppress_external_hook = false;
                    /* Don't print the prompt if we just entered graphics mode */
                    if (!wm_session_active()) {
                        print_prompt();
                    }
                }
                /* Handle backspace (UTF-8 aware) */
                else if (out_str[0] == '\b' || out_str[0] == 127) {
                    if (cursor_pos > 0 && input_buf != NULL) {
                        size_t del_bytes = 1;
                        while (cursor_pos >= del_bytes + 1u &&
                               ((unsigned char)input_buf[cursor_pos - del_bytes] & 0xC0) == 0x80) {
                            del_bytes++;
                        }
                        size_t old_len = input_len;
                        memmove(&input_buf[cursor_pos - del_bytes], &input_buf[cursor_pos], input_len - cursor_pos + 1);
                        cursor_pos -= del_bytes;
                        input_len -= del_bytes;
                        input_buf[input_len] = '\0';
                        render_input_line(old_len);
                    }
                }
                /* Handle tab */
                else if (out_str[0] == '\t') {
                    ensure_input_buffer(input_len + 1u);
                    if (input_buf != NULL) {
                        size_t old_len = input_len;
                        memmove(&input_buf[cursor_pos + 1], &input_buf[cursor_pos], input_len - cursor_pos + 1);
                        input_buf[cursor_pos] = '\t';
                        cursor_pos++;
                        input_len++;
                        input_buf[input_len] = '\0';
                        render_input_line(old_len);
                    }
                }
                /* Printable string (arbitrary length: UTF-8 character, word, symbol, emoji) */
                else {
                    size_t slen = strlen(out_str);
                    ensure_input_buffer(input_len + slen + 1u);
                    if (input_buf != NULL) {
                        size_t old_len = input_len;
                        memmove(&input_buf[cursor_pos + slen], &input_buf[cursor_pos], input_len - cursor_pos + 1);
                        memcpy(&input_buf[cursor_pos], out_str, slen);
                        cursor_pos += slen;
                        input_len += slen;
                        input_buf[input_len] = '\0';
                        render_input_line(old_len);
                    }
                }
            }
        }
    }
}

/* Auto scroll when terminal overflows - keep only current screen in RAM */
void terminal_auto_scroll(void) {
    volatile uint16_t *vga = VGA_MEMORY;

    vga_cursor_erase();

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
    *active_bottom_count() = 0;

    for (uint16_t r = 0; r < rows - 1; r++) {
        uint16_t src_offset = (top + r + 1) * VGA_WIDTH;
        uint16_t dst_offset = (top + r) * VGA_WIDTH;

        for (uint16_t c = 0; c < VGA_WIDTH; c++) {
            uint16_t val = vga[src_offset + c];
            if ((val & 0xFF) == VGA_CURSOR_GLYPH_SLOT) {
                val = ((val >> 8) << 8) | ' ';
            }
            vga[dst_offset + c] = val;
        }
    }

    uint16_t blank = vga_entry(' ', VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    uint16_t bottom_offset = (top + rows - 1) * VGA_WIDTH;

    for (uint16_t c = 0; c < VGA_WIDTH; c++) {
        vga[bottom_offset + c] = blank;
    }

    if (process_is_batch_active() && process_get_separate_windows()) {
        process_t *fg = process_get_foreground();
        if (fg && fg->text_vram_backup) {
            memcpy(fg->text_vram_backup, (const void *)vga, 80 * 25 * sizeof(uint16_t));
        }
    }
}