/**
 * mouse_test - Live PS/2 Mouse & IntelliMouse Wheel Monitor for IPO_OS
 *
 * Displays:
 *  - Accurate screen coordinates (0..639 px X, 0..399 px Y for VGA 640x400)
 *  - Accurate monitor character cell (Column 0..79, Row 0..24 for 80x25 text mode)
 *  - Sub-character pixel accuracy (+X: 0..7 px, +Y: 0..15 px in 8x16 font cell)
 *  - Full IntelliMouse scroll wheel test:
 *      * Detection status (4-byte IntelliMouse vs standard 3-byte)
 *      * Live scroll indicators (UP / DOWN / IDLE)
 *      * Scroll position and event counters (Up ticks, Down ticks)
 *      * Dynamic visual scroll level gauge
 *  - Hardware button states (Left, Middle / Wheel click, Right) with click counters
 *  - Real-time action monitor (Moving direction, Scrolling, Dragging, Clicking, Idle)
 *  - Interactive 2D drawing canvas (paint with Left/Right buttons, Scroll Wheel selects brush)
 *  - Live cursor tracking across the monitor
 *
 * Controls:
 *  - Move mouse: move cursor on screen and canvas
 *  - Scroll wheel: scroll up/down, change canvas brush
 *  - Left click / drag: draw with primary brush
 *  - Middle click: wheel button click test
 *  - Right click / drag: draw with secondary brush
 *  - 'C' key: Clear canvas and reset click & scroll counters
 *  - 'Q' or ESC: Return to IPO_OS shell
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <vga.h>
#include <ioport.h>
#include <driver/input/keyboard.h>
#include <driver/input/mouse.h>

#define CANVAS_START_ROW 8
#define CANVAS_ROWS      16
#define CANVAS_COLS      80

#define CANVAS_INNER_W   CANVAS_COLS
#define CANVAS_INNER_H   CANVAS_ROWS

static uint16_t saved_screen[VGA_HEIGHT * VGA_WIDTH];
static uint16_t saved_cursor = 0;
static uint16_t back_buffer[VGA_HEIGHT * VGA_WIDTH];

static char canvas_grid[CANVAS_INNER_H][CANVAS_INNER_W];
static uint8_t canvas_color[CANVAS_INNER_H][CANVAS_INNER_W];

static const char brush_palette[] = {'*', '#', '@', 'O', '+', 'x', '%', '$', '=', '?'};
#define BRUSH_PALETTE_SIZE ((int)(sizeof(brush_palette) / sizeof(brush_palette[0])))
static int current_brush_idx = 0;

static void put_char_at(int row, int col, char ch, enum vga_color fg, enum vga_color bg) {
    if (row >= 0 && row < VGA_HEIGHT && col >= 0 && col < VGA_WIDTH) {
        back_buffer[row * VGA_WIDTH + col] = vga_entry((unsigned char)ch, fg, bg);
    }
}

static void print_string_at(int row, int col, const char *str, enum vga_color fg, enum vga_color bg) {
    while (*str && col < VGA_WIDTH) {
        put_char_at(row, col++, *str++, fg, bg);
    }
}

static void print_fmt_at(int row, int col, enum vga_color fg, enum vga_color bg, const char *fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    print_string_at(row, col, buf, fg, bg);
}

static void clear_canvas(void) {
    for (int r = 0; r < CANVAS_INNER_H; r++) {
        for (int c = 0; c < CANVAS_INNER_W; c++) {
            canvas_grid[r][c] = (r == 0 || r == CANVAS_INNER_H - 1 || c == 0 || c == CANVAS_INNER_W - 1) ? '.' : ' ';
            canvas_color[r][c] = VGA_COLOR_DARK_GREY;
        }
    }
}

static const char *get_direction_name(int32_t dx, int32_t dy) {
    if (dx == 0 && dy == 0) return "NONE";
    if (dx > 0 && dy < 0) return "UP-RIGHT";
    if (dx < 0 && dy < 0) return "UP-LEFT";
    if (dx > 0 && dy > 0) return "DOWN-RIGHT";
    if (dx < 0 && dy > 0) return "DOWN-LEFT";
    if (dx > 0) return "RIGHT";
    if (dx < 0) return "LEFT";
    if (dy < 0) return "UP";
    return "DOWN";
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* 1. Save original screen and cursor */
    vga_hide_cursor();
    memcpy(saved_screen, (void *)VGA_MEMORY, sizeof(saved_screen));
    saved_cursor = vga_get_cursor_position();
    for (int i = 0; i < VGA_HEIGHT * VGA_WIDTH; i++) {
        if ((saved_screen[i] & 0xFF) == VGA_CURSOR_GLYPH_SLOT) {
            saved_screen[i] = (saved_screen[i] & 0xFF00) | ' ';
        }
    }

    keyboard_set_app_input_mode(true);

    mouse_init();

    clear_canvas();

    mouse_state_t st;
    memset(&st, 0, sizeof(st));

    bool prev_left = false;
    bool prev_right = false;
    bool prev_middle = false;
    uint32_t prev_event_count = 0;
    int32_t prev_scroll_pos = 0;

    uint32_t left_clicks = 0;
    uint32_t right_clicks = 0;
    uint32_t middle_clicks = 0;
    uint32_t scroll_up_count = 0;
    uint32_t scroll_down_count = 0;

    int idle_counter = 0;
    int wheel_flash_timer = 0;
    int wheel_flash_dir = 0; // +1 up, -1 down

    char last_action[80] = "Waiting for mouse or scroll wheel input...";

    bool running = true;

    while (running) {
        /* Poll keyboard for exit or canvas clear */
        uint8_t sc;
        while ((sc = keyboard_get_scancode()) != 0) {
            if (sc == 0x01 || sc == 0x10) { /* ESC or Q */
                running = false;
                break;
            } else if (sc == 0x2E) { /* C key: Clear canvas and reset counters */
                clear_canvas();
                left_clicks = 0;
                right_clicks = 0;
                middle_clicks = 0;
                scroll_up_count = 0;
                scroll_down_count = 0;
                snprintf(last_action, sizeof(last_action), "Canvas cleared and all counters reset.");
            }
        }
        if (!running) break;

        /* Get live mouse state */
        mouse_get_state(&st);

        bool new_packet = (st.event_count != prev_event_count);
        prev_event_count = st.event_count;

        /* Detect button clicks (edge transitions) */
        if (st.left_button && !prev_left) left_clicks++;
        if (st.right_button && !prev_right) right_clicks++;
        if (st.middle_button && !prev_middle) middle_clicks++;

        prev_left = st.left_button;
        prev_right = st.right_button;
        prev_middle = st.middle_button;

        /* Detect scroll wheel activity */
        int32_t dz = st.dz;
        if (new_packet && dz != 0) {
            if (dz > 0) {
                scroll_up_count += (uint32_t)dz;
                wheel_flash_timer = 15;
                wheel_flash_dir = 1;
                /* Change brush on wheel scroll */
                current_brush_idx = (current_brush_idx + 1) % BRUSH_PALETTE_SIZE;
                snprintf(last_action, sizeof(last_action), "SCROLL WHEEL UP (+%d) -> Brush: '%c'", (int)dz, brush_palette[current_brush_idx]);
                idle_counter = 0;
            } else {
                scroll_down_count += (uint32_t)(-dz);
                wheel_flash_timer = 15;
                wheel_flash_dir = -1;
                current_brush_idx = (current_brush_idx - 1 + BRUSH_PALETTE_SIZE) % BRUSH_PALETTE_SIZE;
                snprintf(last_action, sizeof(last_action), "SCROLL WHEEL DOWN (%d) -> Brush: '%c'", (int)dz, brush_palette[current_brush_idx]);
                idle_counter = 0;
            }
        } else if (st.scroll_pos != prev_scroll_pos) {
            /* Fallback scroll detection via accumulated position */
            int32_t diff = st.scroll_pos - prev_scroll_pos;
            if (diff > 0) {
                scroll_up_count += (uint32_t)diff;
                wheel_flash_timer = 15;
                wheel_flash_dir = 1;
                current_brush_idx = (current_brush_idx + 1) % BRUSH_PALETTE_SIZE;
                snprintf(last_action, sizeof(last_action), "SCROLL WHEEL UP (+%d) -> Brush: '%c'", (int)diff, brush_palette[current_brush_idx]);
            } else {
                scroll_down_count += (uint32_t)(-diff);
                wheel_flash_timer = 15;
                wheel_flash_dir = -1;
                current_brush_idx = (current_brush_idx - 1 + BRUSH_PALETTE_SIZE) % BRUSH_PALETTE_SIZE;
                snprintf(last_action, sizeof(last_action), "SCROLL WHEEL DOWN (%d) -> Brush: '%c'", (int)diff, brush_palette[current_brush_idx]);
            }
            idle_counter = 0;
        }
        prev_scroll_pos = st.scroll_pos;

        if (wheel_flash_timer > 0) {
            wheel_flash_timer--;
        } else {
            wheel_flash_dir = 0;
        }

        /* Determine current action */
        bool is_moving = new_packet && (st.dx != 0 || st.dy != 0);
        const char *dir = get_direction_name(st.dx, st.dy);

        if (wheel_flash_timer > 0) {
            /* Keep scroll wheel message visible during flash */
        } else if (st.middle_button) {
            snprintf(last_action, sizeof(last_action), "PRESSING MIDDLE BUTTON (WHEEL CLICK)");
            idle_counter = 0;
        } else if (st.left_button && is_moving) {
            snprintf(last_action, sizeof(last_action), "DRAGGING (LEFT BUTTON) -> %s (dX=%d, dY=%d)", dir, (int)st.dx, (int)st.dy);
            idle_counter = 0;
        } else if (st.right_button && is_moving) {
            snprintf(last_action, sizeof(last_action), "DRAGGING (RIGHT BUTTON) -> %s (dX=%d, dY=%d)", dir, (int)st.dx, (int)st.dy);
            idle_counter = 0;
        } else if (st.left_button) {
            snprintf(last_action, sizeof(last_action), "PRESSING LEFT BUTTON [CLICK]");
            idle_counter = 0;
        } else if (st.right_button) {
            snprintf(last_action, sizeof(last_action), "PRESSING RIGHT BUTTON [CLICK]");
            idle_counter = 0;
        } else if (is_moving) {
            snprintf(last_action, sizeof(last_action), "MOVING MOUSE -> %s (dX=%d, dY=%d)", dir, (int)st.dx, (int)st.dy);
            idle_counter = 0;
        } else {
            idle_counter++;
            if (idle_counter > 40) {
                snprintf(last_action, sizeof(last_action), "IDLE (No motion, buttons released, wheel idle)");
            }
        }

        /* If button is held, paint on 1:1 canvas */
        if (st.row >= CANVAS_START_ROW && st.row < CANVAS_START_ROW + CANVAS_INNER_H &&
            st.col >= 0 && st.col < CANVAS_INNER_W) {
            int cr = st.row - CANVAS_START_ROW;
            int cc = st.col;
            if (st.left_button) {
                canvas_grid[cr][cc] = brush_palette[current_brush_idx];
                canvas_color[cr][cc] = VGA_COLOR_LIGHT_GREEN;
            } else if (st.middle_button) {
                canvas_grid[cr][cc] = '@';
                canvas_color[cr][cc] = VGA_COLOR_LIGHT_RED;
            } else if (st.right_button) {
                canvas_grid[cr][cc] = ' ';
                canvas_color[cr][cc] = VGA_COLOR_DARK_GREY;
            }
        }

        /* ------------------------------------------------------------- */
        /* Render Frame into Backbuffer                                  */
        /* ------------------------------------------------------------- */

        /* Clear background */
        for (int i = 0; i < VGA_HEIGHT * VGA_WIDTH; i++) {
            back_buffer[i] = vga_entry(' ', VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        }

        /* Top Header Title Bar */
        for (int c = 0; c < VGA_WIDTH; c++) {
            put_char_at(0, c, ' ', VGA_COLOR_WHITE, VGA_COLOR_BLUE);
        }
        print_string_at(0, 2, "IPO_OS PS/2 MOUSE & INTELLIMOUSE WHEEL DIAGNOSTIC", VGA_COLOR_WHITE, VGA_COLOR_BLUE);
        if (st.has_wheel) {
            print_fmt_at(0, 52, VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLUE, "[WHEEL: ID=0x%02x (%d-BYTE)]", st.dev_id, st.packet_size);
        } else {
            print_fmt_at(0, 52, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLUE, "[WHEEL: ID=0x%02x (%d-BYTE)]", st.dev_id, st.packet_size);
        }

        /* Sub-cell pixel offset inside 8x16 font glyph */
        int sub_x = st.x % 8;
        int sub_y = st.y % 16;

        /* Row 1: Position details */
        print_string_at(1, 2, "POSITION :", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        print_fmt_at(1, 13, VGA_COLOR_WHITE, VGA_COLOR_BLACK,
                     "X = %-3d px, Y = %-3d px (Bounds: %dx%d) | CELL: Col %-2d, Row %-2d (80x25)",
                     (int)st.x, (int)st.y, (int)st.max_x, (int)st.max_y, (int)st.col, (int)st.row);

        /* Row 2: Sub-cell accuracy & Deltas */
        print_string_at(2, 2, "ACCURACY :", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        print_fmt_at(2, 13, VGA_COLOR_WHITE, VGA_COLOR_BLACK,
                     "Cell Offset: +%d px X, +%d px Y (in 8x16 cell) | Packets: %-5u",
                     sub_x, sub_y, (unsigned int)st.event_count);

        /* Row 3: Motion & Deltas */
        print_string_at(3, 2, "MOTION   :", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        print_fmt_at(3, 13, VGA_COLOR_WHITE, VGA_COLOR_BLACK,
                     "dX = %-4d, dY = %-4d, dZ (Wheel) = %-3d | Dir: %-10s",
                     (int)st.dx, (int)st.dy, (int)st.dz, dir);

        /* Row 4: Scroll Wheel Monitor & Test */
        print_string_at(4, 2, "WHEEL    :", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);

        /* Wheel status badge */
        if (wheel_flash_dir > 0) {
            print_string_at(4, 13, "[ ^ SCROLL UP   ]", VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREEN);
        } else if (wheel_flash_dir < 0) {
            print_string_at(4, 13, "[ v SCROLL DOWN ]", VGA_COLOR_BLACK, VGA_COLOR_LIGHT_CYAN);
        } else {
            print_string_at(4, 13, "[ - WHEEL IDLE - ]", VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        }

        /* Build scroll visual gauge */
        char gauge[17] = "-------|-------";
        int gauge_center = 7;
        int gauge_offset = (int)(st.scroll_pos % 8);
        int dot_pos = gauge_center + gauge_offset;
        if (dot_pos < 0) dot_pos = 0;
        if (dot_pos > 14) dot_pos = 14;
        gauge[dot_pos] = 'O';

        print_fmt_at(4, 32, VGA_COLOR_WHITE, VGA_COLOR_BLACK,
                     "Pos: %-4d (Up: %-3u, Down: %-3u)  Gauge: [%s]",
                     (int)st.scroll_pos, (unsigned int)scroll_up_count, (unsigned int)scroll_down_count, gauge);

        /* Row 5: Hardware Buttons */
        print_string_at(5, 2, "BUTTONS  :", VGA_COLOR_LIGHT_MAGENTA, VGA_COLOR_BLACK);

        if (st.left_button) {
            print_string_at(5, 13, "[ LEFT: PRESSED  ]", VGA_COLOR_WHITE, VGA_COLOR_GREEN);
        } else {
            print_string_at(5, 13, "[ LEFT: RELEASED ]", VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        }

        if (st.middle_button) {
            print_string_at(5, 33, "[ MID/WHEEL: PRESSED  ]", VGA_COLOR_WHITE, VGA_COLOR_RED);
        } else {
            print_string_at(5, 33, "[ MID/WHEEL: RELEASED ]", VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        }

        if (st.right_button) {
            print_string_at(5, 58, "[ RIGHT: PRESSED  ]", VGA_COLOR_WHITE, VGA_COLOR_CYAN);
        } else {
            print_string_at(5, 58, "[ RIGHT: RELEASED ]", VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        }

        /* Row 6: Click Counters & Raw Packet */
        print_string_at(6, 2, "CLICKS   :", VGA_COLOR_LIGHT_MAGENTA, VGA_COLOR_BLACK);
        print_fmt_at(6, 13, VGA_COLOR_WHITE, VGA_COLOR_BLACK,
                     "L:%-3u M:%-3u R:%-3u",
                     (unsigned int)left_clicks, (unsigned int)middle_clicks, (unsigned int)right_clicks);
        print_fmt_at(6, 38, VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK,
                     "[Raw: %02x %02x %02x %02x (cycle=%d)]",
                     st.raw_packet[0], st.raw_packet[1], st.raw_packet[2], st.raw_packet[3], (int)st.packet_size);

        /* Row 7: Current Action Banner */
        print_string_at(7, 2, "ACTION   :", VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
        for (int c = 13; c < 78; c++) put_char_at(7, c, ' ', VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        print_string_at(7, 13, last_action, VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);

        /* Rows 8..23: 1:1 Screen Canvas (Under Mouse) */
        for (int r = 0; r < CANVAS_INNER_H; r++) {
            for (int c = 0; c < CANVAS_INNER_W; c++) {
                char ch = canvas_grid[r][c];
                uint8_t col = canvas_color[r][c];
                put_char_at(CANVAS_START_ROW + r, c, ch, (enum vga_color)col, VGA_COLOR_BLACK);
            }
        }

        /* Section 5: Bottom Navigation / Instructions Bar */
        for (int c = 0; c < VGA_WIDTH; c++) {
            put_char_at(24, c, ' ', VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
        }
        print_string_at(24, 2, "[Q/ESC] Shell", VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
        print_string_at(24, 18, "[C] Clear Canvas & Counters", VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
        print_string_at(24, 48, "[Wheel] Change Brush", VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
        print_string_at(24, 70, "[IPO_OS]", VGA_COLOR_BLUE, VGA_COLOR_LIGHT_GREY);

        /* Live Full-Screen Cursor: Rendered EXACTLY at (st.col, st.row) under mouse pointer */
        if (st.row >= 0 && st.row < VGA_HEIGHT && st.col >= 0 && st.col < VGA_WIDTH) {
            uint16_t orig_entry = back_buffer[st.row * VGA_WIDTH + st.col];
            uint8_t orig_ch = orig_entry & 0xFF;
            /* Invert cell colors or show bright cursor marker */
            uint8_t cur_bg = st.middle_button ? VGA_COLOR_LIGHT_RED :
                             st.left_button ? VGA_COLOR_LIGHT_GREEN :
                             st.right_button ? VGA_COLOR_LIGHT_CYAN : VGA_COLOR_WHITE;
            uint8_t cur_fg = VGA_COLOR_BLACK;
            if (orig_ch == ' ' || orig_ch == '.') orig_ch = '+';
            back_buffer[st.row * VGA_WIDTH + st.col] = vga_entry(orig_ch, (enum vga_color)cur_fg, (enum vga_color)cur_bg);
        }

        /* Flip Backbuffer to Screen */
        memcpy((void *)VGA_MEMORY, back_buffer, sizeof(back_buffer));

        /* Small delay to prevent tight 100% busy-loop */
        for (volatile int d = 0; d < 20000; d++) {
            io_wait();
        }
    }

    /* ------------------------------------------------------------- */
    /* Restore Original Screen & State on Exit                       */
    /* ------------------------------------------------------------- */
    vga_hide_cursor();
    memcpy((void *)VGA_MEMORY, saved_screen, sizeof(saved_screen));
    vga_set_cursor(saved_cursor);
    keyboard_set_app_input_mode(false);

    return 0;
}
