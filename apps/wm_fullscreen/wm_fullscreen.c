/**
 * wm_fullscreen.c — Fullscreen / Maximize test application for IPO_OS
 *
 * Demonstrates:
 * - Fullscreen / maximized window dimensions (320x186, fits above taskbar)
 * - Window controls: minimize (_), maximize/restore (^), close (X)
 * - Border drag resizing and titlebar dragging
 * - Taskbar interaction (Start/Exit, window tab, scroll buttons)
 */

#include <wm.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

static void draw_fullscreen_content(wm_window_t *win) {
    uint8_t *fb = win->framebuf;
    uint16_t w = win->w;
    uint16_t h = win->h;

    /* Fill background with Windows 95 classic silver/gray (color 7) */
    wm_buf_fill_rect(fb, w, h, 0, 0, (int)w, (int)h, 7);

    /* Draw an inner sunken work area */
    if (w > 16 && h > 30) {
        int pw = (int)w - 16;
        int ph = (int)h - 26;
        /* Sunken frame: top/left dark (8), bottom/right white (15), fill white (15) */
        wm_buf_fill_rect(fb, w, h, 8, 8, pw, ph, 15);
        wm_buf_draw_line(fb, w, h, 8, 8, 8 + pw - 1, 8, 8);
        wm_buf_draw_line(fb, w, h, 8, 8, 8, 8 + ph - 1, 8);
        wm_buf_draw_line(fb, w, h, 8, 8 + ph - 1, 8 + pw - 1, 8 + ph - 1, 15);
        wm_buf_draw_line(fb, w, h, 8 + pw - 1, 8, 8 + pw - 1, 8 + ph - 1, 15);
    }

    /* Header banner */
    wm_buf_draw_string(fb, w, h, 14, 12, "WINDOWS 95 FULLSCREEN TEST", 1);
    wm_buf_draw_line(fb, w, h, 14, 21, (int)w - 14, 21, 8);

    /* Features list */
    int y = 26;
    wm_buf_draw_string(fb, w, h, 14, y, "Window Controls:", 9); y += 10;
    wm_buf_draw_string(fb, w, h, 18, y, "[_] Minimize to taskbar", 0); y += 9;
    wm_buf_draw_string(fb, w, h, 18, y, "[^] Toggle Maximize / Restore", 0); y += 9;
    wm_buf_draw_string(fb, w, h, 18, y, "[X] Close application window", 0); y += 12;

    wm_buf_draw_string(fb, w, h, 14, y, "Taskbar & Desktop:", 9); y += 10;
    wm_buf_draw_string(fb, w, h, 18, y, "- Click taskbar tab to focus/minimize", 0); y += 9;
    wm_buf_draw_string(fb, w, h, 18, y, "- Scroll tabs with [<] and [>] buttons", 0); y += 9;
    wm_buf_draw_string(fb, w, h, 18, y, "- Drag border/corner to resize window", 0); y += 9;
    wm_buf_draw_string(fb, w, h, 18, y, "- Drag titlebar to move window", 0); y += 9;
    wm_buf_draw_string(fb, w, h, 18, y, "- Click [Exit X] on taskbar to quit WM", 0); y += 14;

    /* Palette / gradient test bars */
    if (y + 16 < (int)h) {
        wm_buf_draw_string(fb, w, h, 14, y, "VGA 256-Color Gradient:", 8); y += 9;
        int bar_w = (int)w - 28;
        if (bar_w > 0) {
            for (int i = 0; i < bar_w; i++) {
                uint8_t c = (uint8_t)(32 + (i * 216 / bar_w));
                wm_buf_fill_rect(fb, w, h, 14 + i, y, 1, 6, c);
            }
        }
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = "Fullscreen App";
    opt.x = 0;
    opt.y = 0;
    opt.w = 320;
    opt.h = 186; /* Maximize size, above taskbar (y=186..199) */
    opt.decor_style = WM_DECOR_DEFAULT;
    opt.bg_color = 7;
    opt.draw_cb = draw_fullscreen_content;

    wm_window_t *win = wm_create_window(&opt);
    if (win != NULL) {
        win->maximized = 1;
        win->saved_x = 20;
        win->saved_y = 15;
        win->saved_w = 200;
        win->saved_h = 130;
    }

    return 0;
}
