#include <wm.h>
#include <vga_gfx.h>
#include <vga.h>
#include <memory/kmalloc.h>
#include <memory/address_space.h>
#include <driver/input/keyboard.h>
#include <driver/input/keymap/keymap.h>
#include <driver/input/keymap/dynamic_keymap.h>
#include <driver/input/mouse.h>
#include <system/timer.h>
#include <system/state.h>
#include <ioport.h>
#include <kernel/terminal.h>
#include <kernel/utf.h>
#include <kernel/input_ext.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define WM_FONT_W 6
#define WM_FONT_H 8
#define TITLEBAR_H 11
#define TASKBAR_Y  186
#define WM_MIN_WIDTH 40
#define WM_MIN_HEIGHT 20
#define WM_MAX_WIDTH (VGA_GFX_WIDTH - 4)
#define WM_MAX_HEIGHT (TASKBAR_Y - TITLEBAR_H - 4)
#define TASKBAR_H  14

/* Built-in 6x8 bitmap font for printable ASCII 0x20..0x7E */
static const uint8_t *wm_font_data = (const uint8_t *)(
/*sp*/"\x00\x00\x00\x00\x00\x00\x00\x00"
/* ! */"\x20\x20\x20\x20\x00\x20\x00\x00"
/* " */"\x50\x50\x00\x00\x00\x00\x00\x00"
/* # */"\x50\xF8\x50\x50\xF8\x50\x00\x00"
/* $ */"\x20\x78\xA0\x70\x28\xF0\x20\x00"
/* % */"\xC0\xC8\x10\x20\x40\x98\x18\x00"
/* & */"\x40\xA0\xA0\x40\xA8\x90\x68\x00"
/* ' */"\x20\x40\x00\x00\x00\x00\x00\x00"
/* ( */"\x10\x20\x40\x40\x40\x20\x10\x00"
/* ) */"\x40\x20\x10\x10\x10\x20\x40\x00"
/* * */"\x00\x20\xA8\x70\xA8\x20\x00\x00"
/* + */"\x00\x20\x20\xF8\x20\x20\x00\x00"
/* , */"\x00\x00\x00\x00\x00\x20\x40\x00"
/* - */"\x00\x00\x00\xF8\x00\x00\x00\x00"
/* . */"\x00\x00\x00\x00\x00\x60\x60\x00"
/* / */"\x08\x10\x10\x20\x40\x40\x80\x00"
/* 0 */"\x70\x88\x88\x88\x88\x88\x70\x00"
/* 1 */"\x20\x60\x20\x20\x20\x20\x70\x00"
/* 2 */"\x70\x88\x08\x30\x40\x80\xF8\x00"
/* 3 */"\xF8\x08\x10\x30\x08\x88\x70\x00"
/* 4 */"\x10\x30\x50\x90\xF8\x10\x10\x00"
/* 5 */"\xF8\x80\xF0\x08\x08\x88\x70\x00"
/* 6 */"\x38\x40\x80\xF0\x88\x88\x70\x00"
/* 7 */"\xF8\x08\x10\x20\x20\x40\x40\x00"
/* 8 */"\x70\x88\x88\x70\x88\x88\x70\x00"
/* 9 */"\x70\x88\x88\x78\x08\x10\xE0\x00"
/* : */"\x00\x60\x60\x00\x60\x60\x00\x00"
/* ; */"\x00\x20\x00\x20\x20\x40\x00\x00"
/* < */"\x10\x20\x40\x80\x40\x20\x10\x00"
/* = */"\x00\x00\xF8\x00\xF8\x00\x00\x00"
/* > */"\x80\x40\x20\x10\x20\x40\x80\x00"
/* ? */"\x70\x88\x10\x20\x20\x00\x20\x00"
/* @ */"\x70\x88\xA8\xB8\xB0\x80\x70\x00"
/* A */"\x20\x50\x88\x88\xF8\x88\x88\x00"
/* B */"\xF0\x88\x88\xF0\x88\x88\xF0\x00"
/* C */"\x70\x88\x80\x80\x80\x88\x70\x00"
/* D */"\xE0\x90\x88\x88\x88\x90\xE0\x00"
/* E */"\xF8\x80\x80\xF0\x80\x80\xF8\x00"
/* F */"\xF8\x80\x80\xF0\x80\x80\x80\x00"
/* G */"\x70\x88\x80\xB8\x88\x88\x70\x00"
/* H */"\x88\x88\x88\xF8\x88\x88\x88\x00"
/* I */"\x70\x20\x20\x20\x20\x20\x70\x00"
/* J */"\x38\x10\x10\x10\x10\x90\x60\x00"
/* K */"\x88\x90\xA0\xC0\xA0\x90\x88\x00"
/* L */"\x80\x80\x80\x80\x80\x80\xF8\x00"
/* M */"\x88\xD8\xA8\xA8\x88\x88\x88\x00"
/* N */"\x88\xC8\xA8\x98\x88\x88\x88\x00"
/* O */"\x70\x88\x88\x88\x88\x88\x70\x00"
/* P */"\xF0\x88\x88\xF0\x80\x80\x80\x00"
/* Q */"\x70\x88\x88\x88\xA8\x90\x68\x00"
/* R */"\xF0\x88\x88\xF0\xA0\x90\x88\x00"
/* S */"\x70\x88\x80\x70\x08\x88\x70\x00"
/* T */"\xF8\x20\x20\x20\x20\x20\x20\x00"
/* U */"\x88\x88\x88\x88\x88\x88\x70\x00"
/* V */"\x88\x88\x88\x50\x50\x20\x20\x00"
/* W */"\x88\x88\x88\xA8\xA8\xD8\x88\x00"
/* X */"\x88\x88\x50\x20\x50\x88\x88\x00"
/* Y */"\x88\x88\x50\x20\x20\x20\x20\x00"
/* Z */"\xF8\x08\x10\x20\x40\x80\xF8\x00"
/* [ */"\x70\x40\x40\x40\x40\x40\x70\x00"
/* \ */"\x80\x40\x40\x20\x10\x10\x08\x00"
/* ] */"\x70\x10\x10\x10\x10\x10\x70\x00"
/* ^ */"\x20\x50\x88\x00\x00\x00\x00\x00"
/* _ */"\x00\x00\x00\x00\x00\x00\xF8\x00"
/* ` */"\x40\x20\x00\x00\x00\x00\x00\x00"
/* a */"\x00\x00\x70\x08\x78\x88\x78\x00"
/* b */"\x80\x80\xF0\x88\x88\x88\xF0\x00"
/* c */"\x00\x00\x70\x88\x80\x88\x70\x00"
/* d */"\x08\x08\x78\x88\x88\x88\x78\x00"
/* e */"\x00\x00\x70\x88\xF8\x80\x70\x00"
/* f */"\x30\x48\x40\xF0\x40\x40\x40\x00"
/* g */"\x00\x00\x78\x88\x78\x08\x70\x00"
/* h */"\x80\x80\xF0\x88\x88\x88\x88\x00"
/* i */"\x20\x00\x60\x20\x20\x20\x70\x00"
/* j */"\x10\x00\x30\x10\x10\x90\x60\x00"
/* k */"\x80\x80\x90\xA0\xC0\xA0\x90\x00"
/* l */"\x60\x20\x20\x20\x20\x20\x70\x00"
/* m */"\x00\x00\xD8\xA8\xA8\xA8\xA8\x00"
/* n */"\x00\x00\xF0\x88\x88\x88\x88\x00"
/* o */"\x00\x00\x70\x88\x88\x88\x70\x00"
/* p */"\x00\x00\xF0\x88\x88\xF0\x80\x00"
/* q */"\x00\x00\x78\x88\x88\x78\x08\x00"
/* r */"\x00\x00\xB0\xC8\x80\x80\x80\x00"
/* s */"\x00\x00\x78\x80\x70\x08\xF0\x00"
/* t */"\x40\x40\xF0\x40\x40\x48\x30\x00"
/* u */"\x00\x00\x88\x88\x88\x88\x78\x00"
/* v */"\x00\x00\x88\x88\x50\x50\x20\x00"
/* w */"\x00\x00\x88\x88\xA8\xA8\x50\x00"
/* x */"\x00\x00\x88\x50\x20\x50\x88\x00"
/* y */"\x00\x00\x88\x88\x78\x08\x70\x00"
/* z */"\x00\x00\xF8\x10\x20\x40\xF8\x00"
/* { */"\x18\x20\x20\x60\x20\x20\x18\x00"
/* | */"\x20\x20\x20\x00\x20\x20\x20\x00"
/* } */"\xC0\x20\x20\x30\x20\x20\xC0\x00"
/* ~ */"\x48\x90\x00\x00\x00\x00\x00\x00"
);

void wm_buf_put_pixel(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, uint8_t c) {
    if (!buf || x < 0 || y < 0 || x >= (int)bw || y >= (int)bh) return;
    buf[(size_t)y * bw + (size_t)x] = c;
}

