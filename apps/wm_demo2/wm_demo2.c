/**
 * wm_demo2.c — Window Manager demo app 2 for IPO_OS
 *
 * Creates 2 windows and returns immediately.
 * Run together with wm_demo to show all 5 windows simultaneously:
 *
 *   run wm_demo & wm_demo2
 *
 * The WM compositor async task blits all windows from both apps every 16 ms.
 */

#include <wm.h>
#include <vga_gfx.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── Window A — Gradient panel ──────────────────────────────────────────── */
static void draw_gradient(wm_window_t *win) {
    uint8_t *fb = win->framebuf;
    uint16_t w = win->w, h = win->h;
    for (int y = 0; y < (int)h; y++) {
        for (int x = 0; x < (int)w; x++) {
            uint8_t ci = (uint8_t)(32 + ((x*6/(int)w) + (y*6/(int)h)*6) % 216);
            fb[(size_t)y * w + x] = ci;
        }
    }
    wm_buf_draw_string(fb, w, h,  4,  3, "WM Demo 2",        0);
    wm_buf_fill_rect  (fb, w, h,  0, 12, (int)w, 1, 0);
    wm_buf_draw_string(fb, w, h,  4, 15, "2 windows from",  15);
    wm_buf_draw_string(fb, w, h,  4, 23, "wm_demo2 app.",   15);
    wm_buf_fill_rect  (fb, w, h,  0, 33, (int)w, 1, 0);
    wm_buf_draw_string(fb, w, h,  4, 37, "Tab: next win",   15);
    wm_buf_draw_string(fb, w, h,  4, 45, "ESC/Q/[X]: exit", 15);
}

/* ── Window B — Status panel ────────────────────────────────────────────── */
static void draw_status(wm_window_t *win) {
    uint8_t *fb = win->framebuf;
    uint16_t w = win->w, h = win->h;

    bool focused = (wm_get_focused() == win);
    wm_buf_fill_rect(fb, w, h, 0, 0, (int)w, (int)h, focused ? 5 : 1);
    wm_buf_draw_rect(fb, w, h, 0, 0, (int)w, (int)h, focused ? 13 : 9);

    wm_buf_draw_string(fb, w, h, 4,  4, "Demo2: Status",    15);
    wm_buf_fill_rect  (fb, w, h, 0, 13, (int)w, 1, 13);

    int cnt = wm_get_window_count();
    char cbuf[20] = "Windows: ";
    cbuf[9] = (char)('0' + cnt % 10); cbuf[10] = '\0';
    wm_buf_draw_string(fb, w, h, 4, 17, cbuf, 14);
    wm_buf_draw_string(fb, w, h, 4, 25, "(wm_demo x3",   7);
    wm_buf_draw_string(fb, w, h, 4, 33, " wm_demo2 x2)", 7);
    wm_buf_fill_rect  (fb, w, h, 0, 43, (int)w, 1, 13);
    wm_buf_draw_string(fb, w, h, 4, 47, focused ? "FOCUSED" : "unfocused",
                                        focused ? 14 : 8);
}

/* ─────────────────────────────────────────────────────────────────────────
 * main — create windows and return immediately.
 * ───────────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Window A — Gradient */
    wm_window_options_t oa = WM_WINDOW_OPTIONS_DEFAULT;
    oa.title = "Demo2: Gradient";
    oa.x = 108; oa.y = 4; oa.w = 110; oa.h = 72;
    oa.decor_style = WM_DECOR_DEFAULT;
    oa.bg_color = 40; oa.border_color = 11; oa.title_bg = 3; oa.title_fg = 0;
    oa.draw_cb = draw_gradient;
    wm_create_window(&oa);

    /* Window B — Status */
    wm_window_options_t ob = WM_WINDOW_OPTIONS_DEFAULT;
    ob.title = "Demo2: Status";
    ob.x = 4; ob.y = 100; ob.w = 100; ob.h = 72;
    ob.decor_style = WM_DECOR_DEFAULT;
    ob.bg_color = 1; ob.border_color = 9; ob.title_bg = 5; ob.title_fg = 15;
    ob.draw_cb = draw_status;
    wm_create_window(&ob);

    return 0;
}
