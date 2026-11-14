#include <wm.h>
#include <vga_gfx.h>
#include <driver/vbe_bga.h>
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
#define TASKBAR_H  14

typedef struct {
    int y;
    int close_x;
    int close_w;
    int scroll_l_x;
    int scroll_w;
    int tabs_x0;
    int tabs_x1;
    int tab_w;
    int tab_stride;
    int max_tabs;
    int scroll_r_x;
    int lang_x;
    int lang_w;
    int clock_x;
    int clock_w;
} wm_taskbar_layout_t;

static void wm_get_dims(int *out_w, int *out_h) {
    if (out_w) *out_w = VGA_GFX_WIDTH;
    if (out_h) *out_h = VGA_GFX_HEIGHT;
}

static inline int wm_taskbar_y(int sh) { int y = sh - TASKBAR_H; return y > 0 ? y : 0; }
static inline int wm_max_win_w(int sw) { int w = sw - 4; return w > 1 ? w : (sw > 0 ? sw : 1); }
static inline int wm_max_win_h(int sh) { int h = sh - TASKBAR_H - TITLEBAR_H - 4; return h > 1 ? h : (sh > 0 ? sh : 1); }

static void wm_get_taskbar_layout(int sw, int sh, wm_taskbar_layout_t *out) {
    out->y = wm_taskbar_y(sh);
    out->close_x = 2;
    out->close_w = 40;
    out->scroll_l_x = 44;
    out->scroll_w = 9;
    out->clock_w = 36;
    out->clock_x = sw - out->clock_w - 2;
    out->lang_w = 85;
    out->lang_x = out->clock_x - out->lang_w - 2;
    out->scroll_r_x = out->lang_x - out->scroll_w - 2;
    out->tabs_x0 = 54;
    out->tabs_x1 = out->scroll_r_x - 2;
    out->tab_w = 40;
    out->tab_stride = 43;
    int span = out->tabs_x1 - out->tabs_x0 + 3;
    out->max_tabs = span > 0 ? (span / out->tab_stride) : 1;
    if (out->max_tabs < 1) out->max_tabs = 1;
}

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

static void wm_buf_put_pixel(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, uint8_t c) {
    if (!buf || x < 0 || y < 0 || x >= (int)bw || y >= (int)bh) return;
    buf[(size_t)y * bw + (size_t)x] = c;
}