void wm_buf_draw_line(uint8_t *buf, uint16_t bw, uint16_t bh, int x0, int y0, int x1, int y1, uint8_t c) {
    if (!buf) return;
    int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    while (1) {
        wm_buf_put_pixel(buf, bw, bh, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

void wm_buf_fill_rect(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, int w, int h, uint8_t c) {
    if (!buf || w <= 0 || h <= 0) return;
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int64_t x2_64 = (int64_t)x + (int64_t)w;
    int64_t y2_64 = (int64_t)y + (int64_t)h;
    int x2 = (x2_64 > (int64_t)bw) ? (int)bw : (int)x2_64;
    int y2 = (y2_64 > (int64_t)bh) ? (int)bh : (int)y2_64;
    if (x2 <= x1 || y2 <= y1) return;
    for (int cy = y1; cy < y2; cy++) {
        memset(buf + (size_t)cy * bw + (size_t)x1, c, (size_t)(x2 - x1));
    }
}

void wm_buf_draw_rect(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, int w, int h, uint8_t c) {
    if (!buf || w <= 0 || h <= 0) return;
    int x_end = (int)((int64_t)x + (int64_t)w - 1);
    int y_end = (int)((int64_t)y + (int64_t)h - 1);
    wm_buf_draw_line(buf, bw, bh, x, y, x_end, y, c);
    wm_buf_draw_line(buf, bw, bh, x, y_end, x_end, y_end, c);
    wm_buf_draw_line(buf, bw, bh, x, y, x, y_end, c);
    wm_buf_draw_line(buf, bw, bh, x_end, y, x_end, y_end, c);
}

/* Draw a single VGA font slot at 6x8, using the built-in ASCII table for
 * 0x20..0x7E and downscaling 8x16 cached VGA glyphs for extended slots. */
static void wm_buf_draw_glyph_impl(uint8_t *buf, uint16_t bw, uint16_t bh,
                                   int x, int y, uint8_t slot,
                                   uint8_t fg, uint8_t bg, bool draw_bg) {
    if (!buf) return;
    bool use_ascii = (slot >= 0x20u && slot <= 0x7Eu);
    const uint8_t *bmp = use_ascii
        ? (wm_font_data + (size_t)(slot - 0x20u) * WM_FONT_H)
        : vga_font_get_cached_glyph(slot);
    if (!bmp) {
        if (draw_bg) wm_buf_fill_rect(buf, bw, bh, x, y, WM_FONT_W, WM_FONT_H, bg);
        return;
    }

    uint8_t msb_mask = use_ascii ? 0x80u : 0x40u;
    for (int row = 0; row < WM_FONT_H; row++) {
        uint8_t bits = use_ascii ? bmp[row] : (bmp[row * 2] | bmp[row * 2 + 1]);
        for (int col = 0; col < WM_FONT_W; col++) {
            bool set = (bits & (msb_mask >> col)) != 0;
            if (set) {
                wm_buf_put_pixel(buf, bw, bh, x + col, y + row, fg);
            } else if (draw_bg) {
                wm_buf_put_pixel(buf, bw, bh, x + col, y + row, bg);
            }
        }
    }
}

void wm_buf_draw_char(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, char ch, uint8_t c) {
    wm_buf_draw_glyph_impl(buf, bw, bh, x, y, (uint8_t)ch, c, 0, false);
}

void wm_buf_draw_char_bg(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, char ch, uint8_t fg, uint8_t bg) {
    wm_buf_draw_glyph_impl(buf, bw, bh, x, y, (uint8_t)ch, fg, bg, true);
}

static void wm_buf_draw_string_impl(uint8_t *buf, uint16_t bw, uint16_t bh,
                                    int x, int y, const char *str,
                                    uint8_t fg, uint8_t bg, bool draw_bg) {
    if (!str || !buf) return;
    int cx = x;
    size_t len = strlen(str);
    size_t i = 0;
    while (i < len) {
        if (str[i] == '\n') {
            cx = x;
            y += WM_FONT_H;
            i++;
            continue;
        }
        if (str[i] == '\t') {
            int tab_step = WM_FONT_W * 4;
            int rel = cx - x;
            int next_cx = x + ((rel / tab_step) + 1) * tab_step;
            if (draw_bg) {
                wm_buf_fill_rect(buf, bw, bh, cx, y, next_cx - cx, WM_FONT_H, bg);
            }
            cx = next_cx;
            i++;
            continue;
        }
        size_t bytes = 0;
        uint8_t slot = utf8_to_vga_glyph(&str[i], len - i, &bytes);
        if (bytes == 0) {
            i++;
            continue;
        }
        wm_buf_draw_glyph_impl(buf, bw, bh, cx, y, slot, fg, bg, draw_bg);
        cx += WM_FONT_W;
        i += bytes;
    }
}

void wm_buf_draw_string(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, const char *str, uint8_t c) {
    wm_buf_draw_string_impl(buf, bw, bh, x, y, str, c, 0, false);
}

void wm_buf_draw_string_bg(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, const char *str, uint8_t fg, uint8_t bg) {
    wm_buf_draw_string_impl(buf, bw, bh, x, y, str, fg, bg, true);
}

#include <kernel/process.h>

static wm_window_t *wm_head  = NULL;
static wm_window_t *wm_tail  = NULL;
static int          wm_count = 0;

static void list_append(wm_window_t *w) {
    w->prev = wm_tail;
    w->next = NULL;
    if (wm_tail) wm_tail->next = w; else wm_head = w;
    wm_tail = w;
    wm_count++;
}

static void list_remove(wm_window_t *w) {
    if (w->prev) w->prev->next = w->next; else wm_head = w->next;
    if (w->next) w->next->prev = w->prev; else wm_tail = w->prev;
    w->prev = w->next = NULL;
    wm_count--;
}

static void list_front(wm_window_t *w) {
    if (wm_tail == w) return;
    list_remove(w);
    list_append(w);
}

static bool wm_sess_active = false;
static bool wm_prev_left   = false;
static bool wm_prev_right  = false;
static uint8_t *wm_backbuf = NULL;

static uint8_t *wm_get_backbuf(void) {
    if (!wm_backbuf) {
        wm_backbuf = (uint8_t *)kmalloc(VGA_GFX_SIZE);
    }
    return wm_backbuf;
}

static bool wm_window_is_visible_for_fg(const wm_window_t *w) {
    if (!w) return false;
    if (!process_is_batch_active()) return true;
    if (!process_get_separate_windows()) return true;
    process_t *fg = process_get_displayed_foreground();
    if (!fg) return false;
    if (w->owner_pid != 0) {
        process_t *owner = process_find_by_pid(w->owner_pid);
        int wid = owner ? owner->window_id : w->window_id;
        return (wid == fg->window_id);
    }
    return (w->window_id == fg->window_id);
}

static int wm_count_visible_windows(void) {
    int count = 0;
    for (wm_window_t *w = wm_head; w; w = w->next) {
        if (wm_window_is_visible_for_fg(w)) count++;
    }
    return count;
}

void wm_init(void) {
    wm_head = wm_tail = NULL;
    wm_count = 0;
}

wm_window_t *wm_create_window(const wm_window_options_t *opts) {
    if (!opts) return NULL;

    process_t *cur_proc = process_get_current();
    if (cur_proc) {
        cur_proc->is_wm_app = true;
        cur_proc->wants_graphics = true;
    }

    /* Auto-start WM session if not already running */
    if (!wm_sess_active) {
        wm_session_start();
    } else if (!vga_is_graphics_mode()) {
        process_t *fg = process_get_foreground();
        bool should_switch_hw = (!process_is_batch_active() || !process_get_separate_windows() ||
                                 (fg && (fg->wants_graphics || fg->is_wm_app ||
                                         (cur_proc && fg->window_id == cur_proc->window_id))));
        if (should_switch_hw) {
            vga_set_mode_13h_hardware();
            vga_gfx_init_default_palette();
            mouse_set_bounds(VGA_GFX_WIDTH, VGA_GFX_HEIGHT);
        }
    }

    wm_window_t *win = kmalloc(sizeof(wm_window_t));
    if (!win) return NULL;
    memset(win, 0, sizeof(wm_window_t));

    if (opts->fullscreen) {
        win->x=0; win->y=0; win->w=VGA_GFX_WIDTH; win->h=VGA_GFX_HEIGHT; win->fullscreen=true;
    } else {
        win->w = opts->w>0 ? opts->w : 80;
        win->h = opts->h>0 ? opts->h : 50;
        if (win->w > WM_MAX_WIDTH)  win->w = WM_MAX_WIDTH;
        if (win->h > WM_MAX_HEIGHT) win->h = WM_MAX_HEIGHT;

        int16_t req_x = opts->x;
        int16_t req_y = opts->y;
        int16_t min_y = (opts->decor_style != WM_DECOR_NONE) ? (TITLEBAR_H + 2) : 0;
        int16_t max_x = (int16_t)(VGA_GFX_WIDTH - 2 - win->w);
        int16_t max_y = (int16_t)(TASKBAR_Y - 2 - win->h);
        if (max_x < 2) max_x = 2;
        if (max_y < min_y) max_y = min_y;

        if (req_x < 2) req_x = 2;
        if (req_x > max_x) req_x = max_x;
        if (req_y < min_y) req_y = min_y;
        if (req_y > max_y) req_y = max_y;

        int cur_wid = cur_proc ? cur_proc->window_id : 0;
        uint32_t cur_pid = cur_proc ? cur_proc->pid : 0;
        bool sep_wins = process_is_batch_active() && process_get_separate_windows();
        wm_window_t **ws_wins = NULL;
        int ws_win_count = 0;
        int ws_win_cap = 0;
        for (wm_window_t *w = wm_head; w; w = w->next) {
            bool same_ws = sep_wins ? (w->window_id == cur_wid) : wm_window_is_visible_for_fg(w);
            if (same_ws && !w->fullscreen) {
                if (ws_win_count >= ws_win_cap) {
                    int ncap = ws_win_cap ? ws_win_cap * 2 : 8;
                    wm_window_t **grown = (wm_window_t **)krealloc(ws_wins, (size_t)ncap * sizeof(wm_window_t *));
                    if (!grown) break;
                    ws_wins = grown;
                    ws_win_cap = ncap;
                }
                ws_wins[ws_win_count++] = w;
            }
        }

        if (sep_wins && ws_win_count == 0) {
            /* Separate workspaces (&): place Workspace 0 on left, Workspace 1 on right
             * so windows in different workspaces do not occupy the same screen area. */
            if (win->w <= 154) {
                req_x = (cur_wid % 2 == 0) ? 2 : 162;
                req_y = min_y;
            }
        } else if (ws_win_count == 1 &&
                   (process_is_batch_active() || ws_wins[0]->owner_pid != cur_pid)) {
            wm_window_t *w0 = ws_wins[0];
            if (w0->w > 154) {
                wm_resize(w0, 154, w0->h);
            }
            w0->x = 2;
            w0->saved_x = 2;
            if (w0->y < min_y) w0->y = min_y;
            if (w0->y > (int16_t)(TASKBAR_Y - 2 - w0->h)) {
                w0->y = (int16_t)(TASKBAR_Y - 2 - w0->h);
            }
            w0->saved_y = w0->y;
            w0->dirty = true;

            if (win->w > 154) win->w = 154;
            max_x = (int16_t)(VGA_GFX_WIDTH - 2 - win->w);
            req_x = 162;
            if (req_x > max_x) req_x = max_x;
            req_y = w0->y;
            if (req_y < min_y) req_y = min_y;
            if (req_y > max_y) req_y = max_y;
        } else if (ws_win_count >= 2 && process_is_batch_active()) {
            /* 3+ windows in a batch workspace: tile in dynamic 2-column grid */
            for (int i = 0; i < ws_win_count; i++) {
                wm_window_t *wi = ws_wins[i];
                uint16_t tw = wi->w > 154 ? 154 : wi->w;
                uint16_t th = wi->h > 72 ? 72 : wi->h;
                if (wi->w != tw || wi->h != th) wm_resize(wi, tw, th);
                wi->x = (int16_t)((i & 1) ? 162 : 2);
                wi->y = (int16_t)(14 + ((i / 2) % 2) * 88);
                wi->saved_x = wi->x;
                wi->saved_y = wi->y;
                wi->dirty = true;
            }
            int slot = ws_win_count;
            if (win->w > 154) win->w = 154;
            if (win->h > 72) win->h = 72;
            req_x = (int16_t)((slot & 1) ? 162 : 2);
            req_y = (int16_t)(14 + ((slot / 2) % 2) * 88);
        } else {
            for (int i = 0; i < ws_win_count; i++) {
                wm_window_t *w = ws_wins[i];
                if (w->x == req_x && w->y == req_y) {
                    int16_t range_x = (int16_t)(max_x - 2);
                    int16_t range_y = (int16_t)(max_y - min_y);
                    req_x = (range_x > 0) ? (int16_t)(2 + ((req_x - 2 + 22) % (range_x + 1))) : 2;
                    req_y = (range_y > 0) ? (int16_t)(min_y + ((req_y - min_y + 12) % (range_y + 1))) : min_y;
                }
            }
        }
        if (ws_wins) kfree(ws_wins);
        win->x = req_x;
        win->y = req_y;
    }

    win->saved_x = win->x;
    win->saved_y = win->y;
    win->saved_w = win->w;
    win->saved_h = win->h;
    win->maximized = false;

    if (opts->title && *opts->title) {
        size_t tl=strlen(opts->title);
        win->title=kmalloc(tl+1);
        if(win->title) memcpy(win->title,opts->title,tl+1);
    }

    win->decor_style  = opts->decor_style;
    win->bg_color     = opts->bg_color;
    win->border_color = opts->border_color ? opts->border_color : 15;
    win->title_bg     = opts->title_bg;
    win->title_fg     = opts->title_fg ? opts->title_fg : 15;
    win->shape_mask   = opts->shape_mask;
    win->mask_w       = opts->mask_w;
    win->mask_h       = opts->mask_h;
    win->draw_cb      = opts->draw_cb;   /* kept for wm_session_run() */
    win->event_cb     = opts->event_cb;
    win->user_data    = opts->user_data;
    win->owner_pid    = cur_proc ? cur_proc->pid : 0;
    win->window_id    = cur_proc ? cur_proc->window_id : 0;

    /* Copy the shape mask into kernel heap so it remains valid after the
       calling app exits (the original array lives in app memory).          */
    if (opts->shape_mask && opts->mask_w > 0 && opts->mask_h > 0) {
        size_t msz = (size_t)opts->mask_w * opts->mask_h;
        uint8_t *mask_copy = kmalloc(msz);
        if (mask_copy) {
            memcpy(mask_copy, opts->shape_mask, msz);
            win->shape_mask       = mask_copy;
            win->mask_w           = opts->mask_w;
            win->mask_h           = opts->mask_h;
            win->shape_mask_owned = true;
        } else {
            win->shape_mask       = NULL;
            win->mask_w           = 0;
            win->mask_h           = 0;
            win->shape_mask_owned = false;
        }
        /* if kmalloc fails, fall through with no mask (window still shows) */
    }

    size_t fsz = (size_t)win->w * win->h;
    if (opts->framebuf) {
        win->framebuf = opts->framebuf;
        win->framebuf_owned = false;
    } else {
        size_t cap = VGA_GFX_SIZE; /* Full-screen capacity so resize never reallocates */
        if (cap < fsz) cap = fsz;
        win->framebuf = kmalloc(cap);
        if (!win->framebuf) {
            if (win->shape_mask_owned && win->shape_mask) kfree((void *)win->shape_mask);
            if (win->title) kfree(win->title);
            kfree(win);
            return NULL;
        }
        win->framebuf_owned = true;
        memset(win->framebuf, win->bg_color, cap);
    }

    list_append(win);

    /* Render initial frame while owner process memory is still mapped. */
    if (win->draw_cb) {
        bool in_proc = process_in_process_context();
        uint32_t saved_eflags = 0;
        if (in_proc) {
            __asm__ volatile("pushfl; popl %0; sti" : "=r"(saved_eflags) : : "memory");
        }
        win->draw_cb(win);
        if (in_proc) {
            __asm__ volatile("pushl %0; popfl" : : "r"(saved_eflags) : "memory", "cc");
        }
    }

    return win;
}

static wm_window_t *drag_win = NULL;
static int drag_mode = 0;
static wm_window_t *last_title_click_win = NULL;
static uint32_t last_title_click_time = 0;
static bool raw_overlay_has_saved = false;

static void wm_free_window(wm_window_t *win) {
    if (!win) return;
    if (drag_win == win) {
        drag_win = NULL;
        drag_mode = 0;
    }
    if (last_title_click_win == win) {
        last_title_click_win = NULL;
    }
    if (win->raw_lut) kfree(win->raw_lut);
    if (win->raw_kbd_queue) kfree(win->raw_kbd_queue);
    if (win->framebuf_owned && win->framebuf) kfree(win->framebuf);
    if (win->shape_mask_owned && win->shape_mask) kfree((void *)win->shape_mask);
    if (win->title) kfree(win->title);
    kfree(win);
}

typedef struct {
    process_t *owner;
    wm_window_t *win;
    wm_event_cb_t event_cb;
    uint32_t type;
    uint32_t data;
    void (*draw_cb)(wm_window_t *);
    bool toggle_interrupts;
} wm_owner_cb_ctx_t;

static void wm_owner_cb_trampoline(void *raw_ctx) {
    wm_owner_cb_ctx_t *c = (wm_owner_cb_ctx_t *)raw_ctx;
    process_t *prev_mapped = process_get_mapped_app();
    process_t *prev_cur = process_get_current();

    if (c->owner) {
        process_map_app(c->owner);
        process_set_current(c->owner);
    }

    if (c->event_cb) {
        c->event_cb(c->win, c->type, c->data);
    } else if (c->draw_cb) {
        uint32_t saved_eflags = 0;
        if (c->toggle_interrupts) {
            __asm__ volatile("pushfl; popl %0; sti" : "=r"(saved_eflags) : : "memory");
        }
        c->draw_cb(c->win);
        if (c->toggle_interrupts) {
            __asm__ volatile("pushl %0; popfl" : : "r"(saved_eflags) : "memory", "cc");
        }
    }

    if (c->owner) {
        process_map_app(prev_mapped);
        process_set_current(prev_cur);
    }
}

static void wm_invoke_owner_cb(wm_window_t *win, wm_event_cb_t event_cb,
                               uint32_t type, uint32_t data,
                               void (*draw_cb)(wm_window_t *),
                               bool toggle_interrupts) {
    process_t *owner = (win && win->owner_pid) ? process_find_by_pid(win->owner_pid) : NULL;
    wm_owner_cb_ctx_t ctx = {
        .owner = owner,
        .win = win,
        .event_cb = event_cb,
        .type = type,
        .data = data,
        .draw_cb = draw_cb,
        .toggle_interrupts = toggle_interrupts
    };

    if (owner && owner->address_space) {
        address_space_call_with_copy(owner->address_space, wm_owner_cb_trampoline, &ctx, sizeof(ctx));
    } else {
        wm_owner_cb_trampoline(&ctx);
    }
}

static void wm_dispatch_window_event(wm_window_t *win, uint32_t type, uint32_t data) {
    if (!win || !win->event_cb) return;
    if (win->owner_pid != 0 && !process_is_alive(win->owner_pid)) return;
    wm_invoke_owner_cb(win, win->event_cb, type, data, NULL, false);
}

void wm_notify_window_event(wm_window_t *win, uint32_t type, uint32_t data) {
    if (!wm_is_window_valid(win)) return;
    wm_dispatch_window_event(win, type, data);
}

bool wm_is_window_valid(wm_window_t *win) {
    if (!win) return false;
    for (wm_window_t *w = wm_head; w; w = w->next) {
        if (w == win) return true;
    }
    return false;
}

void wm_destroy_window(wm_window_t *win) {
    if (!win || !wm_is_window_valid(win)) return;
    uint32_t owner_pid = win->owner_pid;
    if (win->event_cb && (win->owner_pid == 0 || process_is_alive(win->owner_pid))) {
        wm_event_cb_t cb = win->event_cb;
        win->event_cb = NULL; /* Prevent re-entrancy */
        wm_invoke_owner_cb(win, cb, WM_EVENT_CLOSE, 0, NULL, false);
    }
    list_remove(win);
    wm_free_window(win);

    if (owner_pid != 0 && !wm_has_windows_for_pid(owner_pid)) {
        process_t *owner = process_find_by_pid(owner_pid);
        if (owner) {
            if (!process_in_process_context()) {
                owner->is_running = 0;
                owner->completed_and_acknowledged = true;
            }
        }
    }

    if (wm_sess_active) {
        wm_invalidate_all();
    }
}

void wm_destroy_windows_for_pid(uint32_t pid) {
    if (pid == 0) return;
    wm_window_t *w = wm_head;
    while (w) {
        wm_window_t *next = w->next;
        if (w->owner_pid == pid) {
            w->event_cb = NULL;
            list_remove(w);
            wm_free_window(w);
        }
        w = next;
    }
    if (wm_sess_active) {
        if (wm_count == 0) {
            wm_session_stop();
        } else {
            wm_invalidate_all();
        }
    }
}

static void wm_close_window_by_user(wm_window_t *win) {
    if (!win) return;
    uint32_t owner_pid = win->owner_pid;
    wm_destroy_window(win);
    if (owner_pid != 0 && !wm_has_windows_for_pid(owner_pid)) {
        process_t *owner = process_find_by_pid(owner_pid);
        if (owner) {
            owner->is_running = 0;
            owner->completed_and_acknowledged = true;
        }
    }
}

int          wm_get_window_count(void) { return wm_count; }

int          wm_get_window_count_for_pid(uint32_t pid) {
    if (pid == 0) return 0;
    int c = 0;
    for (wm_window_t *w = wm_head; w; w = w->next) {
        if (w->owner_pid == pid) c++;
    }
    return c;
}

bool         wm_has_windows_for_pid(uint32_t pid) {
    if (pid == 0) return false;
    for (wm_window_t *w = wm_head; w; w = w->next) {
        if (w->owner_pid == pid) return true;
    }
    return false;
}

wm_window_t *wm_get_window_list(void)  { return wm_head; }
wm_window_t *wm_get_focused(void) {
    for (wm_window_t *w = wm_tail; w; w = w->prev) {
        if (wm_window_is_visible_for_fg(w) && !w->minimized) {
            return w;
        }
    }
    return NULL;
}

void wm_set_focus(wm_window_t *win) {
    if (!win || !wm_is_window_valid(win)) return;
    if (wm_tail != win) {
        wm_window_t *old = wm_tail;
        if (old) {
            old->dirty = true;
            wm_dispatch_window_event(old, WM_EVENT_FOCUS_OUT, 0);
        }
        list_front(win);
        win->dirty = true;
        wm_dispatch_window_event(win, WM_EVENT_FOCUS_IN, 0);
    }
    if (win->owner_pid) {
        process_t *owner = process_find_by_pid(win->owner_pid);
        if (owner) {
            process_set_foreground(owner);
        }
    }
}

void wm_focus_window_for_pid(uint32_t pid) {
    if (pid == 0) return;
    for (wm_window_t *w = wm_tail; w; w = w->prev) {
        if (w->owner_pid == pid && !w->minimized) {
            wm_set_focus(w);
            return;
        }
    }
    for (wm_window_t *w = wm_tail; w; w = w->prev) {
        if (w->owner_pid == pid) {
            w->minimized = false;
            wm_set_focus(w);
            return;
        }
    }
    for (wm_window_t *w = wm_tail; w; w = w->prev) {
        if (wm_window_is_visible_for_fg(w) && !w->minimized) {
            wm_set_focus(w);
            return;
        }
    }
}
void wm_focus_next(void) {
    wm_window_t *cur = wm_get_focused();
    wm_window_t *start = cur ? cur->next : wm_head;
    for (wm_window_t *w = start; w; w = w->next) {
        if (wm_window_is_visible_for_fg(w) && !w->minimized) {
            wm_set_focus(w);
            return;
        }
    }
    for (wm_window_t *w = wm_head; w && w != cur; w = w->next) {
        if (wm_window_is_visible_for_fg(w) && !w->minimized) {
            wm_set_focus(w);
            return;
        }
    }
}
void wm_focus_prev(void) {
    wm_window_t *cur = wm_get_focused();
    wm_window_t *start = cur ? cur->prev : wm_tail;
    for (wm_window_t *w = start; w; w = w->prev) {
        if (wm_window_is_visible_for_fg(w) && !w->minimized) {
            wm_set_focus(w);
            return;
        }
    }
    for (wm_window_t *w = wm_tail; w && w != cur; w = w->prev) {
        if (wm_window_is_visible_for_fg(w) && !w->minimized) {
            wm_set_focus(w);
            return;
        }
    }
}

void wm_invalidate(wm_window_t *win) { if(win) win->dirty=true; }
void wm_invalidate_all(void)         { for(wm_window_t *w=wm_head;w;w=w->next) w->dirty=true; }

void wm_move(wm_window_t *win, int16_t nx, int16_t ny) {
    if (!win || win->fullscreen) return;
    /* Ensure title bar remains completely accessible and on screen */
    if (ny < TITLEBAR_H + 2) ny = TITLEBAR_H + 2;
    if (ny > (int16_t)(TASKBAR_Y - 16)) ny = (int16_t)(TASKBAR_Y - 16);
    if (nx < -(int16_t)win->w + 30) nx = -(int16_t)win->w + 30;
    if (nx > (int16_t)(VGA_GFX_WIDTH - 30)) nx = (int16_t)(VGA_GFX_WIDTH - 30);
    win->x = nx;
    win->y = ny;
    if (!win->maximized) {
        win->saved_x = nx;
        win->saved_y = ny;
    }
    win->dirty = true;
    uint32_t pos_data = (((uint32_t)(uint16_t)nx) << 16) | (uint32_t)(uint16_t)ny;
    wm_dispatch_window_event(win, WM_EVENT_MOVE, pos_data);
}

static void wm_scale_copy_framebuf(uint8_t *dst, uint16_t dw, uint16_t dh,
                                   const uint8_t *src, uint16_t sw, uint16_t sh) {
    if (!dst || !src || dw == 0 || dh == 0 || sw == 0 || sh == 0) return;
    uint32_t step_y = ((uint32_t)sh << 16) / dh;
    uint32_t step_x = ((uint32_t)sw << 16) / dw;
    uint32_t cur_y = 0;
    for (uint16_t dy = 0; dy < dh; dy++) {
        uint16_t sy = (uint16_t)(cur_y >> 16);
        if (sy >= sh) sy = (uint16_t)(sh - 1);
        uint8_t *drow = dst + (size_t)dy * dw;
        const uint8_t *srow = src + (size_t)sy * sw;
        uint32_t cur_x = 0;
        for (uint16_t dx = 0; dx < dw; dx++) {
            uint16_t sx = (uint16_t)(cur_x >> 16);
            if (sx >= sw) sx = (uint16_t)(sw - 1);
            drow[dx] = srow[sx];
            cur_x += step_x;
        }
        cur_y += step_y;
    }
}

bool wm_resize(wm_window_t *win, uint16_t nw, uint16_t nh) {
    if (!win || win->fullscreen || !nw || !nh) return false;
    if (nw < WM_MIN_WIDTH) nw = WM_MIN_WIDTH;
    if (nw > WM_MAX_WIDTH) nw = WM_MAX_WIDTH;
    if (nh < WM_MIN_HEIGHT) nh = WM_MIN_HEIGHT;
    if (nh > WM_MAX_HEIGHT) nh = WM_MAX_HEIGHT;

    if (win->w == nw && win->h == nh) return true;

    uint16_t old_w = win->w;
    uint16_t old_h = win->h;

    if (win->framebuf_owned && win->framebuf && old_w > 0 && old_h > 0) {
        static uint8_t *scale_scratch = NULL;
        static size_t scale_scratch_cap = 0;
        size_t cur_sz = (size_t)old_w * old_h;
        if (cur_sz > scale_scratch_cap) {
            uint8_t *new_scratch = (uint8_t *)krealloc(scale_scratch, cur_sz);
            if (!new_scratch) return false;
            scale_scratch = new_scratch;
            scale_scratch_cap = cur_sz;
        }
        memcpy(scale_scratch, win->framebuf, cur_sz);
        wm_scale_copy_framebuf(win->framebuf, nw, nh, scale_scratch, old_w, old_h);
    } else if (win->framebuf_owned && win->framebuf) {
        memset(win->framebuf, win->bg_color, (size_t)nw * nh);
    }

    if (win->shape_mask && win->shape_mask_owned && win->mask_w > 0 && win->mask_h > 0) {
        uint8_t *nm = kmalloc((size_t)nw * nh);
        if (nm) {
            wm_scale_copy_framebuf(nm, nw, nh, win->shape_mask, win->mask_w, win->mask_h);
            kfree((void *)win->shape_mask);
            win->shape_mask = nm;
            win->mask_w = nw;
            win->mask_h = nh;
        }
    }

    win->w = nw;
    win->h = nh;
    win->dirty = true;
    if (!win->maximized) {
        win->saved_w = nw;
        win->saved_h = nh;
    }
    uint32_t dim_data = ((uint32_t)nw << 16) | (uint32_t)nh;
    wm_dispatch_window_event(win, WM_EVENT_RESIZE, dim_data);
    return true;
}

void wm_minimize(wm_window_t *win) {
    if (!win || !wm_is_window_valid(win)) return;
    win->minimized = true;
    for (wm_window_t *w = wm_tail; w; w = w->prev) {
        if (wm_window_is_visible_for_fg(w) && !w->minimized) {
            wm_set_focus(w);
            break;
        }
    }
}

void wm_restore(wm_window_t *win) {
    if (!win) return;
    win->minimized = false;
    wm_set_focus(win);
}

void wm_toggle_maximize(wm_window_t *win) {
    if (!win || win->fullscreen) return;
    if (win->maximized) {
        win->maximized = false;
        int16_t rx = win->saved_x >= 2 ? win->saved_x : 2;
        int16_t ry = win->saved_y >= (TITLEBAR_H + 2) ? win->saved_y : (TITLEBAR_H + 2);
        uint16_t rw = win->saved_w >= WM_MIN_WIDTH ? win->saved_w : 140;
        uint16_t rh = win->saved_h >= WM_MIN_HEIGHT ? win->saved_h : 100;
        win->x = rx;
        win->y = ry;
        wm_resize(win, rw, rh);
    } else {
        win->saved_x = win->x;
        win->saved_y = win->y;
        win->saved_w = win->w;
        win->saved_h = win->h;
        win->maximized = true;
        win->x = 2;
        win->y = TITLEBAR_H + 2;
        wm_resize(win, WM_MAX_WIDTH, WM_MAX_HEIGHT);
    }
    wm_invalidate_all();
}


void wm_set_title(wm_window_t *win, const char *title) {
    if (!win) return;
    if (win->title) { kfree(win->title); win->title=NULL; }
    if (title && *title) {
        size_t tl=strlen(title);
        win->title=kmalloc(tl+1);
        if (win->title) memcpy(win->title, title, tl+1);
    }
    win->dirty = true;
}

void wm_set_shape(wm_window_t *win, const uint8_t *mask, uint16_t mw, uint16_t mh) {
    if (!win) return;
    uint8_t *mask_copy = NULL;
    if (mask && mw > 0 && mh > 0) {
        size_t mask_size = (size_t)mw * mh;
        mask_copy = kmalloc(mask_size);
        if (!mask_copy) return;
        memcpy(mask_copy, mask, mask_size);
    }
    if (win->shape_mask_owned && win->shape_mask) {
        kfree((void *)win->shape_mask);
    }
    win->shape_mask = mask_copy;
    win->mask_w = mask_copy ? mw : 0;
    win->mask_h = mask_copy ? mh : 0;
    win->shape_mask_owned = mask_copy != NULL;
    win->dirty = true;
}

void wm_set_fullscreen(wm_window_t *win, bool enable) {
    if (!win || !wm_is_window_valid(win) || win->fullscreen == enable) return;
    if (enable) {
        uint8_t *new_framebuf = kmalloc(VGA_GFX_SIZE);
        if (!new_framebuf) return;
        memset(new_framebuf, win->bg_color, VGA_GFX_SIZE);
        wm_scale_copy_framebuf(new_framebuf, VGA_GFX_WIDTH, VGA_GFX_HEIGHT,
                               win->framebuf, win->w, win->h);
        win->fullscreen_saved_x = win->x;
        win->fullscreen_saved_y = win->y;
        win->fullscreen_saved_w = win->w;
        win->fullscreen_saved_h = win->h;
        if (win->framebuf_owned && win->framebuf) kfree(win->framebuf);
        win->framebuf = new_framebuf;
        win->framebuf_owned = true;
        win->x = 0;
        win->y = 0;
        win->w = VGA_GFX_WIDTH;
        win->h = VGA_GFX_HEIGHT;
        win->minimized = false;
        win->fullscreen = true;
        wm_set_focus(win);
    } else {
        uint16_t width = win->fullscreen_saved_w ? win->fullscreen_saved_w : 80;
        uint16_t height = win->fullscreen_saved_h ? win->fullscreen_saved_h : 50;
        size_t need = (size_t)width * height;
        size_t cap = VGA_GFX_SIZE; /* Preserve full-screen capacity for subsequent wm_resize() */
        if (cap < need) cap = need;
        uint8_t *new_framebuf = kmalloc(cap);
        if (!new_framebuf) return;
        memset(new_framebuf, win->bg_color, cap);
        wm_scale_copy_framebuf(new_framebuf, width, height,
                               win->framebuf, win->w, win->h);
        if (win->framebuf_owned && win->framebuf) kfree(win->framebuf);
        win->framebuf = new_framebuf;
        win->framebuf_owned = true;
        win->x = win->fullscreen_saved_x;
        win->y = win->fullscreen_saved_y;
        win->w = width;
        win->h = height;
        win->fullscreen = false;
    }
    win->dirty = true;
    uint32_t dim_data = ((uint32_t)win->w << 16) | (uint32_t)win->h;
    wm_dispatch_window_event(win, WM_EVENT_RESIZE, dim_data);
    wm_invalidate_all();
}

/* Windows 95 palette indices and 3D primitives */

#define WIN_CLR_DESKTOP        3    /* Teal / Cyan */
#define WIN_CLR_FACE           7    /* Light Gray */
#define WIN_CLR_HILIGHT        15   /* White */
#define WIN_CLR_SHADOW         8    /* Dark Gray */
#define WIN_CLR_DKSHADOW       0    /* Black */
#define WIN_CLR_ACTIVE_TITLE   1    /* Dark Blue / Navy */
#define WIN_CLR_ACTIVE_TEXT    15   /* White */
#define WIN_CLR_INACT_TITLE    8    /* Dark Gray */
#define WIN_CLR_INACT_TEXT     7    /* Light Gray */

static void draw_3d_box(uint8_t *bb, int x, int y, int w, int h, bool sunken) {
    if (w <= 0 || h <= 0) return;
    uint8_t topleft = sunken ? WIN_CLR_DKSHADOW : WIN_CLR_HILIGHT;
    uint8_t botright = sunken ? WIN_CLR_HILIGHT : WIN_CLR_DKSHADOW;
    wm_buf_fill_rect(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, x, y, w, h, WIN_CLR_FACE);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, x, y, x + w - 1, y, topleft);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, x, y, x, y + h - 1, topleft);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, x, y + h - 1, x + w - 1, y + h - 1, botright);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, x + w - 1, y, x + w - 1, y + h - 1, botright);
}

