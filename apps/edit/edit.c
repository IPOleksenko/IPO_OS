/**
 * edit - Vim-like Full-Screen Modal Text Editor for IPO_OS
 *
 * Features:
 *  - Modes: NORMAL, INSERT, COMMAND (:)
 *  - Full 80x25 screen visual editing with line numbers and '~' for empty lines
 *  - Free cursor navigation (arrows, hjkl, Home/End, PgUp/PgDn, w, b, 0, $, gg, G)
 *  - Insert mode (i, I, a, A, o, O) with full character insertion, backspace, enter, tab
 *  - Delete commands (x, dd, D), join (J), undo (u)
 *  - Command mode (:w, :q, :wq, :q!, :help)
 *  - Smooth horizontal and vertical viewport scrolling
 *  - Double-buffered flicker-free VGA rendering
 *  - Restores original terminal screen on exit
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <vga.h>
#include <ioport.h>
#include <memory/kmalloc.h>
#include <file_system/ipo_fs.h>
#include <driver/input/keyboard.h>
#include <driver/input/keymap/keymap.h>

#define MAX_LINES 2048
#define MAX_LINE_LEN 256
#define VIEW_ROWS 24
#define GUTTER_WIDTH 5
#define TEXT_COLS (VGA_WIDTH - GUTTER_WIDTH)

/* Editor modes */
typedef enum {
    MODE_NORMAL = 0,
    MODE_INSERT,
    MODE_COMMAND
} editor_mode_t;

/* Scancodes */
#define SC_ESC        0x01
#define SC_BACKSPACE  0x0E
#define SC_TAB        0x0F
#define SC_ENTER      0x1C
#define SC_ARROW_UP   0x48
#define SC_ARROW_DOWN 0x50
#define SC_ARROW_LEFT 0x4B
#define SC_ARROW_RIGHT 0x4D
#define SC_PAGE_UP    0x49
#define SC_PAGE_DOWN  0x51
#define SC_HOME       0x47
#define SC_END        0x4F
#define SC_DELETE     0x53

/* Editor state */
static char lines[MAX_LINES][MAX_LINE_LEN];
static int line_count = 0;
static char current_file[128];
static bool is_modified = false;
static bool show_line_numbers = true;
static bool show_help_screen = false;
static editor_mode_t mode = MODE_NORMAL;

/* Cursor and viewport */
static int cur_line = 0;   /* 0 .. line_count - 1 */
static int cur_col = 0;    /* 0 .. strlen(lines[cur_line]) */
static int row_offset = 0; /* line at top of viewport */
static int col_offset = 0; /* column at left of text area */

/* Command mode buffer */
static char cmd_buf[64];
static int cmd_len = 0;
static char status_msg[128];
static uint32_t status_timer = 0;

/* Undo single step */
static char undo_lines[MAX_LINES][MAX_LINE_LEN];
static int undo_count = 0;
static int undo_cur_line = 0;
static int undo_cur_col = 0;
static bool can_undo = false;

/* Screen preservation & double-buffering */
static uint16_t saved_screen[VGA_WIDTH * VGA_HEIGHT];
static uint16_t saved_cursor = 0;
static uint16_t back_buffer[VGA_WIDTH * VGA_HEIGHT];

/* Pending normal command state (e.g. 'd' for 'dd', 'g' for 'gg') */
static char pending_op = '\0';


static void my_strcat(char *dst, const char *src, size_t max_len) {
    size_t dlen = strlen(dst);
    while (*src && dlen + 1 < max_len) {
        dst[dlen++] = *src++;
    }
    dst[dlen] = '\0';
}

static void save_undo_state(void) {
    if (line_count > MAX_LINES) line_count = MAX_LINES;
    for (int i = 0; i < line_count; i++) {
        strncpy(undo_lines[i], lines[i], MAX_LINE_LEN - 1);
        undo_lines[i][MAX_LINE_LEN - 1] = '\0';
    }
    undo_count = line_count;
    undo_cur_line = cur_line;
    undo_cur_col = cur_col;
    can_undo = true;
}

static void restore_undo_state(void) {
    if (!can_undo) {
        snprintf(status_msg, sizeof(status_msg), "Already at oldest change");
        return;
    }
    char temp_lines[MAX_LINES][MAX_LINE_LEN];
    int temp_count = line_count;
    int temp_cur_line = cur_line;
    int temp_cur_col = cur_col;
    for (int i = 0; i < line_count; i++) {
        strncpy(temp_lines[i], lines[i], MAX_LINE_LEN - 1);
        temp_lines[i][MAX_LINE_LEN - 1] = '\0';
    }

    line_count = undo_count;
    for (int i = 0; i < line_count; i++) {
        strncpy(lines[i], undo_lines[i], MAX_LINE_LEN - 1);
        lines[i][MAX_LINE_LEN - 1] = '\0';
    }
    cur_line = undo_cur_line;
    cur_col = undo_cur_col;

    for (int i = 0; i < temp_count; i++) {
        strncpy(undo_lines[i], temp_lines[i], MAX_LINE_LEN - 1);
        undo_lines[i][MAX_LINE_LEN - 1] = '\0';
    }
    undo_count = temp_count;
    undo_cur_line = temp_cur_line;
    undo_cur_col = temp_cur_col;

    is_modified = true;
    snprintf(status_msg, sizeof(status_msg), "1 change undone");
}

