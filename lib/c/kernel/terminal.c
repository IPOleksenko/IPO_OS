#include <kernel/terminal.h>

#include <vga.h>
#include <driver/keyboard.h>
#include <driver/input/keymap/keymap.h>
#include <file_system/ipo_fs.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>


#define TERM_BUFFER_SIZE (256 * 1024) // 256KB history buffer
#define MAX_LINE_OFFSETS 65536        // max number of lines remembered
#define INPUT_BUFFER_SIZE (16 * 1024) // max current input size

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

static char term_buffer[TERM_BUFFER_SIZE];
static size_t term_buffer_size = 0; // used bytes
static uint32_t line_offsets[MAX_LINE_OFFSETS];
static uint32_t line_count = 0; // number of lines in history (line_offsets entries)

static char input_buffer[INPUT_BUFFER_SIZE];
static size_t input_size = 0; // bytes in current input

// scroll offset in lines (0 == bottom / latest)
static uint32_t view_offset = 0;

// Terminal drawing area (below header)
static inline uint16_t terminal_top_row(void) {
    return VGA_START_CURSOR_POSITION / VGA_WIDTH;
}

static inline uint16_t terminal_rows(void) {
    return VGA_HEIGHT - terminal_top_row();
}

static void append_char_to_history(char c) {
    if (term_buffer_size + 1 >= TERM_BUFFER_SIZE) {
        // drop oldest data: simple sliding window remove first quarter to free space
        size_t drop = TERM_BUFFER_SIZE / 4;
        // memmove replacement
        size_t remain = term_buffer_size - drop;
        for (size_t i = 0; i < remain; i++) {
            term_buffer[i] = term_buffer[drop + i];
        }
        term_buffer_size -= drop;
        // recompute line_offsets
        uint32_t new_count = 0;
        for (uint32_t i = 0; i < line_count; i++) {
            uint32_t off = line_offsets[i];
            if (off >= drop) {
                line_offsets[new_count++] = off - drop;
            }
        }
        line_count = new_count;
    }

    term_buffer[term_buffer_size++] = c;
}

static void append_string_to_history(const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        append_char_to_history(s[i]);
    }
}

// commit current input to history (handles wrapping and newlines)
static void recompute_line_offsets(void) {
    line_count = 0;
    if (term_buffer_size == 0) return;

    uint16_t col = 0;
    uint16_t width = VGA_WIDTH;

    if (line_count < MAX_LINE_OFFSETS) line_offsets[line_count++] = 0;

    for (size_t i = 0; i < term_buffer_size; i++) {
        char ch = term_buffer[i];
        if (ch == '\n') {
            size_t next = i + 1;
            if (next < term_buffer_size && line_count < MAX_LINE_OFFSETS) {
                line_offsets[line_count++] = next;
            }
            col = 0;
        } else {
            col++;
            if (col >= width) {
                size_t next = i + 1;
                if (next < term_buffer_size && line_count < MAX_LINE_OFFSETS) {
                    line_offsets[line_count++] = next;
                }
                col = 0;
            }
        }
    }
}