static bool wm_window_has_close_button(const wm_window_t *win) {
    if (!win || win->decor_style != WM_DECOR_DEFAULT) {
        return false;
    }
    if (win->owner_pid != 0) {
        process_t *owner = process_find_by_pid(win->owner_pid);
        if (owner && (owner->gfx_overlay_flags & VGA_OVERLAY_HIDE_BUTTON)) {
            return false;
        }
    }
    return true;
}

/* Mouse cursor (white arrow, black outline) */
static const uint8_t *wm_cursor = (const uint8_t *)"\x80\xC0\xE0\xF0\xF8\xE0\xA0\x00";

static void draw_cursor_outline_px(uint8_t *bb, int cx, int cy, int row, int col, int dr, int dc) {
    int nr = row + dr, nc = col + dc;
    bool in = nr >= 0 && nr < 8 && nc >= 0 && nc < 5 && (wm_cursor[nr] & (0x80u >> nc));
    if (!in) {
        int px = cx + col + dc, py = cy + row + dr;
        if (px >= 0 && px < VGA_GFX_WIDTH && py >= 0 && py < VGA_GFX_HEIGHT) {
            bb[(size_t)py * VGA_GFX_WIDTH + px] = 0;
        }
    }
}

static void draw_cursor(uint8_t *bb, int cx, int cy) {
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 5; col++)
            if (wm_cursor[row] & (0x80u >> col)) {
                draw_cursor_outline_px(bb, cx, cy, row, col, -1, 0);
                draw_cursor_outline_px(bb, cx, cy, row, col,  1, 0);
                draw_cursor_outline_px(bb, cx, cy, row, col,  0, -1);
                draw_cursor_outline_px(bb, cx, cy, row, col,  0,  1);
            }
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 5; col++)
            if (wm_cursor[row] & (0x80u >> col)) {
                int px = cx + col, py = cy + row;
                if (px >= 0 && px < VGA_GFX_WIDTH && py >= 0 && py < VGA_GFX_HEIGHT)
                    bb[(size_t)py * VGA_GFX_WIDTH + px] = 15;
            }
}

