#include <kernel/terminal.h>

#include <vga.h>
#include <driver/keyboard.h>
#include <driver/input/keymap/keymap.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

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
static void commit_input_to_history(void) {
    // Append the input chars and a terminating newline
    append_string_to_history(input_buffer, input_size);
    append_char_to_history('\n');

    // Recompute line offsets from scratch (robust against previous state)
    line_count = 0;
    if (term_buffer_size == 0) {
        // nothing to index
        input_size = 0;
        return;
    }

    uint16_t col = 0;
    uint16_t width = VGA_WIDTH;

    // first line always starts at 0
    if (line_count < MAX_LINE_OFFSETS) line_offsets[line_count++] = 0;

    for (size_t i = 0; i < term_buffer_size; i++) {
        char ch = term_buffer[i];
        if (ch == '\n') {
            size_t next = i + 1;
            // avoid adding trailing offset equal to term_buffer_size (no content)
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
            // commit current input to history and render
            commit_input_to_history();
            if (view_offset != 0) view_offset = 0; // go to bottom on new input
            render();
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