static void commit_input_to_history(void) {
    // Append the input chars and a terminating newline
    append_string_to_history(input_buffer, input_size);
    append_char_to_history('\n');
    recompute_line_offsets();
    // reset input
    input_size = 0;
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

static void render(void) {
    volatile uint16_t* vga = VGA_MEMORY;
    uint16_t top = terminal_top_row();
    uint16_t rows = terminal_rows();

    // compute input rows (total needed to render whole input)
    uint16_t input_total_rows = (input_size == 0) ? 1 : (input_size + VGA_WIDTH - 1) / VGA_WIDTH;
    if (input_total_rows > rows) input_total_rows = rows; // clamp to screen height

    // determine how many history rows we can show above the input
    uint16_t desired_history_rows = rows - input_total_rows;
    uint32_t available_history = (line_count > 0) ? (line_count) : 0;
    uint16_t displayed_history = (available_history > desired_history_rows) ? desired_history_rows : (uint16_t)available_history;

    // clamp view_offset so user cannot scroll above available history
    uint32_t max_offset = (available_history > displayed_history) ? (available_history - displayed_history) : 0;
    if (view_offset > max_offset) view_offset = max_offset;

    // apply view_offset (scrollback): move the window up by view_offset lines
    uint32_t start_line = 0;
    if (available_history > displayed_history + view_offset) {
        start_line = available_history - displayed_history - view_offset;
    } else {
        start_line = 0;
    }

    clear_terminal_area();

    // render history lines (top-down)
    uint16_t draw_row = 0;
    for (uint32_t li = start_line; li < line_count && draw_row < displayed_history; li++, draw_row++) {
        size_t off = line_offsets[li];
        size_t end = (li + 1 < line_count) ? line_offsets[li + 1] : term_buffer_size;
        uint16_t col = 0;
        uint16_t vga_row = top + draw_row;
        uint16_t vga_off = vga_row * VGA_WIDTH;
        for (size_t p = off; p < end; p++) {
            char ch = term_buffer[p];
            if (ch == '\n') break;
            vga[vga_off + col] = vga_entry((unsigned char)ch, VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            col++;
            if (col >= VGA_WIDTH) break;
        }
        // fill rest with blanks
        for (uint16_t c = col; c < VGA_WIDTH; c++) {
            vga[vga_off + c] = vga_entry(0x00, VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }
    }

    // compute input area placement: directly under last displayed history line
    uint16_t input_start_row = top + displayed_history;
    uint16_t avail_rows_for_input = rows - displayed_history;
    uint16_t input_visible_rows = (input_total_rows > avail_rows_for_input) ? avail_rows_for_input : input_total_rows;

    // compute full input rows taking prompt into account
    uint16_t first_row_capacity = VGA_WIDTH - PROMPT_LEN;
    uint32_t input_total_rows_full;
    if (input_size == 0) {
        input_total_rows_full = 1;
    } else if (input_size <= (size_t)first_row_capacity) {
        input_total_rows_full = 1;
    } else {
        size_t rem = input_size - first_row_capacity;
        input_total_rows_full = 1 + (uint32_t)((rem + VGA_WIDTH - 1) / VGA_WIDTH);
    }

    // if input is taller than available, show the last part of the input (skip earlier rows)
    uint32_t skip_rows = 0;
    if (input_total_rows_full > input_visible_rows) {
        skip_rows = input_total_rows_full - input_visible_rows;
    }

    // compute skip_chars (how many characters to skip from start of input)
    size_t skip_chars = 0;
    if (skip_rows == 0) {
        skip_chars = 0;
    } else {
        // skip the whole first row capacity if skip_rows >=1
        skip_chars = (size_t)first_row_capacity;
        if (skip_rows > 1) {
            skip_chars += (size_t)(skip_rows - 1) * VGA_WIDTH;
        }
        if (skip_chars > input_size) skip_chars = input_size;
    }

    // render visible portion of input row-by-row (starting from skip_chars)
    uint16_t cur_row = input_start_row;
    size_t idx = skip_chars;
    for (uint16_t r = 0; r < input_visible_rows && cur_row < top + rows; r++, cur_row++) {
        uint16_t vga_off = cur_row * VGA_WIDTH;
        uint16_t col = 0;

        // if this is the first visible input row, render prompt at column 0
        if (r == 0) {
            // prompt chars
            for (uint16_t p = 0; p < PROMPT_LEN; p++) {
                vga[vga_off + col] = vga_entry((unsigned char)PROMPT_STR[p], PROMPT_FG, VGA_COLOR_BLACK);
                col++;
            }
            // fill rest of first row
            uint16_t capacity = first_row_capacity;
            for (uint16_t j = 0; j < capacity; j++) {
                if (idx < input_size) {
                    vga[vga_off + col] = vga_entry((unsigned char)input_buffer[idx++], INPUT_FG, VGA_COLOR_BLACK);
                } else {
                    vga[vga_off + col] = vga_entry(0x00, VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                }
                col++;
            }
        } else {
            // full row
            for (uint16_t j = 0; j < VGA_WIDTH; j++) {
                if (idx < input_size) {
                    vga[vga_off + j] = vga_entry((unsigned char)input_buffer[idx++], INPUT_FG, VGA_COLOR_BLACK);
                } else {
                    vga[vga_off + j] = vga_entry(0x00, VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                }
            }
        }
    }

    // fill remaining input rows (if any) with blanks
    for (; cur_row < top + rows; cur_row++) {
        uint16_t off = cur_row * VGA_WIDTH;
        for (uint16_t c = 0; c < VGA_WIDTH; c++) {
            vga[off + c] = vga_entry(0x00, VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }
    }

    // set cursor to end of visible input (on-screen)
    size_t visible_input_chars = (input_size > skip_chars) ? (input_size - skip_chars) : 0;
    uint16_t cursor_row_offset;
    uint16_t cursor_col;
    if (visible_input_chars <= (size_t)first_row_capacity) {
        cursor_row_offset = 0;
        cursor_col = PROMPT_LEN + (uint16_t)visible_input_chars;
    } else {
        size_t after_first = visible_input_chars - (size_t)first_row_capacity;
        cursor_row_offset = 1 + (uint16_t)((after_first) / VGA_WIDTH);
        cursor_col = (uint16_t)((after_first) % VGA_WIDTH);
    }
    uint16_t cursor_row = (uint16_t)(input_start_row + cursor_row_offset);
    vga_set_cursor(cursor_row * VGA_WIDTH + cursor_col);
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

/* Append a printed line to the terminal history and re-render */
void terminal_print(const char *s) {
    if (!s) return;
    size_t len = 0; while (s[len]) len++;
    append_string_to_history(s, len);
    append_char_to_history('\n');
    recompute_line_offsets();
    render();
}

static void cmd_help(const char *arg) {
    if (!arg || arg[0] == '\0') {
        terminal_print("Available commands: mkdir touch ls rm cat write append mv stat help");
        terminal_print("Usage: mkdir <path> - create directory");
        terminal_print("       touch <path> - create file");
        terminal_print("       ls [path] - list directory");
        terminal_print("       rm <path> - delete file or empty dir");
        terminal_print("       cat <path> - print file");
        terminal_print("       write <path> <text> - overwrite file");
        terminal_print("       append <path> <text> - append to file");
        terminal_print("       mv <src> <dst> - rename/move");
        terminal_print("       stat <path> - file info");
        terminal_print("       help [cmd] - this help");
    } else {
        if (strncmp(arg, "mkdir", 5) == 0) terminal_print("mkdir <path> - create directory");
        else if (strncmp(arg, "touch", 5) == 0) terminal_print("touch <path> - create file");
        else if (strncmp(arg, "ls", 2) == 0) terminal_print("ls [path] - list directory");
        else if (strncmp(arg, "rm", 2) == 0) terminal_print("rm <path> - delete file or empty dir");
        else if (strncmp(arg, "cat", 3) == 0) terminal_print("cat <path> - print file");
        else if (strncmp(arg, "write", 5) == 0) terminal_print("write <path> <text> - overwrite file");
        else if (strncmp(arg, "append", 6) == 0) terminal_print("append <path> <text> - append to file");
        else if (strncmp(arg, "mv", 2) == 0) terminal_print("mv <src> <dst> - rename/move");
        else if (strncmp(arg, "stat", 4) == 0) terminal_print("stat <path> - file info");
        else terminal_print("Unknown command for help");
    }
}

void terminal_initialize(void) {
    vga_clear(VGA_COLOR_WHITE, VGA_COLOR_BLACK, true, VGA_START_CURSOR_POSITION);

    print_header();
    // initialize history as empty
    term_buffer_size = 0;
    line_count = 0;
    input_size = 0;
    view_offset = 0;

    // render initial prompt
    render();
}

void handle_control_char(char c) {
    switch(c) {
        case '\n': {
            /* capture the command before clearing input */
            char cmd[INPUT_BUFFER_SIZE];
            size_t cmd_len = (input_size < (INPUT_BUFFER_SIZE-1)) ? input_size : (INPUT_BUFFER_SIZE-1);
            for (size_t i = 0; i < cmd_len; i++) cmd[i] = input_buffer[i];
            cmd[cmd_len] = '\0';

            /* commit input to history */
            commit_input_to_history();
            if (view_offset != 0) view_offset = 0; // go to bottom on new input
            render();

            /* process commands with minimal in-terminal parsing */
            /* trim leading spaces */
            char *p = cmd;
            while (*p == ' ') p++;
            if (strncmp(p, "mkdir", 5) == 0 && (p[5] == ' ' || p[5] == '\0')) {
                char *arg = p + 5; while (*arg == ' ') arg++;
                if (*arg == '\0') terminal_print("mkdir: missing path");
                else if (ipo_fs_create(arg, IPO_INODE_TYPE_DIR) >= 0) terminal_print("mkdir: OK"); else terminal_print("mkdir: failed");
            } else if (strncmp(p, "touch", 5) == 0 && (p[5] == ' ' || p[5] == '\0')) {
                char *arg = p + 5; while (*arg == ' ') arg++;
                if (*arg == '\0') terminal_print("touch: missing path");
                else if (ipo_fs_create(arg, IPO_INODE_TYPE_FILE) >= 0) terminal_print("touch: OK"); else terminal_print("touch: failed");
            } else if (strncmp(p, "ls", 2) == 0 && (p[2] == ' ' || p[2] == '\0')) {
                char *arg = p + 2; while (*arg == ' ') arg++;
                char *path = (*arg == '\0') ? "/" : arg;
                char out[4096]; int r = ipo_fs_list_dir(path, out, sizeof(out));
                if (r < 0) terminal_print("ls: failed"); else terminal_print(out);
            } else if (strncmp(p, "rm", 2) == 0 && (p[2] == ' ' || p[2] == '\0')) {
                char *arg = p + 2; while (*arg == ' ') arg++;
                if (*arg == '\0') terminal_print("rm: missing path");
                else if (ipo_fs_delete(arg)) terminal_print("rm: OK"); else terminal_print("rm: failed");
            } else if (strncmp(p, "cat", 3) == 0 && (p[3] == ' ' || p[3] == '\0')) {
                char *arg = p + 3; while (*arg == ' ') arg++;
                if (*arg == '\0') { terminal_print("cat: missing path"); }
                else {
                    struct ipo_inode st;
                    if (!ipo_fs_stat(arg, &st)) { terminal_print("cat: failed"); }
                    else if ((st.mode & IPO_INODE_TYPE_DIR) != 0) { terminal_print("cat: path is a directory"); }
                    else {
                        int fd = ipo_fs_open(arg);
                        if (fd < 0) { terminal_print("cat: failed"); }
                        else {
                            uint32_t remaining = st.size; uint32_t offset = 0; char buf[512+1];
                            while (remaining > 0) {
                                uint32_t r = (remaining > 512) ? 512 : remaining;
                                int got = ipo_fs_read(fd, buf, r, offset);
                                if (got <= 0) break;
                                buf[got] = '\0'; terminal_print(buf);
                                remaining -= got; offset += got;
                            }
                        }
                    }
                }
            } else if (strncmp(p, "mv", 2) == 0 && (p[2] == ' ' || p[2] == '\0')) {
                char *arg = p + 2; while (*arg == ' ') arg++;
                char *space = strchr(arg, ' ');
                if (!space) { terminal_print("mv: usage: mv <src> <dst>"); }
                else {
                    size_t sl = space - arg; char src[INPUT_BUFFER_SIZE]; char dst[INPUT_BUFFER_SIZE];
                    if (sl >= sizeof(src)) { terminal_print("mv: src too long"); }
                    else {
                        strncpy(src, arg, sl); src[sl] = '\0';
                        char *d = space + 1; while (*d == ' ') d++;
                        size_t dl = 0; const char *q = d; while (*q && *q != ' ') { q++; dl++; }
                        if (dl == 0) { terminal_print("mv: usage: mv <src> <dst>"); }
                        else if (dl >= sizeof(dst)) { terminal_print("mv: dst too long"); }
                        else {
                            strncpy(dst, d, dl); dst[dl] = '\0';
                            if (ipo_fs_rename(src, dst)) terminal_print("mv: OK"); else terminal_print("mv: failed");
                        }
                    }
                }
            } else if (strncmp(p, "write", 5) == 0 && (p[5] == ' ' || p[5] == '\0')) {
                char *arg = p + 5; while (*arg == ' ') arg++;
                char *space = strchr(arg, ' ');
                if (!space) { terminal_print("write: usage: write <path> <text>"); }
                else {
                    size_t plen = space - arg; char path[INPUT_BUFFER_SIZE]; char text[INPUT_BUFFER_SIZE];
                    if (plen >= sizeof(path)) { terminal_print("write: path too long"); }
                    else {
                        strncpy(path, arg, plen); path[plen] = '\0';
                        char *t = space + 1; while (*t == ' ') t++;
                        if (*t == '\0') { terminal_print("write: missing text"); }
                        else { strncpy(text, t, sizeof(text)-1); text[sizeof(text)-1] = '\0'; if (ipo_fs_write_text(path, text, false)) terminal_print("write: OK"); else terminal_print("write: failed"); }
                    }
                }
            } else if (strncmp(p, "append", 6) == 0 && (p[6] == ' ' || p[6] == '\0')) {
                char *arg = p + 6; while (*arg == ' ') arg++;
                char *space = strchr(arg, ' ');
                if (!space) { terminal_print("append: usage: append <path> <text>"); }
                else {
                    size_t plen = space - arg; char path[INPUT_BUFFER_SIZE]; char text[INPUT_BUFFER_SIZE];
                    if (plen >= sizeof(path)) { terminal_print("append: path too long"); }
                    else {
                        strncpy(path, arg, plen); path[plen] = '\0';
                        char *t = space + 1; while (*t == ' ') t++;
                        if (*t == '\0') { terminal_print("append: missing text"); }
                        else { strncpy(text, t, sizeof(text)-1); text[sizeof(text)-1] = '\0'; if (ipo_fs_write_text(path, text, true)) terminal_print("append: OK"); else terminal_print("append: failed"); }
                    }
                }
            } else if (strncmp(p, "stat", 4) == 0 && (p[4] == ' ' || p[4] == '\0')) {
                char *arg = p + 4; while (*arg == ' ') arg++;
                if (*arg == '\0') terminal_print("stat: missing path"); else {
                    struct ipo_inode st; if (!ipo_fs_stat(arg, &st)) { terminal_print("stat: failed"); }
                    else {
                        char out[128]; char numbuf[32]; if (st.mode & IPO_INODE_TYPE_DIR) { strcpy(out, "type=dir size="); } else { strcpy(out, "type=file size="); }
                        size_t pos = strlen(out); int n = itoa(st.size, numbuf, 10); if (n < 0) n = 0; if (n > (int)sizeof(numbuf)-1) n = sizeof(numbuf)-1; numbuf[n] = '\0'; memcpy(out + pos, numbuf, n); pos += n; out[pos] = '\0'; const char *links_str = " links="; size_t ls = 0; while (links_str[ls]) { out[pos++] = links_str[ls++]; } int m = itoa(st.links_count, numbuf, 10); if (m < 0) m = 0; if (m > (int)sizeof(numbuf)-1) m = sizeof(numbuf)-1; numbuf[m] = '\0'; memcpy(out + pos, numbuf, m); pos += m; out[pos] = '\0'; terminal_print(out);
                    }
                }
            } else if (strncmp(p, "help", 4) == 0 && (p[4] == ' ' || p[4] == '\0')) {
                char *arg = p + 4; while (*arg == ' ') arg++; cmd_help(arg);
            } else {
                if (p[0] != '\0') terminal_print("unknown command, type 'help'");
            }
            break;
        }
        case '\t': {
            // simple tab = 4 spaces
            if (input_size + 4 < INPUT_BUFFER_SIZE) {
                for (int i = 0; i < 4; i++) input_buffer[input_size++] = ' ';
                if (view_offset != 0) view_offset = 0;
                render();
            }
            break;
        }
        case '\b': {
            if (input_size > 0) {
                input_size--;
                if (view_offset != 0) view_offset = 0;
                render();
            }
            break;
        }
    }
}

void terminal_console(void){
    uint8_t scancode = keyboard_get_scancode();
    volatile uint16_t* vga = VGA_MEMORY;
        
    if (scancode != 0x00) {
        update_hot_key_state(scancode);
        hot_key_handler(scancode);
        
        bool is_break_code = (scancode & 0x80) != 0;
        
        if (!is_break_code) {
            // navigation keys for scrolling
            if (scancode == SC_PAGE_UP) {
                uint16_t rows = terminal_rows();
                view_offset += rows;
                // clamp using current displayed window
                uint16_t input_rows = (input_size == 0) ? 1 : (input_size + VGA_WIDTH - 1) / VGA_WIDTH;
                uint16_t history_rows = terminal_rows() - ((input_rows > terminal_rows()) ? terminal_rows() : input_rows);
                uint32_t max_off = (line_count > history_rows) ? (line_count - history_rows) : 0;
                if (view_offset > max_off) view_offset = max_off;
                render();
                return;
            }
            if (scancode == SC_PAGE_DOWN) {
                uint16_t rows = terminal_rows();
                if (view_offset <= rows) view_offset = 0; else view_offset -= rows;
                render();
                return;
            }
            if (scancode == SC_ARROW_UP) {
                // clamp: cannot exceed available lines
                uint16_t input_rows = (input_size == 0) ? 1 : (input_size + VGA_WIDTH - 1) / VGA_WIDTH;
                uint16_t history_rows = terminal_rows() - ((input_rows > terminal_rows()) ? terminal_rows() : input_rows);
                uint32_t max_off = (line_count > history_rows) ? (line_count - history_rows) : 0;
                if (view_offset < max_off) view_offset++; else view_offset = max_off;
                render();
                return;
            }
            if (scancode == SC_ARROW_DOWN) {
                if (view_offset > 0) view_offset--;
                render();
                return;
            }

            char c = get_char(scancode);
            if (c != 0x00) {
                // reset view offset (go to bottom) on typing
                if (c != '\n' && c != '\b') {
                    view_offset = 0;
                }

                if (c == '\n' || c == '\t' || c == '\b') {
                    handle_control_char(c);
                } else {
                    // printable char: append to input buffer
                    if (input_size + 1 < INPUT_BUFFER_SIZE) {
                        input_buffer[input_size++] = c;
                        render();
                    }
                }
            }
        }
    }
}