/* Window decorations, taskbar, and wallpaper */
static void draw_decorations(wm_window_t *win, uint8_t *bb) {
    if (win->fullscreen || win->minimized || win->decor_style==WM_DECOR_NONE || win->decor_style==WM_DECOR_CUSTOM) return;
    bool focused = (wm_get_focused() == win);

    if (win->decor_style == WM_DECOR_THIN) {
        wm_buf_draw_rect(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, (int)win->x-1, (int)win->y-1, (int)win->w+2, (int)win->h+2,
                         focused ? WIN_CLR_HILIGHT : WIN_CLR_SHADOW);
        return;
    }

    /* Windows 95 outer 3D beveled frame */
    int ox = (int)win->x - 2;
    int oy = (int)win->y - TITLEBAR_H - 2;
    int ow = (int)win->w + 4;
    int oh = (int)win->h + TITLEBAR_H + 4;

    /* Outer border */
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, ox, oy, ox + ow - 1, oy, WIN_CLR_HILIGHT);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, ox, oy, ox, oy + oh - 1, WIN_CLR_HILIGHT);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, ox, oy + oh - 1, ox + ow - 1, oy + oh - 1, WIN_CLR_DKSHADOW);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, ox + ow - 1, oy, ox + ow - 1, oy + oh - 1, WIN_CLR_DKSHADOW);

    /* Inner border */
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, ox + 1, oy + 1, ox + ow - 2, oy + 1, WIN_CLR_FACE);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, ox + 1, oy + 1, ox + 1, oy + oh - 2, WIN_CLR_FACE);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, ox + 1, oy + oh - 2, ox + ow - 2, oy + oh - 2, WIN_CLR_SHADOW);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, ox + ow - 2, oy + 1, ox + ow - 2, oy + oh - 2, WIN_CLR_SHADOW);

    /* Sunken border around client area */
    int cx = (int)win->x - 1;
    int cy = (int)win->y - 1;
    int cw = (int)win->w + 2;
    int ch = (int)win->h + 2;
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, cx, cy, cx + cw - 1, cy, WIN_CLR_SHADOW);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, cx, cy, cx, cy + ch - 1, WIN_CLR_SHADOW);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, cx, cy + ch - 1, cx + cw - 1, cy + ch - 1, WIN_CLR_HILIGHT);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, cx + cw - 1, cy, cx + cw - 1, cy + ch - 1, WIN_CLR_HILIGHT);

    /* Title bar background */
    uint8_t tbg = focused ? WIN_CLR_ACTIVE_TITLE : WIN_CLR_INACT_TITLE;
    uint8_t tfg = focused ? WIN_CLR_ACTIVE_TEXT : WIN_CLR_INACT_TEXT;
    wm_buf_fill_rect(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, (int)win->x, (int)win->y - TITLEBAR_H, (int)win->w, TITLEBAR_H, tbg);

    /* Title text */
    if (win->title) {
        int avail_w = (int)win->w - 40;
        int max_chars = avail_w > 0 ? avail_w / 6 : 0;
        char disp_buf[64];
        if (max_chars >= (int)sizeof(disp_buf)) {
            max_chars = (int)sizeof(disp_buf) - 1;
        }
        if (max_chars > 0) {
            size_t title_len = strlen(win->title);
            if (title_len <= (size_t)max_chars) {
                strncpy(disp_buf, win->title, (size_t)max_chars);
                disp_buf[max_chars] = '\0';
            } else {
                const char *sep = "   *   ";
                size_t sep_len = strlen(sep);
                size_t loop_len = title_len + sep_len;
                uint32_t now = timer_millis();
                uint32_t offset = (now / 200u) % loop_len;
                for (int c = 0; c < max_chars; c++) {
                    size_t idx = (offset + (size_t)c) % loop_len;
                    if (idx < title_len) {
                        disp_buf[c] = win->title[idx];
                    } else {
                        disp_buf[c] = sep[idx - title_len];
                    }
                }
                disp_buf[max_chars] = '\0';
            }
            wm_buf_draw_string(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, (int)win->x + 2, (int)win->y - TITLEBAR_H + 2, disp_buf, tfg);
        }
    }

    /* 3 Titlebar Buttons on the right */
    int btn_y = (int)win->y - TITLEBAR_H + 1;

    /* 1. Close button [X] */
    if (wm_window_has_close_button(win)) {
        int bx_close = (int)win->x + (int)win->w - 9;
        draw_3d_box(bb, bx_close, btn_y, 8, 8, false);
        wm_buf_put_pixel(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_close + 2, btn_y + 2, WIN_CLR_DKSHADOW);
        wm_buf_put_pixel(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_close + 5, btn_y + 2, WIN_CLR_DKSHADOW);
        wm_buf_put_pixel(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_close + 3, btn_y + 3, WIN_CLR_DKSHADOW);
        wm_buf_put_pixel(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_close + 4, btn_y + 3, WIN_CLR_DKSHADOW);
        wm_buf_put_pixel(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_close + 3, btn_y + 4, WIN_CLR_DKSHADOW);
        wm_buf_put_pixel(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_close + 4, btn_y + 4, WIN_CLR_DKSHADOW);
        wm_buf_put_pixel(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_close + 2, btn_y + 5, WIN_CLR_DKSHADOW);
        wm_buf_put_pixel(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_close + 5, btn_y + 5, WIN_CLR_DKSHADOW);
    }

    /* 2. Maximize / Restore button [^] */
    int bx_max = (int)win->x + (int)win->w - 18;
    draw_3d_box(bb, bx_max, btn_y, 8, 8, false);
    if (win->maximized) {
        wm_buf_draw_rect(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_max + 2, btn_y + 3, 4, 3, WIN_CLR_DKSHADOW);
        wm_buf_draw_rect(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_max + 3, btn_y + 2, 3, 3, WIN_CLR_DKSHADOW);
    } else {
        wm_buf_draw_rect(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_max + 2, btn_y + 2, 5, 5, WIN_CLR_DKSHADOW);
        wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_max + 2, btn_y + 2, bx_max + 6, btn_y + 2, WIN_CLR_DKSHADOW);
    }

    /* 3. Minimize button [_] */
    int bx_min = (int)win->x + (int)win->w - 27;
    draw_3d_box(bb, bx_min, btn_y, 8, 8, false);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_min + 2, btn_y + 5, bx_min + 5, btn_y + 5, WIN_CLR_DKSHADOW);

    /* 4. Fullscreen button [+] (cover entire screen over all WM buttons/taskbar) */
    if ((int)win->w >= 40) {
        int bx_fs = (int)win->x + (int)win->w - 36;
        draw_3d_box(bb, bx_fs, btn_y, 8, 8, false);
        /* Top-left outward corner */
        wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_fs + 1, btn_y + 1, bx_fs + 3, btn_y + 1, WIN_CLR_DKSHADOW);
        wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_fs + 1, btn_y + 1, bx_fs + 1, btn_y + 3, WIN_CLR_DKSHADOW);
        /* Top-right outward corner */
        wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_fs + 4, btn_y + 1, bx_fs + 6, btn_y + 1, WIN_CLR_DKSHADOW);
        wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_fs + 6, btn_y + 1, bx_fs + 6, btn_y + 3, WIN_CLR_DKSHADOW);
        /* Bottom-left outward corner */
        wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_fs + 1, btn_y + 4, bx_fs + 1, btn_y + 6, WIN_CLR_DKSHADOW);
        wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_fs + 1, btn_y + 6, bx_fs + 3, btn_y + 6, WIN_CLR_DKSHADOW);
        /* Bottom-right outward corner */
        wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_fs + 6, btn_y + 4, bx_fs + 6, btn_y + 6, WIN_CLR_DKSHADOW);
        wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, bx_fs + 4, btn_y + 6, bx_fs + 6, btn_y + 6, WIN_CLR_DKSHADOW);
    }
}