/* File operations */
static bool load_file(const char *path) {
    line_count = 0;
    is_modified = false;
    can_undo = false;

    struct ipo_inode st;
    if (ipo_stat(path, &st) != 0) {
        /* New file */
        line_count = 1;
        lines[0][0] = '\0';
        snprintf(status_msg, sizeof(status_msg), "\"%s\" [New File]", path);
        return true;
    }

    if ((st.mode & IPO_INODE_TYPE_DIR) != 0) {
        snprintf(status_msg, sizeof(status_msg), "Error: \"%s\" is a directory", path);
        line_count = 1;
        lines[0][0] = '\0';
        return false;
    }

    if (st.size == 0) {
        line_count = 1;
        lines[0][0] = '\0';
        snprintf(status_msg, sizeof(status_msg), "\"%s\" [Empty File]", path);
        return true;
    }

    int fd = ipo_open(path);
    if (fd < 0) {
        snprintf(status_msg, sizeof(status_msg), "Error: Cannot open \"%s\"", path);
        line_count = 1;
        lines[0][0] = '\0';
        return false;
    }

    char *buf = kmalloc((size_t)st.size + 2);
    if (!buf) {
        ipo_close(fd);
        snprintf(status_msg, sizeof(status_msg), "Error: Out of memory");
        line_count = 1;
        lines[0][0] = '\0';
        return false;
    }

    int rd = ipo_read(fd, buf, (uint32_t)st.size, 0);
    ipo_close(fd);

    if (rd < 0) {
        kfree(buf);
        snprintf(status_msg, sizeof(status_msg), "Error reading \"%s\"", path);
        line_count = 1;
        lines[0][0] = '\0';
        return false;
    }

    buf[rd] = '\0';

    /* Parse lines */
    int l_idx = 0;
    int c_idx = 0;
    for (int i = 0; i < rd && l_idx < MAX_LINES; i++) {
        char ch = buf[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            lines[l_idx][c_idx] = '\0';
            l_idx++;
            c_idx = 0;
        } else {
            if (c_idx < MAX_LINE_LEN - 1) {
                lines[l_idx][c_idx++] = ch;
            }
        }
    }
    if (c_idx > 0 || l_idx == 0) {
        lines[l_idx][c_idx] = '\0';
        l_idx++;
    }
    line_count = l_idx;
    kfree(buf);

    snprintf(status_msg, sizeof(status_msg), "\"%s\" %d lines, %d bytes", path, line_count, rd);
    return true;
}

static bool save_file(const char *path) {
    /* Calculate total size */
    size_t total = 0;
    for (int i = 0; i < line_count; i++) {
        total += strlen(lines[i]) + 1; /* include \n */
    }

    char *buf = kmalloc(total + 1);
    if (!buf) {
        snprintf(status_msg, sizeof(status_msg), "Error: Out of memory saving file");
        return false;
    }

    size_t pos = 0;
    for (int i = 0; i < line_count; i++) {
        size_t len = strlen(lines[i]);
        memcpy(buf + pos, lines[i], len);
        pos += len;
        buf[pos++] = '\n';
    }

    ipo_delete(path);
    ipo_create(path, IPO_INODE_TYPE_FILE);
    int fd = ipo_open(path);
    if (fd < 0) {
        kfree(buf);
        snprintf(status_msg, sizeof(status_msg), "Error: Cannot write to \"%s\"", path);
        return false;
    }

    int wr = ipo_write(fd, buf, (uint32_t)pos, 0);
    ipo_close(fd);
    kfree(buf);

    if (wr < 0) {
        snprintf(status_msg, sizeof(status_msg), "Error: Write failed for \"%s\"", path);
        return false;
    }

    is_modified = false;
    snprintf(status_msg, sizeof(status_msg), "\"%s\" %d lines, %d bytes written", path, line_count, wr);
    return true;
}

/* UTF-8 helpers for multi-byte character navigation and rendering */
static int get_screen_col(const char *line, int byte_col) {
    int col = 0;
    int idx = 0;
    int len = (int)strlen(line);
    while (idx < byte_col && idx < len) {
        size_t bytes = 1;
        utf8_to_vga_glyph(&line[idx], len - idx, &bytes);
        if (bytes == 0) bytes = 1;
        idx += (int)bytes;
        col++;
    }
    return col;
}

static int render_utf8_to_status(uint16_t *status_row, int start_col, int max_cols,
                                 const char *str, enum vga_color fg, enum vga_color bg) {
    if (!str || start_col >= max_cols) return start_col;
    int col = start_col;
    int bpos = 0;
    int len = (int)strlen(str);
    while (bpos < len && col < max_cols) {
        size_t b = 1;
        uint8_t glyph = utf8_to_vga_glyph(&str[bpos], len - bpos, &b);
        if (b == 0) b = 1;
        status_row[col++] = vga_entry(glyph, fg, bg);
        bpos += (int)b;
    }
    return col;
}

static int utf8_next_char_offset(const char *line, int cur) {
    int len = (int)strlen(line);
    if (cur >= len) return len;
    size_t bytes = 1;
    utf8_to_vga_glyph(&line[cur], len - cur, &bytes);
    if (bytes == 0) bytes = 1;
    return cur + (int)bytes;
}

static int utf8_prev_char_offset(const char *line, int cur) {
    if (cur <= 0) return 0;
    cur--;
    while (cur > 0 && ((unsigned char)line[cur] & 0xC0) == 0x80) {
        cur--;
    }
    return cur;
}

/* Adjust scrolling to keep cursor visible */
static void update_viewport(void) {
    if (cur_line < 0) cur_line = 0;
    if (cur_line >= line_count) cur_line = line_count > 0 ? line_count - 1 : 0;

    int cur_len = (int)strlen(lines[cur_line]);
    if (cur_col < 0) cur_col = 0;
    if (mode == MODE_NORMAL) {
        if (cur_col >= cur_len && cur_len > 0) cur_col = utf8_prev_char_offset(lines[cur_line], cur_len);
    } else {
        if (cur_col > cur_len) cur_col = cur_len;
    }

    /* Vertical scrolling */
    if (cur_line < row_offset) {
        row_offset = cur_line;
    }
    if (cur_line >= row_offset + VIEW_ROWS) {
        row_offset = cur_line - VIEW_ROWS + 1;
    }

    /* Horizontal scrolling based on screen character columns */
    int cur_screen_col = get_screen_col(lines[cur_line], cur_col);
    int text_width = show_line_numbers ? TEXT_COLS : VGA_WIDTH;
    if (cur_screen_col < col_offset) {
        col_offset = cur_screen_col;
    }
    if (cur_screen_col >= col_offset + text_width) {
        col_offset = cur_screen_col - text_width + 1;
    }
}