static void wm_buf_draw_line(uint8_t *buf, uint16_t bw, uint16_t bh, int x0, int y0, int x1, int y1, uint8_t c) {
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

static void wm_buf_fill_rect(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, int w, int h, uint8_t c) {
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

static void wm_buf_draw_rect(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, int w, int h, uint8_t c) {
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

static void wm_buf_draw_char(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, char ch, uint8_t c) {
    wm_buf_draw_glyph_impl(buf, bw, bh, x, y, (uint8_t)ch, c, 0, false);
}

static void wm_buf_draw_char_bg(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, char ch, uint8_t fg, uint8_t bg) {
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

static void wm_buf_draw_string(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, const char *str, uint8_t c) {
    wm_buf_draw_string_impl(buf, bw, bh, x, y, str, c, 0, false);
}

static void wm_buf_draw_string_bg(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, const char *str, uint8_t fg, uint8_t bg) {
    wm_buf_draw_string_impl(buf, bw, bh, x, y, str, fg, bg, true);
}

#include <kernel/process.h>

static wm_window_t *wm_head  = NULL;
static wm_window_t *wm_tail  = NULL;
static int          wm_count = 0;

static bool wm_sess_active = false;
static bool wm_prev_left   = false;
static bool wm_prev_right  = false;
static uint8_t *wm_backbuf = NULL;
static size_t wm_backbuf_cap = 0;

static bool wm_surface_dirty = true;
static int s_last_rendered_sw = 0;
static int s_last_rendered_sh = 0;
static int s_last_cursor_x = -1;
static int s_last_cursor_y = -1;
static bool s_last_cursor_hidden = false;
static bool s_cursor_patch_valid = false;
static int s_cursor_patch_x = 0;
static int s_cursor_patch_y = 0;
static uint8_t s_cursor_patch[10][7];
static uint32_t s_last_slow_check_ms = 0;
static int s_last_tb_minute = -1;
static char s_last_lang_name[32];
static uint32_t s_last_title_marquee_tick = 0;
static int s_prev_fg_wid = -1;
static bool s_prev_gfx_mode = false;


static void wm_copy_lang_name(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void list_append(wm_window_t *w) {
    w->prev = wm_tail;
    w->next = NULL;
    if (wm_tail) wm_tail->next = w; else wm_head = w;
    wm_tail = w;
    wm_count++;
    wm_surface_dirty = true;
}

static void list_remove(wm_window_t *w) {
    if (w->prev) w->prev->next = w->next; else wm_head = w->next;
    if (w->next) w->next->prev = w->prev; else wm_tail = w->prev;
    w->prev = w->next = NULL;
    wm_count--;
    wm_surface_dirty = true;
}

static void list_front(wm_window_t *w) {
    if (wm_tail == w) return;
    list_remove(w);
    list_append(w);
}

static uint8_t *wm_get_backbuf(int sw, int sh) {
    if (sw <= 0 || sh <= 0) return NULL;
    if ((size_t)sw > SIZE_MAX / (size_t)sh) return NULL;
    size_t need = (size_t)sw * (size_t)sh;
    if (!wm_backbuf || wm_backbuf_cap < need) {
        if (wm_backbuf) {
            kfree(wm_backbuf);
        }
        wm_backbuf = (uint8_t *)kmalloc(need);
        wm_backbuf_cap = wm_backbuf ? need : 0;
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
    process_on_wm_window_created(cur_proc);

    /* Auto-start WM session if not already running */
    if (!wm_sess_active) {
        wm_session_start();
    } else if (!vga_is_graphics_mode() && process_wm_has_focus()) {
        vga_set_mode_13h_hardware();
        vga_gfx_init_default_palette();
        mouse_set_bounds_from_display();
        wm_invalidate_all();
    }

    int sw = 0, sh = 0;
    wm_get_dims(&sw, &sh);
    int max_ww = wm_max_win_w(sw);
    int max_wh = wm_max_win_h(sh);
    int tb_y = wm_taskbar_y(sh);

    wm_window_t *win = kmalloc(sizeof(wm_window_t));
    if (!win) return NULL;
    memset(win, 0, sizeof(wm_window_t));

    if (opts->fullscreen) {
        win->x=0; win->y=0; win->w=(uint16_t)sw; win->h=(uint16_t)sh; win->fullscreen=true;
    } else {
        win->w = opts->w>0 ? opts->w : 80;
        win->h = opts->h>0 ? opts->h : 50;
        if ((int)win->w > max_ww) win->w = (uint16_t)max_ww;
        if ((int)win->h > max_wh) win->h = (uint16_t)max_wh;

        int16_t req_x = opts->x;
        int16_t req_y = opts->y;
        int16_t min_y = (opts->decor_style != WM_DECOR_NONE) ? (TITLEBAR_H + 2) : 0;
        int16_t max_x = (int16_t)(sw - 2 - win->w);
        int16_t max_y = (int16_t)(tb_y - 2 - win->h);
        if (max_x < 2) max_x = 2;
        if (max_y < min_y) max_y = min_y;

        if (req_x < 2) req_x = 2;
        if (req_x > max_x) req_x = max_x;
        if (req_y < min_y) req_y = min_y;
        if (req_y > max_y) req_y = max_y;

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
    win->user_data    = opts->user_data;
    win->owner_pid    = cur_proc ? cur_proc->pid : 0;
    win->window_id    = cur_proc ? cur_proc->window_id : 0;
    win->event_head   = 0;
    win->event_tail   = 0;
    win->close_requested_ms = 0;

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
    }

    size_t fsz = (size_t)win->w * win->h;
    if (opts->framebuf) {
        win->framebuf = opts->framebuf;
        win->framebuf_owned = false;
    } else {
        win->framebuf = kmalloc(fsz);
        if (!win->framebuf) {
            if (win->shape_mask_owned && win->shape_mask) kfree((void *)win->shape_mask);
            if (win->title) kfree(win->title);
            kfree(win);
            return NULL;
        }
        win->framebuf_owned = true;
        memset(win->framebuf, win->bg_color, fsz);
    }

    list_append(win);
    return win;
}

enum {
    DRAG_NONE = 0,
    DRAG_MOVE,
    DRAG_RESIZE_BR,
    DRAG_RESIZE_R,
    DRAG_RESIZE_B,
    DRAG_RESIZE_BL,
    DRAG_RESIZE_L
};

static wm_window_t *drag_win = NULL;
static int drag_mode = DRAG_NONE;
static wm_window_t *last_title_click_win = NULL;
static uint32_t last_title_click_time = 0;

static void wm_free_window(wm_window_t *win) {
    if (!win) return;
    if (drag_win == win) {
        drag_win = NULL;
        drag_mode = DRAG_NONE;
    }
    if (last_title_click_win == win) {
        last_title_click_win = NULL;
    }
    if (win->framebuf_owned && win->framebuf) kfree(win->framebuf);
    if (win->shape_mask_owned && win->shape_mask) kfree((void *)win->shape_mask);
    if (win->title) kfree(win->title);
    kfree(win);
}

static void wm_push_event(wm_window_t *win, uint32_t type, uint32_t data) {
    if (!win) return;
    uint16_t next = (uint16_t)((win->event_head + 1u) % WM_EVENT_QUEUE_CAP);
    if (next == win->event_tail) {
        win->event_tail = (uint16_t)((win->event_tail + 1u) % WM_EVENT_QUEUE_CAP);
    }
    win->event_queue[win->event_head].type = type;
    win->event_queue[win->event_head].data = data;
    win->event_head = next;
}

void wm_notify_window_event(wm_window_t *win, uint32_t type, uint32_t data) {
    if (!wm_is_window_valid(win)) return;
    wm_push_event(win, type, data);
}

bool wm_poll_event(wm_window_t *win, wm_event_t *out) {
    if (!win || !out || !wm_is_window_valid(win)) return false;
    process_t *cur = process_in_process_context() ? process_get_current() : NULL;
    if (cur && win->owner_pid != 0u && win->owner_pid != cur->pid) {
        return false;
    }
    if (win->event_tail == win->event_head) {
        return false;
    }
    *out = win->event_queue[win->event_tail];
    win->event_tail = (uint16_t)((win->event_tail + 1u) % WM_EVENT_QUEUE_CAP);
    return true;
}

bool wm_is_window_valid(wm_window_t *win) {
    if (!win) return false;
    for (wm_window_t *w = wm_head; w; w = w->next) {
        if (w == win) return true;
    }
    return false;
}

static void wm_handle_window_list_changed(void) {
    if (!wm_sess_active) {
        return;
    }
    if (wm_count == 0 && process_is_batch_active() &&
        process_batch_has_terminal_procs() && !process_get_batch_wm_was_active()) {
        wm_session_stop();
    } else {
        wm_invalidate_all();
    }
}

void wm_destroy_window(wm_window_t *win) {
    if (!win || !wm_is_window_valid(win)) return;
    uint32_t owner_pid = win->owner_pid;
    list_remove(win);
    wm_free_window(win);

    if (owner_pid != 0 && !wm_has_windows_for_pid(owner_pid)) {
        process_t *owner = process_find_by_pid(owner_pid);
        if (owner) {
            if (owner->is_wm_app) {
                owner->wants_graphics = false;
            }
            if (!process_in_process_context()) {
                owner->is_running = 0;
                owner->completed_and_acknowledged = true;
            }
        }
    }

    wm_handle_window_list_changed();
}

void wm_destroy_windows_for_pid(uint32_t pid) {
    if (pid == 0) return;
    wm_window_t *w = wm_head;
    while (w) {
        wm_window_t *next = w->next;
        if (w->owner_pid == pid) {
            list_remove(w);
            wm_free_window(w);
        }
        w = next;
    }
    process_t *owner = process_find_by_pid(pid);
    if (owner && owner->is_wm_app && !wm_has_windows_for_pid(pid)) {
        owner->wants_graphics = false;
    }
    wm_handle_window_list_changed();
}

static void wm_force_close_window(wm_window_t *win) {
    if (!win || !wm_is_window_valid(win)) return;
    uint32_t owner_pid = win->owner_pid;
    wm_destroy_window(win);
    if (owner_pid != 0u && !wm_has_windows_for_pid(owner_pid)) {
        process_t *owner = process_find_by_pid(owner_pid);
        if (owner) {
            owner->is_running = 0;
            owner->completed_and_acknowledged = true;
        }
    }
}

static void wm_close_window_by_user(wm_window_t *win) {
    if (!win || !wm_is_window_valid(win)) return;
    if (win->owner_pid == 0u || !process_is_alive(win->owner_pid) || win->close_requested_ms != 0u) {
        wm_force_close_window(win);
        return;
    }
    uint32_t now = timer_millis();
    win->close_requested_ms = now ? now : 1u;
    wm_push_event(win, WM_EVENT_CLOSE, 0);
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
            wm_push_event(old, WM_EVENT_FOCUS_OUT, 0);
        }
        list_front(win);
        win->dirty = true;
        wm_push_event(win, WM_EVENT_FOCUS_IN, 0);
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

void wm_invalidate(wm_window_t *win) { if (win) { win->dirty = true; wm_surface_dirty = true; } }
void wm_invalidate_all(void)         { wm_surface_dirty = true; for (wm_window_t *w = wm_head; w; w = w->next) w->dirty = true; }

void wm_move(wm_window_t *win, int16_t nx, int16_t ny) {
    if (!win || win->fullscreen) return;
    int sw = 0, sh = 0;
    wm_get_dims(&sw, &sh);
    int tb_y = wm_taskbar_y(sh);
    /* Ensure title bar remains completely accessible and on screen */
    if (ny < TITLEBAR_H + 2) ny = TITLEBAR_H + 2;
    if (ny > (int16_t)(tb_y - 16)) ny = (int16_t)(tb_y - 16);
    if (nx < -(int16_t)win->w + 30) nx = -(int16_t)win->w + 30;
    if (nx > (int16_t)(sw - 30)) nx = (int16_t)(sw - 30);
    win->x = nx;
    win->y = ny;
    if (!win->maximized) {
        win->saved_x = nx;
        win->saved_y = ny;
    }
    win->dirty = true;
    uint32_t pos_data = (((uint32_t)(uint16_t)nx) << 16) | (uint32_t)(uint16_t)ny;
    wm_push_event(win, WM_EVENT_MOVE, pos_data);
}

bool wm_resize(wm_window_t *win, uint16_t nw, uint16_t nh) {
    if (!win || !wm_is_window_valid(win) || win->fullscreen || nw == 0 || nh == 0) return false;
    int sw = 0, sh = 0;
    wm_get_dims(&sw, &sh);
    int max_ww = wm_max_win_w(sw);
    int max_wh = wm_max_win_h(sh);
    if ((int)nw > max_ww) nw = (uint16_t)max_ww;
    if ((int)nh > max_wh) nh = (uint16_t)max_wh;
    if (win->w == nw && win->h == nh) return true;
    if ((size_t)nw > SIZE_MAX / (size_t)nh) return false;

    size_t need = (size_t)nw * (size_t)nh;
    if (win->framebuf_owned) {
        uint8_t *new_fb = (uint8_t *)kmalloc(need);
        if (!new_fb) return false;
        memset(new_fb, win->bg_color, need);
        uint8_t *old_fb = win->framebuf;
        win->framebuf = new_fb;
        win->w = nw;
        win->h = nh;
        if (old_fb) kfree(old_fb);
    } else {
        win->w = nw;
        win->h = nh;
    }
    if (win->shape_mask_owned && win->shape_mask) {
        kfree((void *)win->shape_mask);
        win->shape_mask = NULL;
        win->shape_mask_owned = false;
        win->mask_w = 0;
        win->mask_h = 0;
    }
    win->dirty = true;
    if (!win->maximized) {
        win->saved_w = nw;
        win->saved_h = nh;
    }
    uint32_t dim_data = ((uint32_t)nw << 16) | (uint32_t)nh;
    wm_push_event(win, WM_EVENT_RESIZE, dim_data);
    return true;
}

void wm_minimize(wm_window_t *win) {
    if (!win || !wm_is_window_valid(win)) return;
    win->minimized = true;
    wm_surface_dirty = true;
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
    wm_surface_dirty = true;
    wm_set_focus(win);
}

void wm_toggle_maximize(wm_window_t *win) {
    if (!win || win->fullscreen) return;
    if (win->maximized) {
        win->maximized = false;
        int16_t rx = win->saved_x >= 2 ? win->saved_x : 2;
        int16_t ry = win->saved_y >= (TITLEBAR_H + 2) ? win->saved_y : (TITLEBAR_H + 2);
        uint16_t rw = win->saved_w > 0 ? win->saved_w : 140;
        uint16_t rh = win->saved_h > 0 ? win->saved_h : 100;
        win->x = rx;
        win->y = ry;
        wm_resize(win, rw, rh);
    } else {
        int sw = 0, sh = 0;
        wm_get_dims(&sw, &sh);
        win->saved_x = win->x;
        win->saved_y = win->y;
        win->saved_w = win->w;
        win->saved_h = win->h;
        win->maximized = true;
        win->x = 2;
        win->y = TITLEBAR_H + 2;
        wm_resize(win, (uint16_t)wm_max_win_w(sw), (uint16_t)wm_max_win_h(sh));
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

void wm_set_cursor_visible(wm_window_t *win, bool visible) {
    if (!win || !wm_is_window_valid(win)) return;
    bool hidden = !visible;
    if (win->cursor_hidden == hidden) return;
    win->cursor_hidden = hidden;
}

bool wm_get_cursor_visible(const wm_window_t *win) {
    if (!win) return true;
    for (const wm_window_t *w = wm_head; w; w = w->next) {
        if (w == win) return !w->cursor_hidden;
    }
    return true;
}

void wm_set_fullscreen(wm_window_t *win, bool enable) {
    if (!win || !wm_is_window_valid(win) || win->fullscreen == enable) return;
    if (enable) {
        int sw = 0, sh = 0;
        wm_get_dims(&sw, &sh);
        size_t fsz = (size_t)sw * (size_t)sh;
        uint8_t *new_framebuf = kmalloc(fsz);
        if (!new_framebuf) return;
        memset(new_framebuf, win->bg_color, fsz);
        win->fullscreen_saved_x = win->x;
        win->fullscreen_saved_y = win->y;
        win->fullscreen_saved_w = win->w;
        win->fullscreen_saved_h = win->h;
        uint8_t *old_fb = win->framebuf_owned ? win->framebuf : NULL;
        win->framebuf = new_framebuf;
        win->framebuf_owned = true;
        win->x = 0;
        win->y = 0;
        win->w = (uint16_t)sw;
        win->h = (uint16_t)sh;
        win->minimized = false;
        win->fullscreen = true;
        if (old_fb) kfree(old_fb);
        wm_set_focus(win);
    } else {
        int sw = 0, sh = 0;
        wm_get_dims(&sw, &sh);
        int max_ww = wm_max_win_w(sw);
        int max_wh = wm_max_win_h(sh);
        uint16_t width = win->fullscreen_saved_w ? win->fullscreen_saved_w : 80;
        uint16_t height = win->fullscreen_saved_h ? win->fullscreen_saved_h : 50;
        if ((int)width > max_ww) width = (uint16_t)max_ww;
        if ((int)height > max_wh) height = (uint16_t)max_wh;
        size_t need = (size_t)width * height;
        uint8_t *new_framebuf = kmalloc(need);
        if (!new_framebuf) return;
        memset(new_framebuf, win->bg_color, need);
        uint8_t *old_fb = win->framebuf_owned ? win->framebuf : NULL;
        win->framebuf = new_framebuf;
        win->framebuf_owned = true;
        win->x = win->fullscreen_saved_x;
        win->y = win->fullscreen_saved_y;
        win->w = width;
        win->h = height;
        win->fullscreen = false;
        if (old_fb) kfree(old_fb);
    }
    win->dirty = true;
    uint32_t dim_data = ((uint32_t)win->w << 16) | (uint32_t)win->h;
    wm_push_event(win, WM_EVENT_RESIZE, dim_data);
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

static void draw_3d_box(uint8_t *bb, int bw, int bh, int x, int y, int w, int h, bool sunken) {
    if (w <= 0 || h <= 0) return;
    uint8_t topleft = sunken ? WIN_CLR_DKSHADOW : WIN_CLR_HILIGHT;
    uint8_t botright = sunken ? WIN_CLR_HILIGHT : WIN_CLR_DKSHADOW;
    wm_buf_fill_rect(bb, bw, bh, x, y, w, h, WIN_CLR_FACE);
    wm_buf_draw_line(bb, bw, bh, x, y, x + w - 1, y, topleft);
    wm_buf_draw_line(bb, bw, bh, x, y, x, y + h - 1, topleft);
    wm_buf_draw_line(bb, bw, bh, x, y + h - 1, x + w - 1, y + h - 1, botright);
    wm_buf_draw_line(bb, bw, bh, x + w - 1, y, x + w - 1, y + h - 1, botright);
}

static bool wm_window_has_close_button(const wm_window_t *win) {
    if (!win || win->decor_style != WM_DECOR_DEFAULT) {
        return false;
    }
    return true;
}

/* Mouse cursor (white arrow, black outline) */
static const uint8_t *wm_cursor = (const uint8_t *)"\x80\xC0\xE0\xF0\xF8\xE0\xA0\x00";

static void draw_cursor_outline_px(uint8_t *bb, int bw, int bh, int cx, int cy, int row, int col, int dr, int dc) {
    int nr = row + dr, nc = col + dc;
    bool in = nr >= 0 && nr < 8 && nc >= 0 && nc < 5 && (wm_cursor[nr] & (0x80u >> nc));
    if (!in) {
        int px = cx + col + dc, py = cy + row + dr;
        if (px >= 0 && px < bw && py >= 0 && py < bh) {
            bb[(size_t)py * (size_t)bw + px] = 0;
        }
    }
}

static void draw_cursor(uint8_t *bb, int bw, int bh, int cx, int cy) {
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 5; col++)
            if (wm_cursor[row] & (0x80u >> col)) {
                draw_cursor_outline_px(bb, bw, bh, cx, cy, row, col, -1, 0);
                draw_cursor_outline_px(bb, bw, bh, cx, cy, row, col,  1, 0);
                draw_cursor_outline_px(bb, bw, bh, cx, cy, row, col,  0, -1);
                draw_cursor_outline_px(bb, bw, bh, cx, cy, row, col,  0,  1);
            }
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 5; col++)
            if (wm_cursor[row] & (0x80u >> col)) {
                int px = cx + col, py = cy + row;
                if (px >= 0 && px < bw && py >= 0 && py < bh)
                    bb[(size_t)py * (size_t)bw + px] = 15;
            }
}

/* Window decorations, taskbar, and wallpaper */
/* Titlebar button 6x6 glyph bitmaps inside the 8x8 3D bevel interior */
static const uint8_t s_icon_close[6] = {
    0x00, 0x12, 0x0C, 0x0C, 0x12, 0x00 /* X */
};
static const uint8_t s_icon_max[6] = {
    0x00, 0x08, 0x14, 0x22, 0x22, 0x00 /* ^ */
};
static const uint8_t s_icon_restore[6] = {
    0x00, 0x22, 0x22, 0x14, 0x08, 0x00 /* v */
};
static const uint8_t s_icon_min[6] = {
    0x00, 0x00, 0x00, 0x00, 0x1E, 0x00 /* _ */
};
static const uint8_t s_icon_fullscreen[6] = {
    0x08, 0x08, 0x3E, 0x08, 0x08, 0x00 /* + */
};

static void draw_titlebar_icon_btn(uint8_t *bb, int bw, int bh,
                                   int bx, int by, const uint8_t rows[6],
                                   uint8_t color) {
    draw_3d_box(bb, bw, bh, bx, by, 8, 8, false);
    for (int r = 0; r < 6; r++) {
        uint8_t bits = rows[r];
        for (int c = 0; c < 6; c++) {
            if (bits & (0x20u >> c)) {
                wm_buf_put_pixel(bb, bw, bh, bx + 1 + c, by + 1 + r, color);
            }
        }
    }
}

static void draw_decorations(wm_window_t *win, uint8_t *bb, int bw, int bh) {
    if (win->fullscreen || win->minimized || win->decor_style == WM_DECOR_NONE) return;
    bool focused = (wm_get_focused() == win);

    if (win->decor_style == WM_DECOR_THIN) {
        wm_buf_draw_rect(bb, bw, bh, (int)win->x-1, (int)win->y-1, (int)win->w+2, (int)win->h+2,
                         focused ? WIN_CLR_HILIGHT : WIN_CLR_SHADOW);
        return;
    }

    /* Windows 95 outer 3D beveled frame */
    int ox = (int)win->x - 2;
    int oy = (int)win->y - TITLEBAR_H - 2;
    int ow = (int)win->w + 4;
    int oh = (int)win->h + TITLEBAR_H + 4;

    /* Outer border */
    wm_buf_draw_line(bb, bw, bh, ox, oy, ox + ow - 1, oy, WIN_CLR_HILIGHT);
    wm_buf_draw_line(bb, bw, bh, ox, oy, ox, oy + oh - 1, WIN_CLR_HILIGHT);
    wm_buf_draw_line(bb, bw, bh, ox, oy + oh - 1, ox + ow - 1, oy + oh - 1, WIN_CLR_DKSHADOW);
    wm_buf_draw_line(bb, bw, bh, ox + ow - 1, oy, ox + ow - 1, oy + oh - 1, WIN_CLR_DKSHADOW);

    /* Inner border */
    wm_buf_draw_line(bb, bw, bh, ox + 1, oy + 1, ox + ow - 2, oy + 1, WIN_CLR_FACE);
    wm_buf_draw_line(bb, bw, bh, ox + 1, oy + 1, ox + 1, oy + oh - 2, WIN_CLR_FACE);
    wm_buf_draw_line(bb, bw, bh, ox + 1, oy + oh - 2, ox + ow - 2, oy + oh - 2, WIN_CLR_SHADOW);
    wm_buf_draw_line(bb, bw, bh, ox + ow - 2, oy + 1, ox + ow - 2, oy + oh - 2, WIN_CLR_SHADOW);

    /* Sunken border around client area */
    int cx = (int)win->x - 1;
    int cy = (int)win->y - 1;
    int cw = (int)win->w + 2;
    int ch = (int)win->h + 2;
    wm_buf_draw_line(bb, bw, bh, cx, cy, cx + cw - 1, cy, WIN_CLR_SHADOW);
    wm_buf_draw_line(bb, bw, bh, cx, cy, cx, cy + ch - 1, WIN_CLR_SHADOW);
    wm_buf_draw_line(bb, bw, bh, cx, cy + ch - 1, cx + cw - 1, cy + ch - 1, WIN_CLR_HILIGHT);
    wm_buf_draw_line(bb, bw, bh, cx + cw - 1, cy, cx + cw - 1, cy + ch - 1, WIN_CLR_HILIGHT);

    /* Title bar background */
    uint8_t tbg = focused ? WIN_CLR_ACTIVE_TITLE : WIN_CLR_INACT_TITLE;
    uint8_t tfg = focused ? WIN_CLR_ACTIVE_TEXT : WIN_CLR_INACT_TEXT;
    wm_buf_fill_rect(bb, bw, bh, (int)win->x, (int)win->y - TITLEBAR_H, (int)win->w, TITLEBAR_H, tbg);

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
            wm_buf_draw_string(bb, bw, bh, (int)win->x + 2, (int)win->y - TITLEBAR_H + 2, disp_buf, tfg);
        }
    }

    /* Titlebar Buttons on the right */
    int btn_y = (int)win->y - TITLEBAR_H + 1;

    /* 1. Close button [X] */
    if (wm_window_has_close_button(win)) {
        draw_titlebar_icon_btn(bb, bw, bh, (int)win->x + (int)win->w - 9, btn_y,
                               s_icon_close, WIN_CLR_DKSHADOW);
    }

    /* 2. Maximize [^] / Restore [v] button */
    draw_titlebar_icon_btn(bb, bw, bh, (int)win->x + (int)win->w - 18, btn_y,
                           win->maximized ? s_icon_restore : s_icon_max, WIN_CLR_DKSHADOW);

    /* 3. Minimize button [_] */
    draw_titlebar_icon_btn(bb, bw, bh, (int)win->x + (int)win->w - 27, btn_y,
                           s_icon_min, WIN_CLR_DKSHADOW);

    /* 4. Fullscreen button [+] (cover entire screen over all WM buttons/taskbar) */
    if ((int)win->w >= 40) {
        draw_titlebar_icon_btn(bb, bw, bh, (int)win->x + (int)win->w - 36, btn_y,
                               s_icon_fullscreen, WIN_CLR_DKSHADOW);
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
    static int s_cached_h = -1;
    static int s_cached_m = -1;
    static uint32_t s_last_rtc_ms = 0;

    uint32_t now = timer_millis();
    if (s_cached_h >= 0 && (uint32_t)(now - s_last_rtc_ms) < 1000u) {
        *out_h = s_cached_h;
        *out_m = s_cached_m;
        return;
    }
    s_last_rtc_ms = now;

    int timeout = 256;
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

    s_cached_h = (int)(hr % 24);
    s_cached_m = (int)(min % 60);
    *out_h = s_cached_h;
    *out_m = s_cached_m;
}

static int taskbar_scroll_offset = 0;
static uint32_t wm_lang_offset = 0;
static uint32_t wm_lang_last_tick = 0;

static void wm_draw_close_button(uint8_t *bb, int bw, int bh, int x, int y, int w, int h, bool pressed) {
    bool is_wm = wm_session_active();
    uint8_t clr_face      = is_wm ? WIN_CLR_FACE : 7;
    uint8_t clr_hilight   = is_wm ? WIN_CLR_HILIGHT : 15;
    uint8_t clr_shadow    = is_wm ? WIN_CLR_DKSHADOW : 0;
    uint8_t clr_icon      = is_wm ? WIN_CLR_ACTIVE_TITLE : 1;
    uint8_t clr_text      = is_wm ? WIN_CLR_DKSHADOW : 0;

    uint8_t topleft = pressed ? clr_shadow : clr_hilight;
    uint8_t botright = pressed ? clr_hilight : clr_shadow;

    /* 3D button frame */
    wm_buf_fill_rect(bb, bw, bh, x, y, w, h, clr_face);
    wm_buf_draw_line(bb, bw, bh, x, y, x + w - 1, y, topleft);
    wm_buf_draw_line(bb, bw, bh, x, y, x, y + h - 1, topleft);
    wm_buf_draw_line(bb, bw, bh, x, y + h - 1, x + w - 1, y + h - 1, botright);
    wm_buf_draw_line(bb, bw, bh, x + w - 1, y, x + w - 1, y + h - 1, botright);

    /* Indicator icon (6x6) */
    wm_buf_fill_rect(bb, bw, bh, x + 2, y + 2, 6, 6, clr_icon);

    /* Text: "Close" */
    wm_buf_draw_string(bb, bw, bh, x + 10, y + 2, "Close", clr_text);
}

static void wm_handle_close_button_click(void) {
    if (wm_session_active()) {
        if (process_is_batch_active() && process_get_separate_windows()) {
            for (wm_window_t *w = wm_head; w; ) {
                wm_window_t *next = w->next;
                if (w->owner_pid == 0 && wm_window_is_visible_for_fg(w)) {
                    wm_destroy_window(w);
                }
                w = next;
            }
            if (process_close_current_workspace()) {
                return;
            }
        }
        /* Standalone WM session or shared workspace: exit WM mode and close all WM windows. */
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
            vga_set_mode_text_hardware();
            dynamic_keymap_reapply_fonts();
            mouse_set_bounds_from_display();
        }
    }
}

static void draw_taskbar(uint8_t *bb, int bw, int bh) {
    wm_taskbar_layout_t tb;
    wm_get_taskbar_layout(bw, bh, &tb);

    /* 3D raised taskbar across the bottom */
    wm_buf_fill_rect(bb, bw, bh, 0, tb.y, bw, TASKBAR_H, WIN_CLR_FACE);
    wm_buf_draw_line(bb, bw, bh, 0, tb.y, bw - 1, tb.y, WIN_CLR_HILIGHT);

    /* Close button on the left */
    wm_draw_close_button(bb, bw, bh, tb.close_x, tb.y + 2, tb.close_w, 10, false);

    /* Left scroll button '<' */
    draw_3d_box(bb, bw, bh, tb.scroll_l_x, tb.y + 2, tb.scroll_w, 10, false);
    wm_buf_draw_string(bb, bw, bh, tb.scroll_l_x + 2, tb.y + 3, "<", WIN_CLR_DKSHADOW);

    /* Window tabs between tabs_x0 and tabs_x1 */
    int win_idx = 0;
    int visible_tab = 0;
    int tab_w = tb.tab_w;
    int tab_h = 10;
    for (wm_window_t *w = wm_head; w; w = w->next) {
        if (!wm_window_is_visible_for_fg(w)) continue;
        if (win_idx < taskbar_scroll_offset) { win_idx++; continue; }
        win_idx++;
        if (visible_tab >= tb.max_tabs) break;

        int tx = tb.tabs_x0 + visible_tab * tb.tab_stride;
        int ty = tb.y + 2;
        bool is_active = (w == wm_get_focused() && !w->minimized);

        if (is_active) {
            draw_3d_box(bb, bw, bh, tx, ty, tab_w, tab_h, true);
            wm_buf_fill_rect(bb, bw, bh, tx + 1, ty + 1, tab_w - 2, tab_h - 2, WIN_CLR_ACTIVE_TITLE);
        } else {
            draw_3d_box(bb, bw, bh, tx, ty, tab_w, tab_h, false);
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
        wm_buf_draw_string(bb, bw, bh, tx + 2, ty + 3, title_buf, text_color);

        /* Dedicated [x] close button on this specific tab */
        if (wm_window_has_close_button(w)) {
            int close_btn_x = tx + tab_w - 9;
            int close_btn_y = ty + 1;
            draw_3d_box(bb, bw, bh, close_btn_x, close_btn_y, 8, 8, false);
            wm_buf_draw_string(bb, bw, bh, close_btn_x + 1, close_btn_y, "x", is_active ? 15 : WIN_CLR_DKSHADOW);
        }

        visible_tab++;
    }

    /* Right scroll button '>' */
    draw_3d_box(bb, bw, bh, tb.scroll_r_x, tb.y + 2, tb.scroll_w, 10, false);
    wm_buf_draw_string(bb, bw, bh, tb.scroll_r_x + 2, tb.y + 3, ">", WIN_CLR_DKSHADOW);

    /* Language box in tray */
    draw_3d_box(bb, bw, bh, tb.lang_x, tb.y + 2, tb.lang_w, 10, true);
    const char *lang_name = dynamic_keymap_get_name();
    if (!lang_name || lang_name[0] == '\0') {
        lang_name = default_keymap_name;
    }
    size_t lang_byte_len = strlen(lang_name);
    int vis_chars = 13; /* (85 - 4) / 6 = 13 character cells */

    /* Count codepoints in lang_name only when byte length exceeds vis_chars */
    size_t lang_cp_count = lang_byte_len;
    if (lang_byte_len > (size_t)vis_chars) {
        lang_cp_count = 0;
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
        wm_buf_draw_string(bb, bw, bh, tb.lang_x + 3, tb.y + 3, disp_buf, WIN_CLR_DKSHADOW);
        if (disp_buf != lang_stack) kfree(disp_buf);
    }

    /* System tray clock from hardware CMOS RTC */
    draw_3d_box(bb, bw, bh, tb.clock_x, tb.y + 2, tb.clock_w, 10, true);
    int rtc_h = 0, rtc_m = 0;
    wm_get_rtc_time(&rtc_h, &rtc_m);
    char clock_buf[6];
    clock_buf[0] = (char)('0' + (rtc_h / 10));
    clock_buf[1] = (char)('0' + (rtc_h % 10));
    clock_buf[2] = ':';
    clock_buf[3] = (char)('0' + (rtc_m / 10));
    clock_buf[4] = (char)('0' + (rtc_m % 10));
    clock_buf[5] = '\0';
    wm_buf_draw_string(bb, bw, bh, tb.clock_x + 3, tb.y + 3, clock_buf, WIN_CLR_DKSHADOW);
}

static void wm_compose_dims(uint8_t *backbuf, int bw, int bh) {
    wm_window_t *fw = wm_get_focused();
    bool top_fullscreen = (fw && fw->fullscreen && !fw->minimized && wm_window_is_visible_for_fg(fw));
    int max_y = top_fullscreen ? bh : wm_taskbar_y(bh);

    bool opaque_full_cover = (top_fullscreen && fw->framebuf && !fw->shape_mask &&
                              fw->x <= 0 && fw->y <= 0 &&
                              (int)fw->w >= bw && (int)fw->h >= bh);
    if (!opaque_full_cover) {
        /* 1. Windows 95 Teal desktop */
        memset(backbuf, WIN_CLR_DESKTOP, (size_t)bw * (size_t)bh);
    }

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
        if (win_x + col_end > bw) col_end = bw - win_x;

        int visible_width = col_end - col_start;

        if (visible_width > 0 && row_start < row_end) {
            bool has_mask = (win->shape_mask && win->mask_w == win->w && win->mask_h == win->h);
            if (!has_mask) {
                if (visible_width == bw && win_w == bw && col_start == 0 && win_x == 0) {
                    memcpy(&backbuf[(size_t)(win_y + row_start) * (size_t)bw],
                           &win->framebuf[(size_t)row_start * (size_t)win_w],
                           (size_t)(row_end - row_start) * (size_t)bw);
                } else {
                    for (int row = row_start; row < row_end; row++) {
                        int sy = win_y + row;
                        uint8_t *dst = &backbuf[(size_t)sy * (size_t)bw + (win_x + col_start)];
                        const uint8_t *src = &win->framebuf[(size_t)row * win_w + col_start];
                        memcpy(dst, src, (size_t)visible_width);
                    }
                }
            } else {
                for (int row = row_start; row < row_end; row++) {
                    int sy = win_y + row;
                    size_t dst_base = (size_t)sy * (size_t)bw + win_x;
                    size_t src_base = (size_t)row * win_w;
                    for (int col = col_start; col < col_end; col++) {
                        if (win->shape_mask[src_base + col]) {
                            backbuf[dst_base + col] = win->framebuf[src_base + col];
                        }
                    }
                }
            }
        }
        draw_decorations(win, backbuf, bw, bh);
    }

    /* 4. Taskbar at bottom (hidden if focused window is fullscreen) */
    if (!top_fullscreen) {
        draw_taskbar(backbuf, bw, bh);
    }
}

static void wm_compose(uint8_t *backbuf) {
    int sw = 0, sh = 0;
    wm_get_dims(&sw, &sh);
    wm_compose_dims(backbuf, sw, sh);
}

static bool wm_window_hides_cursor(const wm_window_t *win) {
    return win ? win->cursor_hidden : false;
}

static bool wm_is_cursor_hidden_at(int cx, int cy) {
    if (drag_mode != DRAG_NONE) return false;
    wm_window_t *fw = wm_get_focused();
    if (!fw || fw->minimized || !wm_window_hides_cursor(fw)) return false;
    return (cx >= (int)fw->x && cx < (int)fw->x + (int)fw->w &&
            cy >= (int)fw->y && cy < (int)fw->y + (int)fw->h);
}

static void wm_cursor_patch_restore(uint8_t *bb, int sw, int sh) {
    if (!s_cursor_patch_valid) return;
    for (int r = 0; r < 10; r++) {
        int py = s_cursor_patch_y - 1 + r;
        if (py < 0 || py >= sh) continue;
        for (int c = 0; c < 7; c++) {
            int px = s_cursor_patch_x - 1 + c;
            if (px < 0 || px >= sw) continue;
            bb[(size_t)py * (size_t)sw + (size_t)px] = s_cursor_patch[r][c];
        }
    }
    s_cursor_patch_valid = false;
}

static void wm_cursor_patch_save(const uint8_t *bb, int sw, int sh, int cx, int cy) {
    s_cursor_patch_x = cx;
    s_cursor_patch_y = cy;
    for (int r = 0; r < 10; r++) {
        int py = cy - 1 + r;
        if (py < 0 || py >= sh) continue;
        for (int c = 0; c < 7; c++) {
            int px = cx - 1 + c;
            if (px < 0 || px >= sw) continue;
            s_cursor_patch[r][c] = bb[(size_t)py * (size_t)sw + (size_t)px];
        }
    }
    s_cursor_patch_valid = true;
}

static void wm_render_frame(int sw, int sh, int cursor_x, int cursor_y, bool blit_hw) {
    uint8_t *bb = wm_get_backbuf(sw, sh);
    if (!bb) return;

    bool hide_cursor = wm_is_cursor_hidden_at(cursor_x, cursor_y);
    if (sw != s_last_rendered_sw || sh != s_last_rendered_sh) {
        wm_surface_dirty = true;
    }

    if (wm_surface_dirty) {
        s_cursor_patch_valid = false;
        wm_compose_dims(bb, sw, sh);
        wm_surface_dirty = false;
        s_last_rendered_sw = sw;
        s_last_rendered_sh = sh;
    } else {
        if (cursor_x == s_last_cursor_x &&
            cursor_y == s_last_cursor_y &&
            hide_cursor == s_last_cursor_hidden) {
            return;
        }
        wm_cursor_patch_restore(bb, sw, sh);
    }

    if (!hide_cursor) {
        wm_cursor_patch_save(bb, sw, sh, cursor_x, cursor_y);
        draw_cursor(bb, sw, sh, cursor_x, cursor_y);
    }

    s_last_cursor_x = cursor_x;
    s_last_cursor_y = cursor_y;
    s_last_cursor_hidden = hide_cursor;

    if (blit_hw) {
        vga_gfx_blit_hardware(bb, true, sw, sh);
    }
}

/* Compositor tick and mouse interactions */
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
    uint16_t restore_w = win->saved_w > 0 ? win->saved_w : 140;
    uint16_t restore_h = win->saved_h > 0 ? win->saved_h : 100;
    drag_offset_x = (int16_t)(local_x * restore_w / win->w);
    drag_offset_y = (int16_t)(local_y * restore_h / win->h);
    win->x = win->saved_x >= 2 ? win->saved_x : 2;
    win->y = win->saved_y >= TITLEBAR_H + 2 ? win->saved_y : TITLEBAR_H + 2;
    win->maximized = false;
    wm_resize(win, restore_w, restore_h);
}

static void wm_clamp_left_resize(int dx, int max_ww, int *out_nx, int *out_nw) {
    int nw = (int)drag_start_w - dx;
    int nx = (int)drag_start_x + dx;
    if (nw < 1) {
        nx = (int)drag_start_x + (int)drag_start_w - 1;
        nw = 1;
    }
    if (nw > max_ww) {
        nx = (int)drag_start_x + (int)drag_start_w - max_ww;
        nw = max_ww;
    }
    if (nx < 2) {
        nw -= (2 - nx);
        nx = 2;
        if (nw < 1) nw = 1;
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
    if (!process_wm_has_focus()) {
        wm_cancel_mouse_interaction();
        return;
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
                if (process_is_batch_active()) {
                    if (make == 0x49u) {
                        process_switch_foreground_prev();
                    } else {
                        process_switch_foreground_next();
                    }
                    process_t *nfg = process_get_foreground();
                    if (process_wm_has_focus() && nfg && (nfg->is_wm_app || !process_get_separate_windows())) {
                        wm_focus_window_for_pid(nfg->pid);
                        wm_invalidate_all();
                    }
                }
                return;
            } else if (keyboard_is_ctrl_pressed() &&
                       (make == 0x48u || make == 0x50u || make == 0x4Bu || make == 0x4Du)) {
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
            uint32_t ext_data = (kev.keycode & 0x7FFFFFFFu) | (is_break ? 0x80000000u : 0u);
            wm_push_event(fw, WM_EVENT_KEY_EXT, ext_data);
            for (uint8_t bi = 0; bi < kev.raw_len; bi++) {
                uint8_t b = kev.raw_bytes[bi];
                uint32_t ev_t = (b == 0xE0u || b == 0xE1u) ? WM_EVENT_KEY_DOWN
                                : ((b & 0x80u) ? WM_EVENT_KEY_UP : WM_EVENT_KEY_DOWN);
                wm_push_event(fw, ev_t, (uint32_t)b);
            }
        }
    }
    if (!wm_sess_active) return;
    if (!process_wm_has_focus()) {
        wm_cancel_mouse_interaction();
        return;
    }

    /* Mouse */
    mouse_poll();
    if (!wm_sess_active) return;
    if (!process_wm_has_focus()) {
        wm_cancel_mouse_interaction();
        return;
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

    int sw = 0, sh = 0;
    wm_get_dims(&sw, &sh);
    wm_taskbar_layout_t tb;
    wm_get_taskbar_layout(sw, sh, &tb);

    wm_window_t *top_win = wm_get_focused();
    bool top_fullscreen = (top_win && top_win->fullscreen && !top_win->minimized);

    if (right_pressed) {
        if (ms.y >= tb.y && !top_fullscreen) {
            if (ms.x >= tb.tabs_x0 && ms.x <= tb.tabs_x1) {
                int rel_x = (int)ms.x - tb.tabs_x0;
                int tab_idx = rel_x / tb.tab_stride;
                if (tab_idx < tb.max_tabs && (rel_x % tb.tab_stride) <= tb.tab_w) {
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
            else if (ms.x >= tb.lang_x && ms.x < tb.lang_x + tb.lang_w) {
                dynamic_keymap_cycle_prev();
            }
        }
    }
    if (left_pressed) {
        bool handled = false;
        drag_win = NULL;
        drag_mode = DRAG_NONE;

        /* 1. Check Taskbar */
        if (ms.y >= tb.y && !top_fullscreen) {
            /* Close button */
            if (ms.x >= tb.close_x && ms.x <= tb.close_x + tb.close_w) {
                wm_handle_close_button_click();
                if (!wm_sess_active || !process_wm_has_focus() || !vga_is_graphics_mode()) {
                    return;
                }
                handled = true;
            }
            /* Scroll left '<' */
            if (!handled && ms.x >= tb.scroll_l_x && ms.x <= tb.scroll_l_x + tb.scroll_w) {
                if (taskbar_scroll_offset > 0) taskbar_scroll_offset--;
                handled = true;
            }
            /* Scroll right '>' */
            if (!handled && ms.x >= tb.scroll_r_x && ms.x <= tb.scroll_r_x + tb.scroll_w) {
                if (taskbar_scroll_offset + tb.max_tabs < wm_count_visible_windows()) taskbar_scroll_offset++;
                handled = true;
            }
            /* Window tabs */
            if (!handled && ms.x >= tb.tabs_x0 && ms.x <= tb.tabs_x1) {
                int rel_x = (int)ms.x - tb.tabs_x0;
                int tab_idx = rel_x / tb.tab_stride;
                if (tab_idx < tb.max_tabs && (rel_x % tb.tab_stride) <= tb.tab_w) {
                    int target_idx = taskbar_scroll_offset + tab_idx;
                    int tab_in_x = rel_x % tb.tab_stride;
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
            if (!handled && ms.x >= tb.lang_x && ms.x < tb.lang_x + tb.lang_w) {
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

                            /* Maximize [^] / Restore [v] button */
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

                        /* Outer resize hit-testing stays outside client pixels so interior clicks
                         * (e.g. hidden-cursor / custom-cursor apps) never start a resize drag. */
                        bool on_outer_frame =
                            (ms.x < (int)w->x || ms.x >= (int)w->x + (int)w->w ||
                             ms.y >= (int)w->y + (int)w->h);

                        /* Bottom-right corner resize: strictly on outer corner frame */
                        if (on_outer_frame &&
                            ms.x >= (int)w->x + (int)w->w - 4 && ms.x <= (int)w->x + (int)w->w + 3 &&
                            ms.y >= (int)w->y + (int)w->h - 4 && ms.y <= (int)w->y + (int)w->h + 3) {
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

                        /* Bottom-left corner resize: strictly on outer corner frame */
                        if (on_outer_frame &&
                            ms.x >= (int)w->x - 3 && ms.x <= (int)w->x + 4 &&
                            ms.y >= (int)w->y + (int)w->h - 4 && ms.y <= (int)w->y + (int)w->h + 3) {
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

                        /* Left border resize: outer left edge frame */
                        if (ms.x >= (int)w->x - 3 && ms.x < (int)w->x &&
                            ms.y >= (int)w->y && ms.y <= (int)w->y + (int)w->h + 2) {
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

                        /* Right border resize: outer right edge frame */
                        if (ms.x >= (int)w->x + (int)w->w && ms.x <= (int)w->x + (int)w->w + 3 &&
                            ms.y >= (int)w->y && ms.y <= (int)w->y + (int)w->h + 2) {
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

                        /* Bottom border resize: outer bottom edge frame */
                        if (ms.y >= (int)w->y + (int)w->h && ms.y <= (int)w->y + (int)w->h + 3 &&
                            ms.x >= (int)w->x && ms.x <= (int)w->x + (int)w->w + 2) {
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
                        if (w->owner_pid == 0 || process_is_alive(w->owner_pid)) {
                            uint32_t click_data =
                                (((uint32_t)(ms.x - w->x) & 0xFFFF) << 16) |
                                ((uint32_t)(ms.y - w->y) & 0xFFFF);
                            wm_push_event(w, WM_EVENT_CLICK, click_data);
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
                if (ny > (int16_t)(tb.y - 16)) ny = (int16_t)(tb.y - 16);
                if (nx < -(int16_t)drag_win->w + 30) nx = -(int16_t)drag_win->w + 30;
                if (nx > (int16_t)(sw - 30)) nx = (int16_t)(sw - 30);
                wm_move(drag_win, nx, ny);
            }
        } else if (drag_mode == DRAG_RESIZE_BR) {
            int max_ww = wm_max_win_w(sw);
            int max_wh = wm_max_win_h(sh);
            int nw = (int)drag_start_w + ((int)ms.x - (int)drag_start_mx);
            int nh = (int)drag_start_h + ((int)ms.y - (int)drag_start_my);
            if (nw < 1) nw = 1;
            if (nw > max_ww) nw = max_ww;
            if (nh < 1) nh = 1;
            if (nh > max_wh) nh = max_wh;
            drag_win->maximized = false;
            wm_resize(drag_win, (uint16_t)nw, (uint16_t)nh);
        } else if (drag_mode == DRAG_RESIZE_R) {
            int max_ww = wm_max_win_w(sw);
            int nw = (int)drag_start_w + ((int)ms.x - (int)drag_start_mx);
            if (nw < 1) nw = 1;
            if (nw > max_ww) nw = max_ww;
            drag_win->maximized = false;
            wm_resize(drag_win, (uint16_t)nw, drag_win->h);
        } else if (drag_mode == DRAG_RESIZE_B) {
            int max_wh = wm_max_win_h(sh);
            int nh = (int)drag_start_h + ((int)ms.y - (int)drag_start_my);
            if (nh < 1) nh = 1;
            if (nh > max_wh) nh = max_wh;
            drag_win->maximized = false;
            wm_resize(drag_win, drag_win->w, (uint16_t)nh);
        } else if (drag_mode == DRAG_RESIZE_L) {
            int nx, nw;
            wm_clamp_left_resize((int)ms.x - (int)drag_start_mx, wm_max_win_w(sw), &nx, &nw);
            drag_win->maximized = false;
            drag_win->x = (int16_t)nx;
            wm_resize(drag_win, (uint16_t)nw, drag_win->h);
        } else if (drag_mode == DRAG_RESIZE_BL) {
            int max_wh = wm_max_win_h(sh);
            int nx, nw;
            wm_clamp_left_resize((int)ms.x - (int)drag_start_mx, wm_max_win_w(sw), &nx, &nw);
            int nh = (int)drag_start_h + ((int)ms.y - (int)drag_start_my);
            if (nh < 1) nh = 1;
            if (nh > max_wh) nh = max_wh;
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

    if (!wm_sess_active || !process_wm_has_focus() || !vga_is_graphics_mode()) {
        s_prev_gfx_mode = false;
        return;
    }
    if (!s_prev_gfx_mode) {
        wm_surface_dirty = true;
        s_prev_gfx_mode = true;
    }

    if (process_is_batch_active() && process_get_separate_windows()) {
        process_t *disp_fg = process_get_displayed_foreground();
        int fg_wid = disp_fg ? disp_fg->window_id : -1;
        if (fg_wid != s_prev_fg_wid) {
            s_prev_fg_wid = fg_wid;
            wm_surface_dirty = true;
        }
    }

    for (wm_window_t *w = wm_head; w; ) {
        wm_window_t *next = w->next;
        if (w->dirty) {
            w->dirty = false;
            if (wm_window_is_visible_for_fg(w) && !w->minimized) {
                wm_surface_dirty = true;
            }
        }
        if (w->close_requested_ms != 0u && timer_elapsed_ms(w->close_requested_ms) >= 500u) {
            wm_force_close_window(w);
        }
        w = next;
    }

    if (!wm_surface_dirty) {
        uint32_t now = timer_millis();
        if ((uint32_t)(now - s_last_slow_check_ms) >= 180u) {
            s_last_slow_check_ms = now;
            wm_window_t *fw = wm_get_focused();
            bool top_fs = (fw && fw->fullscreen && !fw->minimized && wm_window_is_visible_for_fg(fw));
            if (!top_fs) {
                int rtc_h = 0, rtc_m = 0;
                wm_get_rtc_time(&rtc_h, &rtc_m);
                int cur_min = rtc_h * 60 + rtc_m;
                if (cur_min != s_last_tb_minute) {
                    s_last_tb_minute = cur_min;
                    wm_surface_dirty = true;
                }
                const char *cur_lang = dynamic_keymap_get_name();
                if (!cur_lang || !cur_lang[0]) cur_lang = default_keymap_name;
                if (strncmp(cur_lang, s_last_lang_name, sizeof(s_last_lang_name) - 1u) != 0) {
                    wm_copy_lang_name(s_last_lang_name, sizeof(s_last_lang_name), cur_lang);
                    wm_surface_dirty = true;
                }
                if (strlen(cur_lang) > 13u) {
                    wm_surface_dirty = true;
                } else {
                    uint32_t mtick = now / 200u;
                    if (mtick != s_last_title_marquee_tick) {
                        for (wm_window_t *w = wm_head; w; w = w->next) {
                            if (!wm_window_is_visible_for_fg(w) || w->minimized || w->fullscreen || !w->title) continue;
                            if (w->decor_style != WM_DECOR_DEFAULT) continue;
                            int max_chars = ((int)w->w - 40) / 6;
                            if (max_chars > 0 && (int)strlen(w->title) > max_chars) {
                                s_last_title_marquee_tick = mtick;
                                wm_surface_dirty = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    static int s_prev_taskbar_scroll = 0;
    if (taskbar_scroll_offset != s_prev_taskbar_scroll) {
        s_prev_taskbar_scroll = taskbar_scroll_offset;
        wm_surface_dirty = true;
    }

    wm_render_frame(sw, sh, (int)ms.x, (int)ms.y, true);
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

    int screen_w = 0, screen_h = 0;
    wm_get_dims(&screen_w, &screen_h);
    wm_sess_active = true;
    bool should_switch_hw = process_wm_has_focus() ||
                            (cur_proc && fg && fg->window_id == cur_proc->window_id);
    if (should_switch_hw) {
        if (!vga_is_graphics_mode()) {
            vga_set_mode_13h_hardware();
        }
        vga_gfx_init_default_palette();
        mouse_set_bounds(screen_w, screen_h);
        keyboard_flush_queue();
        keyboard_flush_hardware();
    }

    wm_prev_left   = false;
    drag_mode      = DRAG_NONE;
    drag_win       = NULL;
    taskbar_scroll_offset = 0;
    wm_surface_dirty = true;
    s_cursor_patch_valid = false;
    s_last_rendered_sw = 0;
    s_last_rendered_sh = 0;
    s_prev_gfx_mode = should_switch_hw;

    /* Draw initial frame */
    wm_render_frame(screen_w, screen_h, screen_w / 2, screen_h / 2, should_switch_hw);
}

/** wm_session_stop — stop compositor, destroy all windows, restore text mode. */
void wm_session_stop(void) {
    if (!wm_sess_active) return;
    wm_sess_active = false;
    drag_mode      = DRAG_NONE;
    drag_win       = NULL;
    taskbar_scroll_offset = 0;
    wm_surface_dirty = true;
    s_cursor_patch_valid = false;
    s_last_rendered_sw = 0;
    s_last_rendered_sh = 0;
    s_prev_gfx_mode = false;
    while (wm_head) wm_destroy_window(wm_head);
    if (wm_backbuf) {
        kfree(wm_backbuf);
        wm_backbuf = NULL;
        wm_backbuf_cap = 0;
    }
    keyboard_flush_queue();
    if (vga_is_graphics_mode()) {
        vga_set_mode_text_hardware();
        dynamic_keymap_reapply_fonts();
    }
    mouse_set_bounds_from_display();
    process_on_wm_windows_empty();
}

void wm_on_resolution_changed(void) {
    if (!wm_sess_active) return;
    int sw = 0, sh = 0;
    wm_get_dims(&sw, &sh);
    mouse_set_bounds(sw, sh);
    int max_ww = wm_max_win_w(sw);
    int max_wh = wm_max_win_h(sh);
    int tb_y = wm_taskbar_y(sh);
    for (wm_window_t *w = wm_head; w; w = w->next) {
        if (w->fullscreen) {
            int16_t orig_x = w->fullscreen_saved_x;
            int16_t orig_y = w->fullscreen_saved_y;
            uint16_t orig_w = w->fullscreen_saved_w;
            uint16_t orig_h = w->fullscreen_saved_h;
            wm_set_fullscreen(w, false);
            wm_set_fullscreen(w, true);
            w->fullscreen_saved_x = orig_x;
            w->fullscreen_saved_y = orig_y;
            w->fullscreen_saved_w = orig_w;
            w->fullscreen_saved_h = orig_h;
        } else if (w->maximized) {
            w->x = 2;
            w->y = TITLEBAR_H + 2;
            wm_resize(w, (uint16_t)max_ww, (uint16_t)max_wh);
        } else {
            if ((int)w->w > max_ww || (int)w->h > max_wh) {
                uint16_t nw = (int)w->w > max_ww ? (uint16_t)max_ww : w->w;
                uint16_t nh = (int)w->h > max_wh ? (uint16_t)max_wh : w->h;
                wm_resize(w, nw, nh);
            }
            int16_t max_x = (int16_t)(sw - 30);
            int16_t max_y = (int16_t)(tb_y - 16);
            int16_t nx = w->x > max_x ? max_x : w->x;
            int16_t ny = w->y > max_y ? max_y : w->y;
            if (nx != w->x || ny != w->y) {
                wm_move(w, nx, ny);
            }
        }
    }
    wm_invalidate_all();
    if (vga_is_graphics_mode()) {
        mouse_state_t ms;
        mouse_get_state(&ms);
        wm_render_frame(sw, sh, (int)ms.x, (int)ms.y, true);
    }
}

void wm_terminal_putc(char c, uint8_t fg, uint8_t bg) {
    /* Terminal apps render natively in VGA text mode behind WM, not in a WM Terminal window. */
    (void)c;
    (void)fg;
    (void)bg;
}