static inline uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static inline uint8_t bcd_to_bin(uint8_t val) {
    return (uint8_t)(((val >> 4) * 10) + (val & 0x0F));
}

static void wm_get_rtc_time(int *out_h, int *out_m) {
    int timeout = 10000;
    while ((cmos_read(0x0A) & 0x80) && --timeout > 0);

    uint8_t min = cmos_read(0x02);
    uint8_t hr  = cmos_read(0x04);
    uint8_t reg_b = cmos_read(0x0B);

    if (!(reg_b & 0x04)) {
        min = bcd_to_bin(min);
        hr  = (uint8_t)(((hr & 0x7F) ? bcd_to_bin(hr & 0x7F) : 0) | (hr & 0x80));
    }

    if (!(reg_b & 0x02)) {
        bool is_pm = (hr & 0x80) != 0;
        uint8_t h12 = (uint8_t)(hr & 0x7F);
        if (h12 == 12) h12 = 0;
        hr = is_pm ? (uint8_t)(h12 + 12) : h12;
    }

    *out_h = (int)(hr % 24);
    *out_m = (int)(min % 60);
}

static int taskbar_scroll_offset = 0;
static uint32_t wm_lang_offset = 0;
static uint32_t wm_lang_last_tick = 0;

static void wm_draw_close_button(uint8_t *bb, int x, int y, int w, int h, bool pressed) {
    bool is_wm = wm_session_active();
    uint8_t clr_face      = is_wm ? WIN_CLR_FACE : 7;
    uint8_t clr_hilight   = is_wm ? WIN_CLR_HILIGHT : 15;
    uint8_t clr_shadow    = is_wm ? WIN_CLR_DKSHADOW : 0;
    uint8_t clr_icon      = is_wm ? WIN_CLR_ACTIVE_TITLE : 1;
    uint8_t clr_text      = is_wm ? WIN_CLR_DKSHADOW : 0;

    uint8_t topleft = pressed ? clr_shadow : clr_hilight;
    uint8_t botright = pressed ? clr_hilight : clr_shadow;

    /* 3D button frame */
    wm_buf_fill_rect(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, x, y, w, h, clr_face);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, x, y, x + w - 1, y, topleft);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, x, y, x, y + h - 1, topleft);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, x, y + h - 1, x + w - 1, y + h - 1, botright);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, x + w - 1, y, x + w - 1, y + h - 1, botright);

    /* Indicator icon (6x6) */
    wm_buf_fill_rect(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, x + 2, y + 2, 6, 6, clr_icon);

    /* Text: "Close" */
    wm_buf_draw_string(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, x + 10, y + 2, "Close", clr_text);
}

static void wm_handle_close_button_click(void) {
    if (wm_session_active()) {
        /* Clicking taskbar "Close" always exits WM mode and closes WM windows. */
        for (wm_window_t *w = wm_head; w; w = w->next) {
            if (w->owner_pid) {
                process_t *owner = process_find_by_pid(w->owner_pid);
                if (owner) {
                    owner->is_running = 0;
                    owner->completed_and_acknowledged = true;
                }
            }
        }
        process_t *fg = process_get_displayed_foreground();
        if (fg && (fg->is_wm_app || fg->wants_graphics)) {
            fg->is_running = 0;
            fg->completed_and_acknowledged = true;
        }
        wm_session_stop();
    } else {
        process_t *fg = process_get_displayed_foreground();
        if (!fg) fg = process_get_foreground();
        if (fg) {
            fg->is_running = 0;
            fg->completed_and_acknowledged = true;
        }
        if (!process_get_separate_windows()) {
            raw_overlay_has_saved = false;
            vga_set_mode_text_hardware();
            dynamic_keymap_reapply_fonts();
            mouse_set_bounds_from_display();
        }
    }
}

static void draw_taskbar(uint8_t *bb) {
    /* 3D raised taskbar across the bottom */
    wm_buf_fill_rect(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, 0, TASKBAR_Y, VGA_GFX_WIDTH, TASKBAR_H, WIN_CLR_FACE);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, 0, TASKBAR_Y, VGA_GFX_WIDTH - 1, TASKBAR_Y, WIN_CLR_HILIGHT);

    /* Close button on the left */
    process_t *fg = process_get_displayed_foreground();
    if (!fg) fg = process_get_foreground();
    uint32_t overlay_flags = fg ? fg->gfx_overlay_flags : 0;
    bool show_button = !(overlay_flags & VGA_OVERLAY_HIDE_BUTTON);

    if (show_button) {
        wm_draw_close_button(bb, 2, TASKBAR_Y + 2, 40, 10, false);
    }

    /* Left scroll button '<' */
    draw_3d_box(bb, 44, TASKBAR_Y + 2, 9, 10, false);
    wm_buf_draw_string(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, 46, TASKBAR_Y + 3, "<", WIN_CLR_DKSHADOW);

    /* Window tabs between x = 54 and x = 182 (up to 3 visible tabs, width 40) */
    int win_idx = 0;
    int visible_tab = 0;
    int tab_w = 40;
    int tab_h = 10;
    for (wm_window_t *w = wm_head; w; w = w->next) {
        if (!wm_window_is_visible_for_fg(w)) continue;
        if (win_idx < taskbar_scroll_offset) { win_idx++; continue; }
        win_idx++;
        if (visible_tab >= 3) break;

        int tx = 54 + visible_tab * 43;
        int ty = TASKBAR_Y + 2;
        bool is_active = (w == wm_get_focused() && !w->minimized);

        if (is_active) {
            draw_3d_box(bb, tx, ty, tab_w, tab_h, true);
            wm_buf_fill_rect(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, tx + 1, ty + 1, tab_w - 2, tab_h - 2, WIN_CLR_ACTIVE_TITLE);
        } else {
            draw_3d_box(bb, tx, ty, tab_w, tab_h, false);
        }

        char title_buf[6];
        const char *src_title = w->title ? w->title : "Win";
        if (w->minimized) {
            title_buf[0] = '_';
            strncpy(title_buf + 1, src_title, 3);
            title_buf[4] = '\0';
        } else {
            strncpy(title_buf, src_title, 4);
            title_buf[4] = '\0';
        }
        uint8_t text_color = is_active ? WIN_CLR_ACTIVE_TEXT : (w->minimized ? WIN_CLR_SHADOW : WIN_CLR_DKSHADOW);
        wm_buf_draw_string(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, tx + 2, ty + 3, title_buf, text_color);

        /* Dedicated [x] close button on this specific tab */
        if (wm_window_has_close_button(w)) {
            int close_btn_x = tx + tab_w - 9;
            int close_btn_y = ty + 1;
            draw_3d_box(bb, close_btn_x, close_btn_y, 8, 8, false);
            wm_buf_draw_string(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, close_btn_x + 1, close_btn_y, "x", is_active ? 15 : WIN_CLR_DKSHADOW);
        }

        visible_tab++;
    }

    /* Right scroll button '>' */
    draw_3d_box(bb, 184, TASKBAR_Y + 2, 9, 10, false);
    wm_buf_draw_string(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, 186, TASKBAR_Y + 3, ">", WIN_CLR_DKSHADOW);

    /* Language box in tray (x = 195 .. 279, width 85) */
    draw_3d_box(bb, 195, TASKBAR_Y + 2, 85, 10, true);
    const char *lang_name = dynamic_keymap_get_name();
    if (!lang_name || lang_name[0] == '\0') {
        lang_name = default_keymap_name;
    }
    size_t lang_byte_len = strlen(lang_name);
    int vis_chars = 13; /* (85 - 4) / 6 = 13 character cells */

    /* Count codepoints in lang_name */
    size_t lang_cp_count = 0;
    {
        size_t pos = 0;
        while (pos < lang_byte_len) {
            size_t cb = 0;
            utf8_to_vga_glyph(&lang_name[pos], lang_byte_len - pos, &cb);
            if (cb == 0) { pos++; continue; }
            pos += cb;
            lang_cp_count++;
        }
    }

    size_t disp_cap = lang_byte_len + 16u;
    if (disp_cap < 64u) disp_cap = 64u;
    char lang_stack[64];
    char *disp_buf = (disp_cap <= sizeof(lang_stack)) ? lang_stack : (char *)kmalloc(disp_cap);
    if (disp_buf) {
        if ((int)lang_cp_count <= vis_chars) {
            memcpy(disp_buf, lang_name, lang_byte_len);
            disp_buf[lang_byte_len] = '\0';
        } else {
            const char *sep = "   *   ";
            size_t sep_byte_len = strlen(sep);
            size_t sep_cp_count = sep_byte_len;
            size_t loop_cp_len = lang_cp_count + sep_cp_count;
            uint32_t now = timer_millis();
            if (now - wm_lang_last_tick >= 180u) {
                wm_lang_last_tick = now;
                wm_lang_offset = (wm_lang_offset + 1u) % (uint32_t)loop_cp_len;
            }
            size_t out = 0;
            for (int c = 0; c < vis_chars; c++) {
                size_t cp_idx = (wm_lang_offset + (size_t)c) % loop_cp_len;
                if (cp_idx < lang_cp_count) {
                    size_t pos = 0;
                    size_t cp_i = 0;
                    while (cp_i < cp_idx && pos < lang_byte_len) {
                        size_t cb = 0;
                        utf8_to_vga_glyph(&lang_name[pos], lang_byte_len - pos, &cb);
                        if (cb == 0) { pos++; continue; }
                        pos += cb;
                        cp_i++;
                    }
                    size_t cb = 0;
                    utf8_to_vga_glyph(&lang_name[pos], lang_byte_len - pos, &cb);
                    if (cb == 0) cb = 1;
                    for (size_t b = 0; b < cb && out + 1 < disp_cap; b++)
                        disp_buf[out++] = lang_name[pos + b];
                } else {
                    size_t sep_idx = cp_idx - lang_cp_count;
                    if (sep_idx < sep_byte_len && out + 1 < disp_cap)
                        disp_buf[out++] = sep[sep_idx];
                }
            }
            disp_buf[out] = '\0';
        }
        wm_buf_draw_string(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, 198, TASKBAR_Y + 3, disp_buf, WIN_CLR_DKSHADOW);
        if (disp_buf != lang_stack) kfree(disp_buf);
    }

    /* System tray clock from hardware CMOS RTC (x = 282 .. 317) */
    draw_3d_box(bb, 282, TASKBAR_Y + 2, 36, 10, true);
    int rtc_h = 0, rtc_m = 0;
    wm_get_rtc_time(&rtc_h, &rtc_m);
    char clock_buf[6];
    clock_buf[0] = (char)('0' + (rtc_h / 10));
    clock_buf[1] = (char)('0' + (rtc_h % 10));
    clock_buf[2] = ':';
    clock_buf[3] = (char)('0' + (rtc_m / 10));
    clock_buf[4] = (char)('0' + (rtc_m % 10));
    clock_buf[5] = '\0';
    wm_buf_draw_string(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, 285, TASKBAR_Y + 3, clock_buf, WIN_CLR_DKSHADOW);
}

void wm_compose(uint8_t *backbuf) {
    /* 1. Windows 95 Teal desktop */
    memset(backbuf, WIN_CLR_DESKTOP, VGA_GFX_SIZE);

    wm_window_t *fw = wm_get_focused();
    bool top_fullscreen = (fw && fw->fullscreen && !fw->minimized && wm_window_is_visible_for_fg(fw));
    int max_y = top_fullscreen ? VGA_GFX_HEIGHT : TASKBAR_Y;

    /* 2. Windows (back to front). If an opaque fullscreen window is focused, draw it alone over everything. */
    for (wm_window_t *win = wm_head; win; win = win->next) {
        if (!wm_window_is_visible_for_fg(win)) continue;
        if (win->minimized) continue;
        if (top_fullscreen && win != fw) continue;
        if (!win->framebuf) continue;

        int win_w = (int)win->w;
        int win_h = (int)win->h;
        int win_x = (int)win->x;
        int win_y = (int)win->y;

        int row_start = 0;
        int row_end = win_h;
        if (win_y < 0) row_start = -win_y;
        if (win_y + row_end > max_y) row_end = max_y - win_y;

        int col_start = 0;
        int col_end = win_w;
        if (win_x < 0) col_start = -win_x;
        if (win_x + col_end > VGA_GFX_WIDTH) col_end = VGA_GFX_WIDTH - win_x;

        int visible_width = col_end - col_start;

        if (visible_width > 0 && row_start < row_end) {
            bool has_mask = (win->shape_mask && win->mask_w == win->w && win->mask_h == win->h);
            if (!has_mask) {
                for (int row = row_start; row < row_end; row++) {
                    int sy = win_y + row;
                    uint8_t *dst = &backbuf[(size_t)sy * VGA_GFX_WIDTH + (win_x + col_start)];
                    const uint8_t *src = &win->framebuf[(size_t)row * win_w + col_start];
                    memcpy(dst, src, (size_t)visible_width);
                }
            } else {
                for (int row = row_start; row < row_end; row++) {
                    int sy = win_y + row;
                    size_t dst_base = (size_t)sy * VGA_GFX_WIDTH + win_x;
                    size_t src_base = (size_t)row * win_w;
                    for (int col = col_start; col < col_end; col++) {
                        if (win->shape_mask[src_base + col]) {
                            backbuf[dst_base + col] = win->framebuf[src_base + col];
                        }
                    }
                }
            }
        }
        draw_decorations(win, backbuf);
    }

    /* 4. Taskbar at bottom (hidden if focused window is fullscreen) */
    if (!top_fullscreen) {
        draw_taskbar(backbuf);
    }
}

