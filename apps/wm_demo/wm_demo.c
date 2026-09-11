/**
 * wm_demo.c — Window Manager demo app 1 for IPO_OS
 *
 * Creates 3 windows and returns immediately.
 * The WM compositor async task (started automatically by wm_create_window)
 * blits the pre-rendered framebuffers every 16 ms.
 *
 * Usage (multi-app — both visible at the same time):
 *   run wm_demo & wm_demo2
 *
 * Usage (standalone — animated, single-app blocking mode):
 *   wm_demo
 *
 * Exit: left-click [X Exit] in the bottom-left corner.
 *
 * Controls (handled by the WM compositor):
 *   Tab / Shift+Tab  — focus next / previous window
 *   Ctrl+Arrows      — move focused window
 *   Click            — focus a window
 *   [X Exit] button  — exit video mode
 */

#include <wm.h>
#include <vga_gfx.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── Window 1 — Info panel ──────────────────────────────────────────────── */
static void draw_info(wm_window_t *win) {
    uint8_t *fb = win->framebuf;
    uint16_t w = win->w, h = win->h;

    /* Gradient green background */
    for (int y = 0; y < (int)h; y++) {
        uint8_t c = (uint8_t)(40 + y * 8 / (int)h);
        for (int x = 0; x < (int)w; x++) fb[(size_t)y * w + x] = c;
    }
    wm_buf_draw_string(fb, w, h,  4,  3, "WM Demo 1", 15);
    wm_buf_fill_rect  (fb, w, h,  0, 12, (int)w, 1, 10);
    wm_buf_draw_string(fb, w, h,  4, 15, "Tab: next win",   7);
    wm_buf_draw_string(fb, w, h,  4, 23, "Ctrl+Arr: move",  7);
    wm_buf_draw_string(fb, w, h,  4, 31, "Click: focus",    7);
    wm_buf_draw_string(fb, w, h,  4, 39, "ESC/Q/[X]: exit", 7);
    wm_buf_fill_rect  (fb, w, h,  0, 49, (int)w, 1, 10);
    wm_buf_draw_string(fb, w, h,  4, 53, "3 windows from",  14);
    wm_buf_draw_string(fb, w, h,  4, 61, "wm_demo app.",    14);
}

/* ── Window 2 — 256-colour palette swatch ──────────────────────────────── */
static void draw_palette(wm_window_t *win) {
    uint8_t *fb = win->framebuf;
    uint16_t w = win->w, h = win->h;

    wm_buf_fill_rect(fb, w, h, 0, 0, (int)w, (int)h, 8);
    wm_buf_draw_string(fb, w, h, 2, 1, "Demo1: palette", 15);

    int idx = 32, sw = 4, sh = 4, cols = (int)w / sw;
    for (int i = 0; i < 216; i++, idx++) {
        int px = (i % cols) * sw, py = 10 + (i / cols) * sh;
        if (py + sh > (int)h) break;
        wm_buf_fill_rect(fb, w, h, px, py, sw, sh, (uint8_t)idx);
    }
    /* 16 standard colours at the bottom */
    for (int i = 0; i < 16; i++)
        wm_buf_fill_rect(fb, w, h, i*(int)(w/16), (int)h-5, (int)(w/16), 5, (uint8_t)i);
}

/* ── Window 3 — Circular shape-masked filled circle ─────────────────────── */
#define CIRC_R  26
#define CIRC_D  (CIRC_R * 2 + 2)

static uint8_t circ_mask[CIRC_D * CIRC_D];
static bool    circ_built = false;

static void build_circ_mask(void) {
    if (circ_built) return;
    for (int y = 0; y < CIRC_D; y++)
        for (int x = 0; x < CIRC_D; x++) {
            int dx = x - CIRC_R, dy = y - CIRC_R;
            circ_mask[y * CIRC_D + x] = (dx*dx + dy*dy <= CIRC_R*CIRC_R) ? 1 : 0;
        }
    circ_built = true;
}

static void draw_circle(wm_window_t *win) {
    uint8_t *fb = win->framebuf;
    uint16_t w = win->w, h = win->h;
    for (int y = 0; y < (int)h; y++)
        for (int x = 0; x < (int)w; x++) {
            int dx = x - CIRC_R, dy = y - CIRC_R;
            if (dx*dx + dy*dy > CIRC_R*CIRC_R) { fb[(size_t)y*w+x] = 0; continue; }
            uint8_t col = (uint8_t)(32 + ((x + y) % 36) * 6 % 216);
            fb[(size_t)y*w+x] = col;
        }
    wm_buf_draw_string(fb, w, h, CIRC_R-9, CIRC_R-4, "Demo1", 15);
}

/* ─────────────────────────────────────────────────────────────────────────
 * main — create windows and return immediately.
 * wm_create_window() auto-starts the async WM compositor and calls
 * draw_cb ONCE while this code is still in memory.
 * ───────────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    build_circ_mask();

    /* Window 1 — Info */
    wm_window_options_t o1 = WM_WINDOW_OPTIONS_DEFAULT;
    o1.title = "Demo1: Info";
    o1.x = 4; o1.y = 14; o1.w = 100; o1.h = 75;
    o1.decor_style = WM_DECOR_DEFAULT;
    o1.bg_color = 44; o1.border_color = 10; o1.title_bg = 2; o1.title_fg = 15;
    o1.draw_cb = draw_info;
    wm_create_window(&o1);

    /* Window 2 — Palette */
    wm_window_options_t o2 = WM_WINDOW_OPTIONS_DEFAULT;
    o2.title = "Demo1: Colors";
    o2.x = 108; o2.y = 82; o2.w = 108; o2.h = 76;
    o2.decor_style = WM_DECOR_THIN;
    o2.bg_color = 8; o2.border_color = 14;
    o2.draw_cb = draw_palette;
    wm_create_window(&o2);

    /* Window 3 — Circle (shape-masked) */
    wm_window_options_t o3 = WM_WINDOW_OPTIONS_DEFAULT;
    o3.title = NULL;
    o3.x = 218; o3.y = 4; o3.w = (uint16_t)CIRC_D; o3.h = (uint16_t)CIRC_D;
    o3.decor_style = WM_DECOR_NONE;
    o3.bg_color = 0;
    o3.shape_mask = circ_mask; o3.mask_w = (uint16_t)CIRC_D; o3.mask_h = (uint16_t)CIRC_D;
    o3.draw_cb = draw_circle;
    wm_create_window(&o3);

    /*
     * Return immediately.
     * The WM async compositor task (started by wm_create_window above)
     * blits the pre-rendered framebuffers every 16 ms.
     * Exit via [X Exit] button or ESC/Q.
     */
    return 0;
}