/* Full-screen Vim Help Reference Overlay */
static void render_help_screen(void) {
    static const char *help_lines[25] = {
        "========================= VIM EDITOR - HELP REFERENCE =========================",
        " MODES:",
        "   NORMAL (navigation/cmds) | INSERT (text input) | COMMAND (:)",
        " HOW TO ENTER INSERT MODE (from Normal mode):",
        "   i: Insert at cursor       | a: Append after cursor",
        "   I: Insert at line start   | A: Append at line end",
        "   o: Open line below        | O: Open line above",
        "   <ESC>: Return to Normal mode (exit insert / cancel)",
        "",
        " NAVIGATION (Normal mode):",
        "   h, j, k, l: Left, Down, Up, Right (or Arrow keys)",
        "   w, b: Next / Prev word    | 0, $: Start / End of line",
        "   gg, G: First / Last line  | PgUp, PgDn: Page scroll (Home / End)",
        "",
        " EDITING & UNDO (Normal mode):",
        "   x: Delete character under cursor | dd: Delete (cut) current line",
        "   D: Delete to line end            | J: Join with next line",
        "   u: Undo last change",
        "",
        " COMMANDS (Type ':' in Normal mode):",
        "   :w [file]   Save file to disk    | :q    Quit (fails if modified)",
        "   :wq, :x     Save file and quit   | :q!   Force quit without saving",
        "   :ru, :en, :ua Switch layout      | <F2> / Ctrl+Shift: Cycle layout",
        "==================== Press <ESC>, <F1>, or <Enter> to close ===================="
    };

    for (int r = 0; r < 25; r++) {
        const char *line = help_lines[r];
        int len = (int)strlen(line);
        uint16_t *dst_row = &back_buffer[r * VGA_WIDTH];

        enum vga_color fg = VGA_COLOR_LIGHT_GREY;
        enum vga_color bg = VGA_COLOR_BLACK;

        if (r == 0 || r == 24) {
            fg = VGA_COLOR_BLACK;
            bg = VGA_COLOR_LIGHT_CYAN;
        } else if (strncmp(line, " MODES:", 7) == 0 ||
                   strncmp(line, " HOW TO ENTER", 13) == 0 ||
                   strncmp(line, " NAVIGATION", 11) == 0 ||
                   strncmp(line, " EDITING", 8) == 0 ||
                   strncmp(line, " COMMANDS", 9) == 0) {
            fg = VGA_COLOR_LIGHT_GREEN;
        } else if (line[0] == ' ' && line[1] == ' ' && line[2] == ' ' && line[3] != ' ') {
            fg = VGA_COLOR_WHITE;
        }

        for (int c = 0; c < VGA_WIDTH; c++) {
            char ch = (c < len) ? line[c] : ' ';
            dst_row[c] = vga_entry((unsigned char)ch, fg, bg);
        }
    }

    memcpy((void *)VGA_MEMORY, back_buffer, sizeof(back_buffer));
    vga_hide_cursor();
}