/* Keyboard dispatch (blocking wm_session_run mode) */
bool wm_dispatch_key(uint8_t scancode) {
    bool is_break=(scancode&0x80)!=0;
    if (is_break) return true;
    uint8_t make=scancode&0x7F;
    if (make == 0x57) { /* F11: toggle fullscreen */
        wm_window_t *fw = wm_get_focused();
        if (fw) wm_set_fullscreen(fw, !fw->fullscreen);
        return true;
    }
    if (make==0x0F && keyboard_is_ctrl_pressed() && !keyboard_is_alt_pressed()) {
        if(keyboard_is_shift_pressed()) wm_focus_prev(); else wm_focus_next();
        return true;
    }
    if (keyboard_is_ctrl_pressed()) {
        wm_window_t *fw=wm_get_focused();
        if (fw) {
            if(make==0x48){wm_move(fw,fw->x,(int16_t)(fw->y-1));return true;}
            if(make==0x50){wm_move(fw,fw->x,(int16_t)(fw->y+1));return true;}
            if(make==0x4B){wm_move(fw,(int16_t)(fw->x-1),fw->y);return true;}
            if(make==0x4D){wm_move(fw,(int16_t)(fw->x+1),fw->y);return true;}
        }
    }
    wm_window_t *fw=wm_get_focused();
    if (fw) wm_dispatch_window_event(fw, WM_EVENT_KEY_DOWN, (uint32_t)scancode);
    return true;
}

/* Compositor tick and mouse interactions */
enum {
    DRAG_NONE = 0,
    DRAG_MOVE,
    DRAG_RESIZE_BR,
    DRAG_RESIZE_R,
    DRAG_RESIZE_B,
    DRAG_RESIZE_BL,
    DRAG_RESIZE_L
};
static int16_t drag_offset_x = 0, drag_offset_y = 0;
static uint16_t drag_start_w = 0, drag_start_h = 0;
static int16_t drag_start_x = 0, drag_start_y = 0;
static int32_t drag_start_mx = 0, drag_start_my = 0;

static void wm_cancel_mouse_interaction(void) {
    mouse_state_t ms;
    mouse_get_state(&ms);
    drag_win = NULL;
    drag_mode = DRAG_NONE;
    wm_prev_left = ms.left_button;
    wm_prev_right = ms.right_button;
}

static void wm_restore_maximized_for_drag(wm_window_t *win, int32_t mouse_x, int32_t mouse_y) {
    if (!win || !win->maximized || win->w == 0 || win->h == 0) return;

    int32_t local_x = mouse_x - win->x;
    int32_t local_y = mouse_y - win->y;
    uint16_t restore_w = win->saved_w >= WM_MIN_WIDTH ? win->saved_w : 140;
    uint16_t restore_h = win->saved_h >= WM_MIN_HEIGHT ? win->saved_h : 100;
    drag_offset_x = (int16_t)(local_x * restore_w / win->w);
    drag_offset_y = (int16_t)(local_y * restore_h / win->h);
    win->x = win->saved_x >= 2 ? win->saved_x : 2;
    win->y = win->saved_y >= TITLEBAR_H + 2 ? win->saved_y : TITLEBAR_H + 2;
    win->maximized = false;
    wm_resize(win, restore_w, restore_h);
}

static bool wm_raw_gfx_push_scancode(wm_window_t *win, uint8_t sc) {
    if (!win || !win->is_raw_gfx_bridge || sc == 0) return false;
    if (win->raw_kbd_head >= win->raw_kbd_tail) {
        win->raw_kbd_head = 0;
        win->raw_kbd_tail = 0;
    }
    if (win->raw_kbd_tail >= win->raw_kbd_cap) {
        if (win->raw_kbd_head > 0) {
            size_t count = win->raw_kbd_tail - win->raw_kbd_head;
            memmove(win->raw_kbd_queue, win->raw_kbd_queue + win->raw_kbd_head, count);
            win->raw_kbd_head = 0;
            win->raw_kbd_tail = count;
        } else {
            size_t new_cap = win->raw_kbd_cap ? (win->raw_kbd_cap * 2u) : 64u;
            uint8_t *nb = (uint8_t *)krealloc(win->raw_kbd_queue, new_cap);
            if (!nb) return false;
            win->raw_kbd_queue = nb;
            win->raw_kbd_cap = new_cap;
        }
    }
    win->raw_kbd_queue[win->raw_kbd_tail++] = sc;
    return true;
}

static void wm_clamp_left_resize(int dx, int *out_nx, int *out_nw) {
    int nw = (int)drag_start_w - dx;
    int nx = (int)drag_start_x + dx;
    if (nw < WM_MIN_WIDTH) {
        nx = (int)drag_start_x + (int)drag_start_w - WM_MIN_WIDTH;
        nw = WM_MIN_WIDTH;
    }
    if (nw > WM_MAX_WIDTH) {
        nx = (int)drag_start_x + (int)drag_start_w - WM_MAX_WIDTH;
        nw = WM_MAX_WIDTH;
    }
    if (nx < 2) {
        nw -= (2 - nx);
        nx = 2;
        if (nw < WM_MIN_WIDTH) nw = WM_MIN_WIDTH;
    }
    *out_nx = nx;
    *out_nw = nw;
}

static ipo_key_decoder_t s_wm_decoder = {0};
static bool s_wm_decoder_armed = false;

void wm_compositor_tick(void) {
    terminal_poll_serial_commands();
    if (!wm_sess_active) {
        s_wm_decoder_armed = false;
        return;
    }
    if (!s_wm_decoder_armed) {
        ipo_key_decoder_reset(&s_wm_decoder);
        s_wm_decoder_armed = true;
    }
    if (process_is_batch_active() && process_get_separate_windows()) {
        process_t *cur_fg = process_get_foreground();
        if (cur_fg && !cur_fg->is_wm_app && !cur_fg->wants_graphics) {
            wm_cancel_mouse_interaction();
            return;
        }
    }

    /* Keyboard */
    uint8_t sc;
    while ((sc = keyboard_get_scancode_raw()) != 0) {
        update_hot_key_state(sc);
        ipo_key_event_t kev;
        if (!ipo_key_decoder_feed(&s_wm_decoder, sc, &kev)) {
            continue;
        }

        wm_window_t *fw = wm_get_focused();
        uint32_t fw_handle = fw ? (uint32_t)(uintptr_t)fw : 0u;
        uint32_t target_pid = fw ? fw->owner_pid : 0u;
        bool consumed = false;
        (void)ipo_input_process_key_event(&kev, fw_handle, target_pid, true, &consumed);
        if (consumed) {
            continue;
        }

        bool is_break = (kev.flags & IPO_KEY_FLAG_BREAK) != 0;
        bool is_e0    = (kev.flags & IPO_KEY_FLAG_EXT_E0) != 0;
        bool is_e1    = (kev.flags & IPO_KEY_FLAG_EXT_E1) != 0;

        if (!is_e1 && !is_break) {
            uint8_t make = (uint8_t)(kev.keycode & 0x7Fu);
            if (!is_e0 && make == 0x3Cu) { /* F2 key */
                dynamic_keymap_cycle_next();
                continue;
            } else if (!is_e0 && make == 0x57u) { /* F11: toggle fullscreen on focused window */
                if (fw) wm_set_fullscreen(fw, !fw->fullscreen);
                continue;
            } else if (!is_e0 && make == 0x0Fu && keyboard_is_ctrl_pressed() && !keyboard_is_alt_pressed()) { /* Ctrl+Tab / Ctrl+Shift+Tab */
                if (keyboard_is_shift_pressed()) wm_focus_prev(); else wm_focus_next();
                continue;
            } else if (keyboard_is_ctrl_pressed() && (make == 0x49u || make == 0x51u)) {
                /* Ctrl + PageUp (0x49) / Ctrl + PageDown (0x51): switch workspace in batch mode */
                if (process_is_batch_active() && process_get_separate_windows()) {
                    if (make == 0x49u) {
                        process_switch_foreground_prev();
                    } else {
                        process_switch_foreground_next();
                    }
                    process_t *nfg = process_get_foreground();
                    if (nfg && (nfg->is_wm_app || !process_get_separate_windows())) {
                        wm_focus_window_for_pid(nfg->pid);
                        wm_invalidate_all();
                    }
                }
                return;
            } else if (keyboard_is_ctrl_pressed() && (make == 0x48u || make == 0x50u || make == 0x4Bu || make == 0x4Du)) {
                if (fw) {
                    if (make == 0x48u) wm_move(fw, fw->x, (int16_t)(fw->y - 4));
                    else if (make == 0x50u) wm_move(fw, fw->x, (int16_t)(fw->y + 4));
                    else if (make == 0x4Bu) wm_move(fw, (int16_t)(fw->x - 4), fw->y);
                    else if (make == 0x4Du) wm_move(fw, (int16_t)(fw->x + 4), fw->y);
                }
                continue;
            }
        }

        if (fw) {
            if (fw->is_raw_gfx_bridge) {
                for (uint8_t bi = 0; bi < kev.raw_len; bi++) {
                    wm_raw_gfx_push_scancode(fw, kev.raw_bytes[bi]);
                }
            } else {
                uint32_t ext_data = (kev.keycode & 0x7FFFFFFFu) | (is_break ? 0x80000000u : 0u);
                wm_dispatch_window_event(fw, WM_EVENT_KEY_EXT, ext_data);
                for (uint8_t bi = 0; bi < kev.raw_len; bi++) {
                    uint8_t b = kev.raw_bytes[bi];
                    uint32_t ev_t = (b == 0xE0u || b == 0xE1u) ? WM_EVENT_KEY_DOWN
                                    : ((b & 0x80u) ? WM_EVENT_KEY_UP : WM_EVENT_KEY_DOWN);
                    wm_dispatch_window_event(fw, ev_t, (uint32_t)b);
                }
            }
        }
    }
    if (!wm_sess_active) return;
    if (process_is_batch_active() && process_get_separate_windows()) {
        process_t *cur_fg = process_get_foreground();
        if (cur_fg && !cur_fg->is_wm_app && !cur_fg->wants_graphics) {
            wm_cancel_mouse_interaction();
            return;
        }
    }

    /* Mouse */
    mouse_poll();
    if (!wm_sess_active) return;
    if (process_is_batch_active() && process_get_separate_windows()) {
        process_t *cur_fg = process_get_foreground();
        if (cur_fg && !cur_fg->is_wm_app && !cur_fg->wants_graphics) {
            wm_cancel_mouse_interaction();
            return;
        }
    }
    if (!vga_is_graphics_mode()) return;

    mouse_state_t ms;
    mouse_get_state(&ms);
    bool left_now = ms.left_button;
    bool right_now = ms.right_button;
    bool left_pressed = left_now && !wm_prev_left;
    bool left_released = !left_now && wm_prev_left;
    bool right_pressed = right_now && !wm_prev_right;
    wm_prev_left = left_now;
    wm_prev_right = right_now;

    wm_window_t *top_win = wm_get_focused();
    bool top_fullscreen = (top_win && top_win->fullscreen && !top_win->minimized);

    if (right_pressed) {
        if (ms.y >= TASKBAR_Y && !top_fullscreen) {
            if (ms.x >= 54 && ms.x <= 182) {
                int rel_x = (int)ms.x - 54;
                int tab_idx = rel_x / 43;
                if (rel_x % 43 <= 40) {
                    int target_idx = taskbar_scroll_offset + tab_idx;
                    int cur_idx = 0;
                    for (wm_window_t *w = wm_head; w; w = w->next) {
                        if (!wm_window_is_visible_for_fg(w)) continue;
                        if (cur_idx == target_idx) {
                            if (wm_window_has_close_button(w)) {
                                wm_close_window_by_user(w);
                                if (taskbar_scroll_offset > 0 && taskbar_scroll_offset >= wm_count) {
                                    taskbar_scroll_offset = wm_count > 0 ? wm_count - 1 : 0;
                                }
                            }
                            break;
                        }
                        cur_idx++;
                    }
                }
            }
            else if (ms.x >= 195 && ms.x <= 279) {
                dynamic_keymap_cycle_prev();
            }
        }
    }
    if (left_pressed) {
        bool handled = false;
        drag_win = NULL;
        drag_mode = DRAG_NONE;

        process_t *fg = process_get_displayed_foreground();
        if (!fg) fg = process_get_foreground();
        uint32_t overlay_flags = fg ? fg->gfx_overlay_flags : 0;
        bool show_button = !(overlay_flags & VGA_OVERLAY_HIDE_BUTTON);

        /* 1. Check Taskbar */
        if (ms.y >= TASKBAR_Y && !top_fullscreen) {
            /* Close button */
            if (show_button && ms.x >= 2 && ms.x <= 42) {
                wm_handle_close_button_click();
                return;
            }
            /* Scroll left '<' */
            if (ms.x >= 44 && ms.x <= 53) {
                if (taskbar_scroll_offset > 0) taskbar_scroll_offset--;
                handled = true;
            }
            /* Scroll right '>' */
            if (ms.x >= 184 && ms.x <= 193) {
                if (taskbar_scroll_offset + 3 < wm_count_visible_windows()) taskbar_scroll_offset++;
                handled = true;
            }
            /* Window tabs */
            if (!handled && ms.x >= 54 && ms.x <= 182) {
                int rel_x = (int)ms.x - 54;
                int tab_idx = rel_x / 43;
                if (rel_x % 43 <= 40) {
                    int target_idx = taskbar_scroll_offset + tab_idx;
                    int tab_in_x = rel_x % 43;
                    int cur_idx = 0;
                    for (wm_window_t *w = wm_head; w; w = w->next) {
                        if (!wm_window_is_visible_for_fg(w)) continue;
                        if (cur_idx == target_idx) {
                            if (wm_window_has_close_button(w) && tab_in_x >= 30 && tab_in_x <= 39) {
                                /* Clicked dedicated [x] button on this tab: close ONLY this window! */
                                wm_close_window_by_user(w);
                                int vis_cnt = wm_count_visible_windows();
                                if (taskbar_scroll_offset > 0 && taskbar_scroll_offset >= vis_cnt) {
                                    taskbar_scroll_offset = vis_cnt > 0 ? vis_cnt - 1 : 0;
                                }
                                handled = true;
                                break;
                            }
                            if (w->minimized) {
                                w->minimized = false;
                                wm_set_focus(w);
                            } else if (w != wm_get_focused()) {
                                wm_set_focus(w);
                            } else {
                                /* Already active and focused -> minimize */
                                w->minimized = true;
                                for (wm_window_t *pw = wm_tail; pw; pw = pw->prev) {
                                    if (wm_window_is_visible_for_fg(pw) && !pw->minimized) {
                                        wm_set_focus(pw);
                                        break;
                                    }
                                }
                            }
                            handled = true;
                            break;
                        }
                        cur_idx++;
                    }
                }
            }
            /* Language box click */
            if (!handled && ms.x >= 195 && ms.x <= 279) {
                dynamic_keymap_cycle_next();
                handled = true;
            }
            handled = true;
        }

        /* 2. Check Windows (top to bottom) */
        if (!handled) {
            for (wm_window_t *w = wm_tail; w; w = w->prev) {
                if (!wm_window_is_visible_for_fg(w)) continue;
                if (w->minimized) continue;

                int wx0 = (int)w->x - 2;
                int wy0 = (int)w->y - TITLEBAR_H - 2;
                int wx1 = (int)w->x + (int)w->w + 2;
                int wy1 = (int)w->y + (int)w->h + 2;

                if (ms.x >= wx0 && ms.x <= wx1 && ms.y >= wy0 && ms.y <= wy1) {
                    wm_set_focus(w);
                    handled = true;

                    if (!w->fullscreen && w->decor_style == WM_DECOR_DEFAULT) {
                        /* Check Titlebar (any click at or above client top edge y) */
                        if (ms.y < (int)w->y) {
                            int titlebar_button_y = (int)w->y - TITLEBAR_H + 1;
                            bool in_titlebar_button_y =
                                ms.y >= titlebar_button_y && ms.y < titlebar_button_y + 8;

                            /* Fullscreen button [+] */
                            if (in_titlebar_button_y && (int)w->w >= 40 &&
                                ms.x >= (int)w->x + (int)w->w - 36 &&
                                ms.x < (int)w->x + (int)w->w - 28) {
                                wm_set_fullscreen(w, true);
                                break;
                            }

                            /* Close button [X] */
                            if (wm_window_has_close_button(w) && in_titlebar_button_y &&
                                ms.x >= (int)w->x + (int)w->w - 9 &&
                                ms.x < (int)w->x + (int)w->w - 1) {
                                wm_close_window_by_user(w);
                                break;
                            }

                            /* Maximize / Restore button [□] */
                            if (in_titlebar_button_y &&
                                ms.x >= (int)w->x + (int)w->w - 18 &&
                                ms.x < (int)w->x + (int)w->w - 10) {
                                wm_toggle_maximize(w);
                                break;
                            }

                            /* Minimize button [_] */
                            if (in_titlebar_button_y &&
                                ms.x >= (int)w->x + (int)w->w - 27 &&
                                ms.x < (int)w->x + (int)w->w - 19) {
                                wm_minimize(w);
                                break;
                            }

                            /* Title bar double-click to toggle maximize */
                            uint32_t now = timer_millis();
                            if (last_title_click_win == w && (now - last_title_click_time) < 400) {
                                last_title_click_win = NULL;
                                wm_toggle_maximize(w);
                                break;
                            }
                            last_title_click_win = w;
                            last_title_click_time = now;

                            /* Title bar drag to move */
                            drag_mode = DRAG_MOVE;
                            drag_win = w;
                            drag_offset_x = (int16_t)(ms.x - w->x);
                            drag_offset_y = (int16_t)(ms.y - w->y);
                            drag_start_x = w->x;
                            drag_start_y = w->y;
                            drag_start_mx = ms.x;
                            drag_start_my = ms.y;
                            break;
                        }

                        /* Bottom-right corner resize: strictly at corner */
                        if (ms.x >= (int)w->x + (int)w->w - 6 && ms.x <= (int)w->x + (int)w->w + 4 &&
                            ms.y >= (int)w->y + (int)w->h - 6 && ms.y <= (int)w->y + (int)w->h + 4) {
                            drag_mode = DRAG_RESIZE_BR;
                            drag_win = w;
                            drag_start_w = w->w;
                            drag_start_h = w->h;
                            drag_start_x = w->x;
                            drag_start_y = w->y;
                            drag_start_mx = ms.x;
                            drag_start_my = ms.y;
                            break;
                        }

                        /* Bottom-left corner resize: strictly at corner */
                        if (ms.x >= (int)w->x - 4 && ms.x <= (int)w->x + 6 &&
                            ms.y >= (int)w->y + (int)w->h - 6 && ms.y <= (int)w->y + (int)w->h + 4) {
                            drag_mode = DRAG_RESIZE_BL;
                            drag_win = w;
                            drag_start_w = w->w;
                            drag_start_h = w->h;
                            drag_start_x = w->x;
                            drag_start_y = w->y;
                            drag_start_mx = ms.x;
                            drag_start_my = ms.y;
                            break;
                        }

                        /* Left border resize: on left edge frame */
                        if (ms.x >= (int)w->x - 4 && ms.x <= (int)w->x + 1 &&
                            ms.y >= (int)w->y && ms.y <= (int)w->y + (int)w->h) {
                            drag_mode = DRAG_RESIZE_L;
                            drag_win = w;
                            drag_start_w = w->w;
                            drag_start_h = w->h;
                            drag_start_x = w->x;
                            drag_start_y = w->y;
                            drag_start_mx = ms.x;
                            drag_start_my = ms.y;
                            break;
                        }

                        /* Right border resize: on right edge frame */
                        if (ms.x >= (int)w->x + (int)w->w - 2 && ms.x <= (int)w->x + (int)w->w + 4 &&
                            ms.y >= (int)w->y && ms.y <= (int)w->y + (int)w->h) {
                            drag_mode = DRAG_RESIZE_R;
                            drag_win = w;
                            drag_start_w = w->w;
                            drag_start_h = w->h;
                            drag_start_x = w->x;
                            drag_start_y = w->y;
                            drag_start_mx = ms.x;
                            drag_start_my = ms.y;
                            break;
                        }

                        /* Bottom border resize: on bottom edge frame */
                        if (ms.y >= (int)w->y + (int)w->h - 2 && ms.y <= (int)w->y + (int)w->h + 4 &&
                            ms.x >= (int)w->x && ms.x <= (int)w->x + (int)w->w) {
                            drag_mode = DRAG_RESIZE_B;
                            drag_win = w;
                            drag_start_w = w->w;
                            drag_start_h = w->h;
                            drag_start_x = w->x;
                            drag_start_y = w->y;
                            drag_start_mx = ms.x;
                            drag_start_my = ms.y;
                            break;
                        }
                    }

                    /* Client area clicks are delivered without starting a window drag. */
                    if (ms.x >= (int)w->x && ms.x < (int)w->x + (int)w->w &&
                        ms.y >= (int)w->y && ms.y < (int)w->y + (int)w->h) {
                        if (w->event_cb && (w->owner_pid == 0 || process_is_alive(w->owner_pid))) {
                            uint32_t click_data =
                                (((uint32_t)(ms.x - w->x) & 0xFFFF) << 16) |
                                ((uint32_t)(ms.y - w->y) & 0xFFFF);
                            wm_dispatch_window_event(w, WM_EVENT_CLICK, click_data);
                        }
                    }
                    break;
                }
            }
        }

        /* Desktop Exit button removed - only single Start/Exit button on taskbar */
    } else if (left_now && drag_win != NULL) {
        /* Mouse drag in progress */
        if (drag_mode == DRAG_MOVE) {
            if (drag_win->maximized) {
                int dx = (int)ms.x - (int)drag_start_mx;
                int dy = (int)ms.y - (int)drag_start_my;
                if (dx * dx + dy * dy > 9) {
                    wm_restore_maximized_for_drag(drag_win, drag_start_mx, drag_start_my);
                }
            }
            if (!drag_win->maximized) {
                int16_t nx = (int16_t)(ms.x - drag_offset_x);
                int16_t ny = (int16_t)(ms.y - drag_offset_y);
                if (ny < TITLEBAR_H + 2) ny = TITLEBAR_H + 2;
                if (ny > TASKBAR_Y - 16) ny = TASKBAR_Y - 16;
                if (nx < -(int16_t)drag_win->w + 30) nx = -(int16_t)drag_win->w + 30;
                if (nx > VGA_GFX_WIDTH - 30) nx = VGA_GFX_WIDTH - 30;
                wm_move(drag_win, nx, ny);
            }
        } else if (drag_mode == DRAG_RESIZE_BR) {
            int nw = (int)drag_start_w + ((int)ms.x - (int)drag_start_mx);
            int nh = (int)drag_start_h + ((int)ms.y - (int)drag_start_my);
            if (nw < WM_MIN_WIDTH) nw = WM_MIN_WIDTH;
            if (nw > WM_MAX_WIDTH) nw = WM_MAX_WIDTH;
            if (nh < WM_MIN_HEIGHT) nh = WM_MIN_HEIGHT;
            if (nh > WM_MAX_HEIGHT) nh = WM_MAX_HEIGHT;
            drag_win->maximized = false;
            wm_resize(drag_win, (uint16_t)nw, (uint16_t)nh);
        } else if (drag_mode == DRAG_RESIZE_R) {
            int nw = (int)drag_start_w + ((int)ms.x - (int)drag_start_mx);
            if (nw < WM_MIN_WIDTH) nw = WM_MIN_WIDTH;
            if (nw > WM_MAX_WIDTH) nw = WM_MAX_WIDTH;
            drag_win->maximized = false;
            wm_resize(drag_win, (uint16_t)nw, drag_win->h);
        } else if (drag_mode == DRAG_RESIZE_B) {
            int nh = (int)drag_start_h + ((int)ms.y - (int)drag_start_my);
            if (nh < WM_MIN_HEIGHT) nh = WM_MIN_HEIGHT;
            if (nh > WM_MAX_HEIGHT) nh = WM_MAX_HEIGHT;
            drag_win->maximized = false;
            wm_resize(drag_win, drag_win->w, (uint16_t)nh);
        } else if (drag_mode == DRAG_RESIZE_L) {
            int nx, nw;
            wm_clamp_left_resize((int)ms.x - (int)drag_start_mx, &nx, &nw);
            drag_win->maximized = false;
            drag_win->x = (int16_t)nx;
            wm_resize(drag_win, (uint16_t)nw, drag_win->h);
        } else if (drag_mode == DRAG_RESIZE_BL) {
            int nx, nw;
            wm_clamp_left_resize((int)ms.x - (int)drag_start_mx, &nx, &nw);
            int nh = (int)drag_start_h + ((int)ms.y - (int)drag_start_my);
            if (nh < WM_MIN_HEIGHT) nh = WM_MIN_HEIGHT;
            if (nh > WM_MAX_HEIGHT) nh = WM_MAX_HEIGHT;
            drag_win->maximized = false;
            drag_win->x = (int16_t)nx;
            wm_resize(drag_win, (uint16_t)nw, (uint16_t)nh);
        }
    } else if (left_released) {
        if (drag_win && (drag_mode == DRAG_RESIZE_BR || drag_mode == DRAG_RESIZE_R || drag_mode == DRAG_RESIZE_B || drag_mode == DRAG_RESIZE_L || drag_mode == DRAG_RESIZE_BL)) {
            drag_win->saved_w = drag_win->w;
            drag_win->saved_h = drag_win->h;
            drag_win->saved_x = drag_win->x;
            drag_win->saved_y = drag_win->y;
            drag_win->maximized = false;
        }
        drag_mode = DRAG_NONE;
        drag_win = NULL;
    }

    static int32_t last_rendered_mx = -1;
    static int32_t last_rendered_my = -1;
    static uint32_t last_rendered_time = 0;
    bool any_dirty = false;
    for (wm_window_t *w = wm_head; w; w = w->next) {
        if (w->dirty) {
            any_dirty = true;
            w->dirty = false;
        }
    }

    uint32_t now = timer_millis();
    bool mouse_moved = (ms.x != last_rendered_mx || ms.y != last_rendered_my ||
                        left_pressed || left_released || right_pressed);
    bool time_to_update_clock = (now - last_rendered_time >= 500u);

    if (!any_dirty && !mouse_moved && !time_to_update_clock && drag_mode == DRAG_NONE) {
        return;
    }

    last_rendered_mx = ms.x;
    last_rendered_my = ms.y;
    last_rendered_time = now;

    if (!vga_is_graphics_mode()) {
        return;
    }

    /* Compose & flip */
    uint8_t *bb = wm_get_backbuf();
    if (bb) {
        wm_compose(bb);
        process_t *fg = process_get_displayed_foreground();
        if (!fg) fg = process_get_foreground();
        uint32_t overlay_flags = fg ? fg->gfx_overlay_flags : 0;
        bool show_cursor = !(overlay_flags & VGA_OVERLAY_HIDE_CURSOR);
        if (show_cursor) {
            draw_cursor(bb, (int)ms.x, (int)ms.y);
        }
        vga_gfx_blit_hardware(bb, true, VGA_GFX_WIDTH, VGA_GFX_HEIGHT);
    }
}