/* Render editor to back_buffer and blit to VGA */
static void render(void) {
    if (show_help_screen) {
        render_help_screen();
        return;
    }

    update_viewport();

    int text_x = show_line_numbers ? GUTTER_WIDTH : 0;
    int text_w = VGA_WIDTH - text_x;

    bool is_splash = (line_count <= 1 && lines[0][0] == '\0' && !is_modified &&
                      current_file[0] == '\0' && mode == MODE_NORMAL);

    static const char *splash_lines[] = {
        "VIM for IPO_OS",
        "Modal Console Text Editor",
        "",
        "type  i                 to insert text",
        "type  :w <Enter>        to save file",
        "type  :q <Enter>        to exit editor",
        "type  :q! <Enter>       to exit without saving",
        "type  :help  or  <F1>   for command reference",
        "",
        "Press <F1> or <?> at any time for Help"
    };
    int splash_count = (int)(sizeof(splash_lines) / sizeof(splash_lines[0]));
    int splash_start = 5;

    /* Render text rows 0 .. VIEW_ROWS - 1 */
    for (int r = 0; r < VIEW_ROWS; r++) {
        int file_line = row_offset + r;
        uint16_t *dst_row = &back_buffer[r * VGA_WIDTH];

        if (file_line < line_count) {
            /* Line number gutter */
            if (show_line_numbers) {
                char num_str[8];
                int n = file_line + 1;
                num_str[0] = (n >= 1000) ? (0 + (n / 1000) % 10) : ' ';
                num_str[1] = (n >= 100)  ? (0 + (n / 100) % 10)  : ' ';
                num_str[2] = (n >= 10)   ? (0 + (n / 10) % 10)   : ' ';
                num_str[3] = '0' + (n % 10);
                num_str[4] = ' ';
                num_str[5] = '\0';
                for (int c = 0; c < GUTTER_WIDTH; c++) {
                    dst_row[c] = vga_entry((unsigned char)num_str[c], VGA_COLOR_CYAN, VGA_COLOR_BLACK);
                }
            }

            /* Line text decoded from UTF-8 to VGA font glyphs */
            const char *line = lines[file_line];
            int len = (int)strlen(line);
            int bpos = 0;
            int cpos = 0;

            /* Skip character columns scrolled off horizontally */
            while (bpos < len && cpos < col_offset) {
                size_t b = 1;
                utf8_to_vga_glyph(&line[bpos], len - bpos, &b);
                if (b == 0) b = 1;
                bpos += (int)b;
                cpos++;
            }

            /* Render text_w screen columns */
            for (int c = 0; c < text_w; c++) {
                if (bpos < len) {
                    size_t b = 1;
                    uint8_t glyph = utf8_to_vga_glyph(&line[bpos], len - bpos, &b);
                    if (b == 0) b = 1;
                    dst_row[text_x + c] = vga_entry(glyph, VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                    bpos += (int)b;
                } else {
                    dst_row[text_x + c] = vga_entry(' ', VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                }
            }
        } else {
            /* Beyond EOF: '~' */
            if (show_line_numbers) {
                dst_row[0] = vga_entry('~', VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
                for (int c = 1; c < GUTTER_WIDTH; c++) {
                    dst_row[c] = vga_entry(' ', VGA_COLOR_BLACK, VGA_COLOR_BLACK);
                }
            } else {
                dst_row[0] = vga_entry('~', VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
            }

            /* Clear remainder of line */
            for (int c = (show_line_numbers ? 0 : 1); c < text_w; c++) {
                dst_row[text_x + c] = vga_entry(' ', VGA_COLOR_BLACK, VGA_COLOR_BLACK);
            }

            /* Render Vim splash screen if buffer is brand new & empty */
            if (is_splash && r >= splash_start && r < splash_start + splash_count) {
                const char *sp_line = splash_lines[r - splash_start];
                int slen = (int)strlen(sp_line);
                if (slen > 0) {
                    int start_c = (text_w - slen) / 2;
                    if (start_c < 0) start_c = 0;
                    enum vga_color sp_fg = (r == splash_start) ? VGA_COLOR_LIGHT_CYAN :
                                           (r == splash_start + 1 ? VGA_COLOR_CYAN :
                                           (r == splash_start + splash_count - 1 ? VGA_COLOR_BROWN : VGA_COLOR_LIGHT_GREY));
                    for (int c = 0; c < slen && (start_c + c) < text_w; c++) {
                        dst_row[text_x + start_c + c] = vga_entry((unsigned char)sp_line[c], sp_fg, VGA_COLOR_BLACK);
                    }
                }
            }
        }
    }

    /* Status row (Row 24) */
    uint16_t *status_row = &back_buffer[VIEW_ROWS * VGA_WIDTH];

    if (mode == MODE_COMMAND) {
        char left_cmd[128];
        snprintf(left_cmd, sizeof(left_cmd), ":%s", cmd_buf);

        const char *cmd_hint = "[Enter: Run | ESC: Cancel]";
        int hlen = (int)strlen(cmd_hint);

        for (int c = 0; c < VGA_WIDTH; c++) {
            status_row[c] = vga_entry(' ', VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }

        render_utf8_to_status(status_row, 0, VGA_WIDTH - hlen - 1, left_cmd, VGA_COLOR_WHITE, VGA_COLOR_BLACK);

        for (int i = 0; i < hlen; i++) {
            status_row[VGA_WIDTH - hlen + i] = vga_entry((unsigned char)cmd_hint[i], VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        }
    } else {
        const char *mode_str = (mode == MODE_INSERT) ? "-- INSERT --" : "-- NORMAL --";
        enum vga_color mode_fg = (mode == MODE_INSERT) ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_BLACK;
        enum vga_color mode_bg = (mode == MODE_INSERT) ? VGA_COLOR_BLACK : VGA_COLOR_LIGHT_GREY;

        char left_part[64];
        snprintf(left_part, sizeof(left_part), "%s %s%s",
                 mode_str,
                 current_file[0] ? current_file : "[No Name]",
                 is_modified ? " [+]" : "");

        const char *lang_name = dynamic_keymap_get_name();
        const char *lang_tag = "EN";
        if (strstr(lang_name, "Russian") != NULL) lang_tag = "RU";
        else if (strstr(lang_name, "Ukrainian") != NULL) lang_tag = "UA";
        else if (strstr(lang_name, "Chinese") != NULL) lang_tag = "ZH";

        int cur_screen_col = get_screen_col(lines[cur_line], cur_col);
        char right_part[48];
        snprintf(right_part, sizeof(right_part), "[%s|F2] ln %d/%d col %d",
                 lang_tag, cur_line + 1, line_count, cur_screen_col + 1);

        char center_hint[128];
        if (status_msg[0] != '\0') {
            snprintf(center_hint, sizeof(center_hint), "%s", status_msg);
        } else if (mode == MODE_NORMAL) {
            snprintf(center_hint, sizeof(center_hint), "[F1:Help | F2:Lang | i:Insert | :w:Save]");
        } else {
            snprintf(center_hint, sizeof(center_hint), "[F2:Lang | ESC:Normal | Arrows:Move]");
        }

        /* Clear entire status bar with background color */
        for (int c = 0; c < VGA_WIDTH; c++) {
            status_row[c] = vga_entry(' ', mode_fg, mode_bg);
        }

        int left_cols = get_screen_col(left_part, (int)strlen(left_part));
        int right_cols = get_screen_col(right_part, (int)strlen(right_part));
        int hint_cols = get_screen_col(center_hint, (int)strlen(center_hint));

        int right_start = VGA_WIDTH - right_cols;
        if (right_start < 0) right_start = 0;

        /* Render left part */
        render_utf8_to_status(status_row, 0, right_start > 0 ? right_start - 1 : 0,
                              left_part, mode_fg, mode_bg);

        /* Render right part */
        render_utf8_to_status(status_row, right_start, VGA_WIDTH,
                              right_part, mode_fg, mode_bg);

        /* Render centered hint/status */
        int hint_start = (VGA_WIDTH - hint_cols) / 2;
        if (hint_start < left_cols + 1) {
            hint_start = left_cols + 1;
        }
        int max_hint_end = right_start - 1;
        if (hint_start < max_hint_end) {
            enum vga_color hint_fg = (status_msg[0] != '\0') ? VGA_COLOR_LIGHT_BROWN :
                                     (mode == MODE_INSERT ? VGA_COLOR_LIGHT_CYAN : VGA_COLOR_BLUE);
            render_utf8_to_status(status_row, hint_start, max_hint_end,
                                  center_hint, hint_fg, mode_bg);
        }
    }

    /* Fast blit to VGA text buffer (80 * 25 words = 4000 bytes) */
    memcpy((void *)VGA_MEMORY, back_buffer, sizeof(back_buffer));

    /* Position hardware cursor */
    if (mode == MODE_COMMAND) {
        int cmd_cols = get_screen_col(cmd_buf, cmd_len);
        int hw_cursor = VIEW_ROWS * VGA_WIDTH + 1 + cmd_cols;
        if (hw_cursor >= VGA_WIDTH * VGA_HEIGHT) hw_cursor = VGA_WIDTH * VGA_HEIGHT - 1;
        vga_set_cursor((uint16_t)hw_cursor);
    } else {
        int cur_screen_col = get_screen_col(lines[cur_line], cur_col);
        int screen_r = cur_line - row_offset;
        int screen_c = text_x + (cur_screen_col - col_offset);
        if (screen_r >= 0 && screen_r < VIEW_ROWS && screen_c >= text_x && screen_c < VGA_WIDTH) {
            vga_set_cursor((uint16_t)(screen_r * VGA_WIDTH + screen_c));
        }
    }
    vga_show_cursor();
}

/* Insert mode operations */
static void insert_char(char c) {
    save_undo_state();
    char *line = lines[cur_line];
    int len = (int)strlen(line);
    if (len >= MAX_LINE_LEN - 2) return;

    if (cur_col > len) cur_col = len;
    memmove(&line[cur_col + 1], &line[cur_col], len - cur_col + 1);
    line[cur_col] = c;
    cur_col++;
    is_modified = true;
}

static void insert_string(const char *s) {
    while (*s) {
        insert_char(*s++);
    }
}

static void split_line(void) {
    if (line_count >= MAX_LINES - 1) return;
    save_undo_state();

    char *cur = lines[cur_line];
    int len = (int)strlen(cur);
    if (cur_col > len) cur_col = len;

    /* Shift lines down */
    for (int i = line_count; i > cur_line + 1; i--) {
        strncpy(lines[i], lines[i - 1], MAX_LINE_LEN - 1);
        lines[i][MAX_LINE_LEN - 1] = '\0';
    }

    /* Copy split half to new line */
    strncpy(lines[cur_line + 1], &cur[cur_col], MAX_LINE_LEN - 1);
    lines[cur_line + 1][MAX_LINE_LEN - 1] = '\0';
    cur[cur_col] = '\0';

    line_count++;
    cur_line++;
    cur_col = 0;
    is_modified = true;
}

static void delete_char_backwards(void) {
    if (cur_col > 0) {
        save_undo_state();
        char *line = lines[cur_line];
        int len = (int)strlen(line);
        int prev = utf8_prev_char_offset(line, cur_col);
        memmove(&line[prev], &line[cur_col], len - cur_col + 1);
        cur_col = prev;
        is_modified = true;
    } else if (cur_line > 0) {
        /* Join with previous line */
        save_undo_state();
        char *prev = lines[cur_line - 1];
        int prev_len = (int)strlen(prev);
        char *cur = lines[cur_line];
        int cur_len = (int)strlen(cur);

        if (prev_len + cur_len < MAX_LINE_LEN - 1) {
            my_strcat(prev, cur, MAX_LINE_LEN);
            for (int i = cur_line; i < line_count - 1; i++) {
                strncpy(lines[i], lines[i + 1], MAX_LINE_LEN - 1);
                lines[i][MAX_LINE_LEN - 1] = '\0';
            }
            line_count--;
            cur_line--;
            cur_col = prev_len;
            is_modified = true;
        }
    }
}

static void delete_char_under_cursor(void) {
    char *line = lines[cur_line];
    int len = (int)strlen(line);
    if (cur_col < len) {
        save_undo_state();
        int next = utf8_next_char_offset(line, cur_col);
        memmove(&line[cur_col], &line[next], len - next + 1);
        is_modified = true;
    } else if (cur_line < line_count - 1) {
        /* Join with next line */
        save_undo_state();
        char *next = lines[cur_line + 1];
        int next_len = (int)strlen(next);
        if (len + next_len < MAX_LINE_LEN - 1) {
            my_strcat(line, next, MAX_LINE_LEN);
            for (int i = cur_line + 1; i < line_count - 1; i++) {
                strncpy(lines[i], lines[i + 1], MAX_LINE_LEN - 1);
                lines[i][MAX_LINE_LEN - 1] = '\0';
            }
            line_count--;
            is_modified = true;
        }
    }
}

static void delete_current_line(void) {
    if (line_count <= 1) {
        save_undo_state();
        lines[0][0] = '\0';
        cur_col = 0;
        is_modified = true;
        return;
    }
    save_undo_state();
    for (int i = cur_line; i < line_count - 1; i++) {
        strncpy(lines[i], lines[i + 1], MAX_LINE_LEN - 1);
        lines[i][MAX_LINE_LEN - 1] = '\0';
    }
    line_count--;
    if (cur_line >= line_count) cur_line = line_count - 1;
    cur_col = 0;
    is_modified = true;
}

static void delete_to_line_end(void) {
    save_undo_state();
    lines[cur_line][cur_col] = '\0';
    if (cur_col > 0) cur_col--;
    is_modified = true;
}

static void join_lines(void) {
    if (cur_line >= line_count - 1) return;
    save_undo_state();
    char *cur = lines[cur_line];
    int cur_len = (int)strlen(cur);
    char *next = lines[cur_line + 1];
    int next_len = (int)strlen(next);

    if (cur_len + 1 + next_len < MAX_LINE_LEN - 1) {
        cur[cur_len] = ' ';
        cur[cur_len + 1] = '\0';
        my_strcat(cur, next, MAX_LINE_LEN);
        for (int i = cur_line + 1; i < line_count - 1; i++) {
            strncpy(lines[i], lines[i + 1], MAX_LINE_LEN - 1);
            lines[i][MAX_LINE_LEN - 1] = '\0';
        }
        line_count--;
        cur_col = cur_len;
        is_modified = true;
    }
}

/* Word navigation */
static void move_word_forward(void) {
    char *line = lines[cur_line];
    int len = (int)strlen(line);
    while (cur_col < len && line[cur_col] != ' ') cur_col++;
    while (cur_col < len && line[cur_col] == ' ') cur_col++;
    if (cur_col >= len && cur_line < line_count - 1) {
        cur_line++;
        cur_col = 0;
        line = lines[cur_line];
        len = (int)strlen(line);
        while (cur_col < len && line[cur_col] == ' ') cur_col++;
    }
}

static void move_word_backward(void) {
    if (cur_col > 0) {
        char *line = lines[cur_line];
        cur_col--;
        while (cur_col > 0 && line[cur_col] == ' ') cur_col--;
        while (cur_col > 0 && line[cur_col - 1] != ' ') cur_col--;
    } else if (cur_line > 0) {
        cur_line--;
        cur_col = (int)strlen(lines[cur_line]);
        if (cur_col > 0) cur_col--;
    }
}

/* Execute command mode input (:w, :q, :wq, :q!) */
static bool execute_command(void) {
    char *cmd = cmd_buf;
    while (*cmd == ' ') cmd++;

    if (strcmp(cmd, "q") == 0 || strcmp(cmd, "й") == 0) {
        if (is_modified) {
            snprintf(status_msg, sizeof(status_msg), "No write since last change (add ! to override)");
            return false;
        }
        return true; /* Exit editor */
    } else if (strcmp(cmd, "q!") == 0 || strcmp(cmd, "й!") == 0) {
        return true; /* Force exit */
    } else if (strcmp(cmd, "w") == 0 || strcmp(cmd, "ц") == 0) {
        if (current_file[0] == '\0') {
            snprintf(status_msg, sizeof(status_msg), "Error: No file name");
        } else {
            save_file(current_file);
        }
    } else if (strncmp(cmd, "w ", 2) == 0 || strncmp(cmd, "ц ", 3) == 0) {
        const char *new_path = (cmd[0] == 'w') ? cmd + 2 : cmd + 3;
        while (*new_path == ' ') new_path++;
        if (*new_path) {
            strncpy(current_file, new_path, sizeof(current_file) - 1);
            save_file(current_file);
        }
    } else if (strcmp(cmd, "wq") == 0 || strcmp(cmd, "цй") == 0 ||
               strcmp(cmd, "wq!") == 0 || strcmp(cmd, "цй!") == 0 ||
               strcmp(cmd, "x") == 0 || strcmp(cmd, "ч") == 0) {
        if (current_file[0] == '\0') {
            snprintf(status_msg, sizeof(status_msg), "Error: No file name");
        } else {
            if (save_file(current_file)) {
                return true; /* Exit editor */
            }
        }
    }
 else if (strcmp(cmd, "set nu") == 0) {
        show_line_numbers = true;
        snprintf(status_msg, sizeof(status_msg), "Line numbers enabled");
    } else if (strcmp(cmd, "set nonu") == 0) {
        show_line_numbers = false;
        snprintf(status_msg, sizeof(status_msg), "Line numbers disabled");
    } else if (strncmp(cmd, "lang ", 5) == 0 || strcmp(cmd, "ru") == 0 || strcmp(cmd, "ua") == 0 ||
               strcmp(cmd, "en") == 0 || strcmp(cmd, "zh") == 0 || strcmp(cmd, "lang") == 0) {
        const char *target = NULL;
        if (strncmp(cmd, "lang ", 5) == 0) target = cmd + 5;
        else if (strcmp(cmd, "ru") == 0) target = "ru";
        else if (strcmp(cmd, "ua") == 0) target = "ua";
        else if (strcmp(cmd, "en") == 0) target = "en";
        else if (strcmp(cmd, "zh") == 0) target = "zh";

        if (target == NULL) {
            dynamic_keymap_cycle_next();
            snprintf(status_msg, sizeof(status_msg), "Layout: %s", dynamic_keymap_get_name());
        } else {
            bool found = false;
            for (int i = 0; i < 8; i++) {
                const char *cur = dynamic_keymap_get_name();
                if ((target[0] == 'r' && strstr(cur, "Russian") != NULL) ||
                    (target[0] == 'u' && strstr(cur, "Ukrainian") != NULL) ||
                    (target[0] == 'e' && strstr(cur, "English") != NULL) ||
                    (target[0] == 'z' && strstr(cur, "Chinese") != NULL) ||
                    strstr(cur, target) != NULL) {
                    found = true;
                    break;
                }
                dynamic_keymap_cycle_next();
            }
            if (found) {
                snprintf(status_msg, sizeof(status_msg), "Layout: %s", dynamic_keymap_get_name());
            } else {
                snprintf(status_msg, sizeof(status_msg), "Layout '%s' not loaded. Run /app/lang_%s first", target, target);
            }
        }
    } else if (strncmp(cmd, "font", 4) == 0) {
        const char *arg = cmd + 4;
        while (*arg == ' ') arg++;
        if (*arg == '\0') {
            snprintf(status_msg, sizeof(status_msg), "Font: %s (%u glyphs)",
                     vga_font_get_current_name(), vga_font_get_glyph_count());
        } else {
            char fpath[128];
            if (arg[0] == '/') {
                snprintf(fpath, sizeof(fpath), "%s", arg);
            } else {
                snprintf(fpath, sizeof(fpath), "/fonts/%s.fnt", arg);
            }
            int res = vga_load_cyrillic_font(fpath);
            if (res == 0) {
                snprintf(status_msg, sizeof(status_msg), "Font: %s (%u glyphs)",
                         vga_font_get_current_name(), vga_font_get_glyph_count());
            } else {
                snprintf(status_msg, sizeof(status_msg), "Cannot load font '%s'", arg);
            }
        }
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0) {
        show_help_screen = true;
        return false;
    } else {
        snprintf(status_msg, sizeof(status_msg), "Unknown command: :%s (type :help or press F1)", cmd);
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("Usage: edit [filename]\n");
        printf("Modal text editor (Vim-like) for IPO_OS.\n\n");
        printf("Modes:\n");
        printf("  NORMAL   Navigation and editing commands (default)\n");
        printf("  INSERT   Text input mode (type 'i' to enter, <ESC> to exit)\n");
        printf("  COMMAND  Command mode (type ':' to enter)\n\n");
        printf("Quick commands:\n");
        printf("  i        Enter INSERT mode\n");
        printf("  <ESC>    Return to NORMAL mode\n");
        printf("  :w       Save file\n");
        printf("  :q       Quit editor (:q! to force quit)\n");
        printf("  :wq      Save and quit\n");
        printf("  u        Undo change\n");
        printf("  dd       Delete line\n");
        printf("  F1 / ?   Open in-editor Help reference\n");
        return 0;
    }

    current_file[0] = '\0';
    if (argc > 1) {
        strncpy(current_file, argv[1], sizeof(current_file) - 1);
        current_file[sizeof(current_file) - 1] = '\0';
    }

    /* Save previous screen to restore cleanly upon exit */
    vga_hide_cursor();
    memcpy(saved_screen, (const void *)VGA_MEMORY, sizeof(saved_screen));
    saved_cursor = vga_get_cursor_position();
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        if ((saved_screen[i] & 0xFF) == VGA_CURSOR_GLYPH_SLOT) {
            saved_screen[i] = (saved_screen[i] & 0xFF00) | ' ';
        }
    }

    /* Load target file */
    if (current_file[0] != '\0') {
        load_file(current_file);
    } else {
        line_count = 1;
        lines[0][0] = '\0';
        status_msg[0] = '\0';
    }

    /* Ensure font and keymap subsystems are in app mode */
    vga_font_set_app_mode(true);
    dynamic_keymap_set_app_mode(true);
    keyboard_flush_hardware();
    keyboard_flush_app_queue();
    keyboard_set_app_input_mode(true);

    bool running = true;
    while (running) {
        render();

        /* Fetch scancode safely via keyboard driver (prevents mouse packets from leaking into edit) */
        uint8_t sc = keyboard_wait_scancode();

        update_hot_key_state(sc);
        bool is_break = (sc & 0x80) != 0;
        if (is_break) {
            continue;
        }

        /* F2 key or Ctrl+Space: dedicated language switch */
        if (sc == 0x3C || (sc == 0x39 && keyboard_is_ctrl_pressed())) {
            dynamic_keymap_cycle_next();
            snprintf(status_msg, sizeof(status_msg), "Layout: %s", dynamic_keymap_get_name());
            status_timer = 0;
            continue;
        }

        /* Ctrl+Q: Exit editor cleanly */
        if (keyboard_is_ctrl_pressed() && (sc == 0x10 || sc == 0x90)) {
            running = false;
            break;
        }

        /* If help screen is active, dismiss on any key */
        if (show_help_screen) {
            if (sc == SC_ESC || sc == 0x3B /* F1 */ || sc == SC_ENTER ||
                sc == 0x10 /* q */ || sc == 0x39 /* Space */) {
                show_help_screen = false;
            }
            continue;
        }

        /* Clear status message upon any interaction */
        status_timer++;
        if (status_timer > 3 && status_msg[0] != '\0') {
            status_msg[0] = '\0';
            status_timer = 0;
        }

        /* Global navigation keys (Arrow keys, PgUp/PgDn, Home, End) */
        if (sc == SC_ARROW_UP) {
            if (cur_line > 0) cur_line--;
            pending_op = '\0';
            continue;
        }
        if (sc == SC_ARROW_DOWN) {
            if (cur_line < line_count - 1) cur_line++;
            pending_op = '\0';
            continue;
        }
        if (sc == SC_ARROW_LEFT) {
            cur_col = utf8_prev_char_offset(lines[cur_line], cur_col);
            pending_op = '\0';
            continue;
        }
        if (sc == SC_ARROW_RIGHT) {
            int len = (int)strlen(lines[cur_line]);
            int next = utf8_next_char_offset(lines[cur_line], cur_col);
            if (mode == MODE_NORMAL) {
                if (next < len) cur_col = next;
            } else {
                if (cur_col < len) cur_col = next;
            }
            pending_op = '\0';
            continue;
        }
        if (sc == SC_PAGE_UP) {
            cur_line -= 15;
            if (cur_line < 0) cur_line = 0;
            pending_op = '\0';
            continue;
        }
        if (sc == SC_PAGE_DOWN) {
            cur_line += 15;
            if (cur_line >= line_count) cur_line = line_count - 1;
            pending_op = '\0';
            continue;
        }
        if (sc == SC_HOME) {
            cur_col = 0;
            pending_op = '\0';
            continue;
        }
        if (sc == SC_END) {
            cur_col = (int)strlen(lines[cur_line]);
            pending_op = '\0';
            continue;
        }

        /* Mode-specific processing */
        if (mode == MODE_NORMAL) {
            /* Help reference: F1 or '?' */
            if (sc == 0x3B || (sc == 0x35 && keyboard_is_shift_pressed())) {
                show_help_screen = true;
                pending_op = '\0';
                continue;
            }

            /* Normal Mode */
            if (sc == SC_ESC) {
                pending_op = '\0';
                status_msg[0] = '\0';
            } else if (sc == 0x17) { /* 'i' key -> Insert mode */
                mode = MODE_INSERT;
                pending_op = '\0';
            } else if (sc == 0x1E) { /* 'a' key -> Append */
                cur_col = utf8_next_char_offset(lines[cur_line], cur_col);
                mode = MODE_INSERT;
                pending_op = '\0';
            } else if (sc == 0x17 && keyboard_is_shift_pressed()) { /* 'I' -> Insert at line start */
                cur_col = 0;
                while (lines[cur_line][cur_col] == ' ') cur_col++;
                mode = MODE_INSERT;
                pending_op = '\0';
            } else if (sc == 0x1E && keyboard_is_shift_pressed()) { /* 'A' -> Append at line end */
                cur_col = (int)strlen(lines[cur_line]);
                mode = MODE_INSERT;
                pending_op = '\0';
            } else if (sc == 0x18 && !keyboard_is_shift_pressed()) { /* 'o' -> Open line below */
                save_undo_state();
                if (line_count < MAX_LINES - 1) {
                    for (int i = line_count; i > cur_line + 1; i--) {
                        strncpy(lines[i], lines[i - 1], MAX_LINE_LEN - 1);
                        lines[i][MAX_LINE_LEN - 1] = '\0';
                    }
                    lines[cur_line + 1][0] = '\0';
                    line_count++;
                    cur_line++;
                    cur_col = 0;
                    is_modified = true;
                    mode = MODE_INSERT;
                }
                pending_op = '\0';
            } else if (sc == 0x18 && keyboard_is_shift_pressed()) { /* 'O' -> Open line above */
                save_undo_state();
                if (line_count < MAX_LINES - 1) {
                    for (int i = line_count; i > cur_line; i--) {
                        strncpy(lines[i], lines[i - 1], MAX_LINE_LEN - 1);
                        lines[i][MAX_LINE_LEN - 1] = '\0';
                    }
                    lines[cur_line][0] = '\0';
                    line_count++;
                    cur_col = 0;
                    is_modified = true;
                    mode = MODE_INSERT;
                }
                pending_op = '\0';
            } else if (sc == 0x2D) { /* 'x' -> Delete char */
                delete_char_under_cursor();
                pending_op = '\0';
            } else if (sc == 0x20) { /* 'd' key */
                if (pending_op == 'd') {
                    delete_current_line();
                    pending_op = '\0';
                } else {
                    pending_op = 'd';
                }
            } else if (sc == 0x20 && keyboard_is_shift_pressed()) { /* 'D' -> Del to line end */
                delete_to_line_end();
                pending_op = '\0';
            } else if (sc == 0x24 && keyboard_is_shift_pressed()) { /* 'J' -> Join lines */
                join_lines();
                pending_op = '\0';
            } else if (sc == 0x16) { /* 'u' -> Undo */
                restore_undo_state();
                pending_op = '\0';
            } else if (sc == 0x23) { /* 'h' -> Left */
                cur_col = utf8_prev_char_offset(lines[cur_line], cur_col);
                pending_op = '\0';
            } else if (sc == 0x24 && !keyboard_is_shift_pressed()) { /* 'j' -> Down */
                if (cur_line < line_count - 1) cur_line++;
                pending_op = '\0';
            } else if (sc == 0x25) { /* 'k' -> Up */
                if (cur_line > 0) cur_line--;
                pending_op = '\0';
            } else if (sc == 0x26) { /* 'l' -> Right */
                int len = (int)strlen(lines[cur_line]);
                int next = utf8_next_char_offset(lines[cur_line], cur_col);
                if (next < len) cur_col = next;
                pending_op = '\0';
            } else if (sc == 0x11) { /* 'w' -> Next word */
                move_word_forward();
                pending_op = '\0';
            } else if (sc == 0x30) { /* 'b' -> Prev word */
                move_word_backward();
                pending_op = '\0';
            } else if (sc == 0x0B) { /* '0' -> Line start */
                cur_col = 0;
                pending_op = '\0';
            } else if (sc == 0x05 && keyboard_is_shift_pressed()) { /* '$' -> Line end */
                int len = (int)strlen(lines[cur_line]);
                cur_col = len > 0 ? len - 1 : 0;
                pending_op = '\0';
            } else if (sc == 0x22 && !keyboard_is_shift_pressed()) { /* 'g' */
                if (pending_op == 'g') {
                    cur_line = 0;
                    cur_col = 0;
                    pending_op = '\0';
                } else {
                    pending_op = 'g';
                }
            } else if (sc == 0x22 && keyboard_is_shift_pressed()) { /* 'G' -> End of file */
                cur_line = line_count > 0 ? line_count - 1 : 0;
                cur_col = 0;
                pending_op = '\0';
            } else if ((sc == 0x27 && keyboard_is_shift_pressed()) ||
                       (sc == 0x07 && keyboard_is_shift_pressed())) { /* ':' -> Command mode */
                mode = MODE_COMMAND;
                cmd_len = 0;
                cmd_buf[0] = '\0';
                pending_op = '\0';
            }
        } else if (mode == MODE_INSERT) {
            /* Insert Mode */
            if (sc == SC_ESC) {
                mode = MODE_NORMAL;
                if (cur_col > 0) cur_col--;
            } else if (sc == SC_BACKSPACE) {
                delete_char_backwards();
            } else if (sc == SC_DELETE) {
                delete_char_under_cursor();
            } else if (sc == SC_ENTER) {
                split_line();
            } else if (sc == SC_TAB) {
                insert_string("    ");
            } else {
                const char *s = keyboard_get_key_string(sc);
                if (s && s[0] != '\0' && s[0] != '\n' && s[0] != '\r' && s[0] != '\b' && s[0] != 127) {
                    insert_string(s);
                }
            }
        } else if (mode == MODE_COMMAND) {
            /* Command Mode (:) */
            if (sc == SC_ESC) {
                mode = MODE_NORMAL;
            } else if (sc == SC_BACKSPACE) {
                if (cmd_len > 0) {
                    int prev = utf8_prev_char_offset(cmd_buf, cmd_len);
                    cmd_len = prev;
                    cmd_buf[cmd_len] = '\0';
                } else {
                    mode = MODE_NORMAL;
                }
            }
 else if (sc == SC_ENTER) {
                mode = MODE_NORMAL;
                if (execute_command()) {
                    running = false;
                }
            } else {
                const char *s = keyboard_get_key_string(sc);
                if (s && s[0] != '\0' && s[0] != '\n' && s[0] != '\r' && s[0] != '\b' && s[0] != 127) {
                    while (*s && cmd_len < (int)sizeof(cmd_buf) - 1) {
                        cmd_buf[cmd_len++] = *s++;
                    }
                    cmd_buf[cmd_len] = '\0';
                }
            }
        }
    }

    /* Restore screen & cursor on exit */
    vga_hide_cursor();
    memcpy((void *)VGA_MEMORY, saved_screen, sizeof(saved_screen));
    vga_set_cursor(saved_cursor);
    vga_font_set_app_mode(false);
    dynamic_keymap_set_app_mode(false);
    keyboard_set_app_input_mode(false);

    return 0;
}