/* Session API */
bool wm_session_active(void) { return wm_sess_active; }

void wm_get_screen_bounds(int *out_w, int *out_h) {
    vga_gfx_get_screen_bounds(out_w, out_h);
}

/** wm_session_start — activate the compositor session and return immediately. */
void wm_session_start(void) {
    if (wm_sess_active) return;

    process_t *fg = process_get_foreground();
    process_t *cur_proc = process_get_current();
    bool should_switch_hw = (!process_is_batch_active() || !process_get_separate_windows() ||
                             (fg && (fg->wants_graphics || fg->is_wm_app ||
                                     (cur_proc && fg->window_id == cur_proc->window_id))));

    int screen_w = VGA_GFX_WIDTH, screen_h = VGA_GFX_HEIGHT;
    if (should_switch_hw) {
        vga_set_mode_13h_hardware();
        vga_gfx_init_default_palette();
        mouse_set_bounds(VGA_GFX_WIDTH, VGA_GFX_HEIGHT);
        keyboard_flush_queue();
        keyboard_flush_hardware();
    }

    wm_sess_active = true;
    wm_prev_left   = false;
    drag_mode      = DRAG_NONE;
    drag_win       = NULL;
    taskbar_scroll_offset = 0;
    raw_overlay_has_saved = false;

    /* Draw initial frame */
    uint8_t *init_bb = wm_get_backbuf();
    if (init_bb) {
        wm_compose(init_bb);
        draw_cursor(init_bb, screen_w / 2, screen_h / 2);
        if (should_switch_hw) {
            vga_gfx_blit_hardware(init_bb, true, VGA_GFX_WIDTH, VGA_GFX_HEIGHT);
        }
    }
}

/** wm_session_stop — stop compositor, destroy all windows, restore text mode. */
void wm_session_stop(void) {
    if (!wm_sess_active) return;
    wm_sess_active = false;
    drag_mode      = DRAG_NONE;
    drag_win       = NULL;
    taskbar_scroll_offset = 0;
    raw_overlay_has_saved = false;
    while (wm_head) wm_destroy_window(wm_head);
    keyboard_flush_queue();
    vga_set_mode_text_hardware();
    dynamic_keymap_reapply_fonts();
    mouse_set_bounds_from_display();

    /* If a terminal app keeps running after WM exits in shared workspace (&&),
     * flush any stdout it buffered while WM covered the screen. */
    if (process_is_batch_active() && !process_get_separate_windows()) {
        for (uint32_t pid = 1; pid <= 32; pid++) {
            process_t *tp = process_find_by_pid(pid);
            if (tp && tp->is_running && !tp->is_wm_app && !tp->wants_graphics &&
                tp->text_output_buf && tp->text_vga_written_len < tp->text_output_len) {
                for (uint32_t i = tp->text_vga_written_len; i < tp->text_output_len; i++) {
                    putchar(tp->text_output_buf[i]);
                }
                tp->text_vga_written_len = tp->text_output_len;
            }
        }
    }
}

/**
 * wm_session_run — BLOCKING version (single-app mode).
 */
void wm_session_run(void) {
    if (!wm_sess_active) {
        wm_session_start();
    }
    while (wm_sess_active) {
        for (wm_window_t *w = wm_head; w; w = w->next) {
            if (w->draw_cb) {
                wm_invoke_owner_cb(w, NULL, 0, 0, w->draw_cb,
                                   process_in_process_context());
            }
        }
        wm_compositor_tick();
    }
}

void wm_terminal_putc(char c, uint8_t fg, uint8_t bg) {
    /* Terminal apps render natively in VGA text mode behind WM, not in a WM Terminal window. */
    (void)c;
    (void)fg;
    (void)bg;
}

void wm_raw_gfx_overlay_tick(void) {
    if (!vga_is_graphics_mode() || wm_sess_active) {
        raw_overlay_has_saved = false;
        return;
    }

    process_t *fg = process_get_displayed_foreground();
    if (!fg) fg = process_get_foreground();
    uint32_t overlay_flags = fg ? fg->gfx_overlay_flags : 0;

    if ((overlay_flags & VGA_OVERLAY_HIDE_ALL) == VGA_OVERLAY_HIDE_ALL) return;

    mouse_poll();
    mouse_state_t ms;
    mouse_get_state(&ms);
    bool left_now = ms.left_button;
    bool left_pressed = left_now && !wm_prev_left;
    wm_prev_left = left_now;

    uint8_t *vram = (uint8_t *)0xA0000;

    static int prev_cx = -1, prev_cy = -1;
    static uint8_t saved_under[64];

    bool show_cursor = !(overlay_flags & VGA_OVERLAY_HIDE_CURSOR);
    bool show_button = !(overlay_flags & VGA_OVERLAY_HIDE_BUTTON);

    /* Restore pixels under the previous cursor position */
    if (raw_overlay_has_saved && prev_cx >= 0 && prev_cy >= 0) {
        for (int row = 0; row < 8; row++) {
            int py = prev_cy + row;
            if (py < 0 || py >= VGA_GFX_HEIGHT) continue;
            for (int col = 0; col < 8; col++) {
                int px = prev_cx + col;
                if (px >= 0 && px < VGA_GFX_WIDTH) {
                    vram[py * VGA_GFX_WIDTH + px] = saved_under[row * 8 + col];
                }
            }
        }
        raw_overlay_has_saved = false;
    }

    if (show_button) {
        wm_draw_close_button(vram, 2, TASKBAR_Y + 2, 40, 10, false);
    }

    if (show_button && left_pressed && ms.x >= 2 && ms.x <= 42 && ms.y >= TASKBAR_Y) {
        raw_overlay_has_saved = false;
        wm_handle_close_button_click();
        return;
    }

    if (show_cursor) {
        prev_cx = (int)ms.x;
        prev_cy = (int)ms.y;
        for (int row = 0; row < 8; row++) {
            int py = prev_cy + row;
            for (int col = 0; col < 8; col++) {
                int px = prev_cx + col;
                if (px >= 0 && px < VGA_GFX_WIDTH && py >= 0 && py < VGA_GFX_HEIGHT) {
                    saved_under[row * 8 + col] = vram[py * VGA_GFX_WIDTH + px];
                } else {
                    saved_under[row * 8 + col] = 0;
                }
            }
        }
        raw_overlay_has_saved = true;
        draw_cursor(vram, prev_cx, prev_cy);
    }
}

/* OS-level Raw Graphics Bridge */

static uint8_t wm_map_rgb6_to_wm_palette(uint8_t r6, uint8_t g6, uint8_t b6) {
    uint8_t r = (uint8_t)(r6 & 0x3Fu);
    uint8_t g = (uint8_t)(g6 & 0x3Fu);
    uint8_t b = (uint8_t)(b6 & 0x3Fu);

    if (r == g && g == b) {
        if (r == 0) return 0;
        if (r >= 62) return 15;
        return (uint8_t)(16u + ((uint32_t)r * 15u + 31u) / 63u);
    }

    int ri = ((int)r * 5 + 31) / 63;
    int gi = ((int)g * 5 + 31) / 63;
    int bi = ((int)b * 5 + 31) / 63;
    return (uint8_t)(32 + ri * 36 + gi * 6 + bi);
}

static wm_window_t *wm_find_raw_gfx_window_by_pid(uint32_t pid) {
    if (pid == 0) return NULL;
    for (wm_window_t *w = wm_head; w; w = w->next) {
        if (w->owner_pid == pid && w->is_raw_gfx_bridge) {
            return w;
        }
    }
    return NULL;
}

bool wm_has_raw_gfx_window_for_pid(uint32_t pid) {
    return wm_find_raw_gfx_window_by_pid(pid) != NULL;
}

void wm_destroy_raw_gfx_window_for_pid(uint32_t pid) {
    wm_window_t *w = wm_head;
    while (w) {
        wm_window_t *next = w->next;
        if (w->owner_pid == pid && w->is_raw_gfx_bridge) {
            wm_destroy_window(w);
        }
        w = next;
    }
}

wm_window_t *wm_ensure_raw_gfx_window(struct process *proc) {
    if (!proc) return NULL;
    wm_window_t *existing = wm_find_raw_gfx_window_by_pid(proc->pid);
    if (existing) return existing;

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = (proc->name && *proc->name) ? proc->name : "Graphics App";
    opt.x = 2;
    opt.y = 14;
    opt.w = 154;
    opt.h = 112;
    opt.bg_color = 0;
    opt.decor_style = WM_DECOR_DEFAULT;

    wm_window_t *win = wm_create_window(&opt);
    if (!win) return NULL;

    win->owner_pid = proc->pid;
    win->window_id = proc->window_id;
    win->is_raw_gfx_bridge = true;
    win->raw_kbd_queue = NULL;
    win->raw_kbd_cap = 0;
    win->raw_kbd_head = 0;
    win->raw_kbd_tail = 0;

    win->raw_lut = (uint8_t *)kmalloc(256u);
    if (win->raw_lut) {
        if (proc->gfx_palette_backup) {
            for (int i = 0; i < 256; i++) {
                win->raw_lut[i] = wm_map_rgb6_to_wm_palette(
                    proc->gfx_palette_backup[i * 3 + 0],
                    proc->gfx_palette_backup[i * 3 + 1],
                    proc->gfx_palette_backup[i * 3 + 2]);
            }
        } else {
            for (int i = 0; i < 256; i++) {
                win->raw_lut[i] = (uint8_t)i;
            }
        }
    }

    return win;
}

void wm_raw_gfx_set_palette(struct process *proc, uint8_t idx,
                            uint8_t r, uint8_t g, uint8_t b) {
    if (!proc) return;
    wm_window_t *win = wm_find_raw_gfx_window_by_pid(proc->pid);
    if (!win || !win->raw_lut) return;
    win->raw_lut[idx] = wm_map_rgb6_to_wm_palette(r, g, b);
    win->dirty = true;
}

void wm_present_raw_gfx_process(struct process *proc, const uint8_t *buf,
                                int src_w, int src_h) {
    if (!proc || !buf || src_w <= 0 || src_h <= 0) return;

    wm_window_t *win = wm_ensure_raw_gfx_window(proc);
    if (!win || !win->framebuf || win->w == 0 || win->h == 0) return;

    int dw = (int)win->w;
    int dh = (int)win->h;
    uint8_t *dst = win->framebuf;
    const uint8_t *lut = win->raw_lut;

    /* Sync palette LUT if app wrote gfx_palette_backup directly */
    if (!lut) {
        win->raw_lut = (uint8_t *)kmalloc(256u);
        lut = win->raw_lut;
        if (win->raw_lut) {
            for (int i = 0; i < 256; i++) win->raw_lut[i] = (uint8_t)i;
        }
    }

    int *lx = (int *)kmalloc((size_t)dw * sizeof(int));
    if (lx) {
        for (int x = 0; x < dw; x++) {
            lx[x] = (int)(((uint64_t)x * (uint32_t)src_w) / (uint32_t)dw);
        }
        for (int y = 0; y < dh; y++) {
            int sy = (int)(((uint64_t)y * (uint32_t)src_h) / (uint32_t)dh);
            const uint8_t *srow = buf + (size_t)sy * (size_t)src_w;
            uint8_t *drow = dst + (size_t)y * (size_t)dw;
            if (lut) {
                for (int x = 0; x < dw; x++) {
                    drow[x] = lut[srow[lx[x]]];
                }
            } else {
                for (int x = 0; x < dw; x++) {
                    drow[x] = srow[lx[x]];
                }
            }
        }
        kfree(lx);
    } else {
        for (int y = 0; y < dh; y++) {
            int sy = (int)(((uint64_t)y * (uint32_t)src_h) / (uint32_t)dh);
            const uint8_t *srow = buf + (size_t)sy * (size_t)src_w;
            uint8_t *drow = dst + (size_t)y * (size_t)dw;
            for (int x = 0; x < dw; x++) {
                int sx = (int)(((uint64_t)x * (uint32_t)src_w) / (uint32_t)dw);
                drow[x] = lut ? lut[srow[sx]] : srow[sx];
            }
        }
    }

    win->dirty = true;

    process_t *fg = process_get_displayed_foreground();
    if (!process_is_batch_active() || !process_get_separate_windows() ||
        (fg && fg->window_id == proc->window_id)) {
        wm_compositor_tick();
    }
}

uint8_t wm_raw_gfx_get_scancode(struct process *proc) {
    if (!proc) return 0;
    wm_window_t *win = wm_find_raw_gfx_window_by_pid(proc->pid);
    if (!win || !win->raw_kbd_queue || win->raw_kbd_head >= win->raw_kbd_tail) {
        return 0;
    }
    uint8_t sc = win->raw_kbd_queue[win->raw_kbd_head++];
    if (win->raw_kbd_head >= win->raw_kbd_tail) {
        win->raw_kbd_head = 0;
        win->raw_kbd_tail = 0;
    }
    return sc;
}
