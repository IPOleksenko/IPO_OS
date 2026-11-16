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
#include <system/rtc.h>
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
#define WM_SMALL_FONT_W 4
#define WM_SMALL_FONT_H 5
#define WM_SMALL_FONT_ADV 5
#define TITLEBAR_H 11
#define TASKBAR_H  20

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

static char *s_tb_time_text = NULL;
static size_t s_tb_time_len = 0u;
static char *s_tb_date_text = NULL;
static size_t s_tb_date_len = 0u;
static uint32_t s_time_marquee_off = 0u;
static uint32_t s_date_marquee_off = 0u;
static uint32_t s_clock_marquee_last_tick = 0u;
static bool s_tb_time_overflow = false;
static bool s_tb_date_overflow = false;
static bool s_tb_tabs_overflow = false;

#define WM_CLOCK_BOX_W 60

static int wm_calc_taskbar_clock_w(int sw) {
    int cw = WM_CLOCK_BOX_W;
    int max_cw = sw - 160;
    if (max_cw >= 40 && cw > max_cw) cw = max_cw;
    return cw;
}

static void wm_get_taskbar_layout(int sw, int sh, wm_taskbar_layout_t *out) {
    out->y = wm_taskbar_y(sh);
    out->close_x = 2;
    out->close_w = 40;
    out->scroll_l_x = 44;
    out->scroll_w = 9;
    out->clock_w = wm_calc_taskbar_clock_w(sw);
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
static uint8_t *s_cursor_patch = NULL;
static size_t s_cursor_patch_cap = 0;
static uint32_t s_last_slow_check_ms = 0;
static uint32_t s_last_clock_check_ms = 0;
static uint32_t s_last_clock_gen = 0;
static char *s_last_lang_name = NULL;
static size_t s_last_lang_cap = 0;
static uint32_t s_last_title_marquee_tick = 0;
static int s_prev_fg_wid = -1;
static bool s_prev_gfx_mode = false;


static void wm_set_lang_name(const char *src) {
    if (!src) src = "";
    size_t len = strlen(src);
    if (!s_last_lang_name || len + 1u > s_last_lang_cap) {
        size_t ncap = (s_last_lang_cap > 0u) ? s_last_lang_cap : 32u;
        while (ncap < len + 1u) {
            if (ncap > SIZE_MAX / 2u) { ncap = len + 1u; break; }
            ncap *= 2u;
        }
        char *nb = (char *)krealloc(s_last_lang_name, ncap);
        if (!nb) return;
        s_last_lang_name = nb;
        s_last_lang_cap = ncap;
    }
    memcpy(s_last_lang_name, src, len + 1u);
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

    /* Copy shape mask to kernel heap so it outlives the caller's address space. */
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
    if (win->event_queue) kfree(win->event_queue);
    kfree(win);
}

static void wm_push_event(wm_window_t *win, uint32_t type, uint32_t data) {
    if (!win) return;
    if (!win->event_queue || win->event_cap == 0u) {
        uint16_t init_cap = 16u;
        win->event_queue = (wm_event_t *)kmalloc((size_t)init_cap * sizeof(wm_event_t));
        if (!win->event_queue) return;
        win->event_cap = init_cap;
        win->event_head = 0u;
        win->event_tail = 0u;
    }
    uint16_t next = (uint16_t)((win->event_head + 1u) % win->event_cap);
    if (next == win->event_tail) {
        uint16_t old_cap = win->event_cap;
        uint16_t new_cap = (old_cap <= 0x7FFFu) ? (uint16_t)(old_cap * 2u) : 0xFFFFu;
        bool grew = false;
        if (new_cap > old_cap) {
            wm_event_t *new_q = (wm_event_t *)kmalloc((size_t)new_cap * sizeof(wm_event_t));
            if (new_q) {
                uint16_t count = (uint16_t)((win->event_head - win->event_tail + old_cap) % old_cap);
                for (uint16_t i = 0u; i < count; i++) {
                    new_q[i] = win->event_queue[(win->event_tail + i) % old_cap];
                }
                kfree(win->event_queue);
                win->event_queue = new_q;
                win->event_cap = new_cap;
                win->event_tail = 0u;
                win->event_head = count;
                next = (uint16_t)((win->event_head + 1u) % win->event_cap);
                grew = true;
            }
        }
        if (!grew) {
            win->event_tail = (uint16_t)((win->event_tail + 1u) % win->event_cap);
        }
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
    if (!win->event_queue || win->event_cap == 0u || win->event_tail == win->event_head) {
        return false;
    }
    *out = win->event_queue[win->event_tail];
    win->event_tail = (uint16_t)((win->event_tail + 1u) % win->event_cap);
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
    /* Keep titlebar reachable on screen */
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

/* Classic desktop UI palette indices and 3D primitives */

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

/* Titlebar button 6x6 glyph bitmaps packed into scalar bitmasks */
#define WM_PACK_ICON6(r0, r1, r2, r3, r4, r5) \
    ((uint64_t)(r0) | ((uint64_t)(r1) << 8) | ((uint64_t)(r2) << 16) | \
     ((uint64_t)(r3) << 24) | ((uint64_t)(r4) << 32) | ((uint64_t)(r5) << 40))

#define WM_ICON_CLOSE      WM_PACK_ICON6(0x00u, 0x12u, 0x0Cu, 0x0Cu, 0x12u, 0x00u)
#define WM_ICON_MAX        WM_PACK_ICON6(0x00u, 0x08u, 0x14u, 0x22u, 0x22u, 0x00u)
#define WM_ICON_RESTORE    WM_PACK_ICON6(0x00u, 0x22u, 0x22u, 0x14u, 0x08u, 0x00u)
#define WM_ICON_MIN        WM_PACK_ICON6(0x00u, 0x00u, 0x00u, 0x00u, 0x1Eu, 0x00u)
#define WM_ICON_FULLSCREEN WM_PACK_ICON6(0x08u, 0x08u, 0x3Eu, 0x08u, 0x08u, 0x00u)

static void draw_titlebar_icon_btn(uint8_t *bb, int bw, int bh,
                                   int bx, int by, uint64_t icon_bits,
                                   uint8_t color) {
    draw_3d_box(bb, bw, bh, bx, by, 8, 8, false);
    for (int r = 0; r < 6; r++) {
        uint8_t bits = (uint8_t)((icon_bits >> (r * 8)) & 0xFFu);
        for (int c = 0; c < 6; c++) {
            if (bits & (0x20u >> c)) {
                wm_buf_put_pixel(bb, bw, bh, bx + 1 + c, by + 1 + r, color);
            }
        }
    }
}

static char *wm_dup_str(const char *s, size_t *out_len) {
    size_t len = s ? strlen(s) : 0u;
    char *copy = (char *)kmalloc(len + 1u);
    if (!copy) return NULL;
    if (len > 0u) memcpy(copy, s, len);
    copy[len] = '\0';
    if (out_len) *out_len = len;
    return copy;
}

static size_t wm_utf8_char_count(const char *src) {
    if (!src) return 0;
    size_t src_byte_len = strlen(src);
    size_t cp_count = 0;
    size_t pos = 0;
    while (pos < src_byte_len) {
        size_t cb = 0;
        utf8_to_vga_glyph(&src[pos], src_byte_len - pos, &cb);
        if (cb == 0) { pos++; continue; }
        pos += cb;
        cp_count++;
    }
    return cp_count;
}

static char *wm_make_marquee_alloc(const char *src, int vis_chars, uint32_t offset) {
    if (!src || vis_chars <= 0) return NULL;
    size_t slen = strlen(src);
    if (slen <= (size_t)vis_chars) {
        return wm_dup_str(src, NULL);
    }
    const char *sep = "       ";
    size_t seplen = strlen(sep);
    size_t total = slen + seplen;
    char *out = (char *)kmalloc((size_t)vis_chars + 1u);
    if (!out) return NULL;
    for (int i = 0; i < vis_chars; i++) {
        size_t idx = ((size_t)offset + (size_t)i) % total;
        out[i] = (idx < slen) ? src[idx] : sep[idx - slen];
    }
    out[vis_chars] = '\0';
    return out;
}

static char *wm_make_marquee_utf8_alloc(const char *src, int vis_chars, uint32_t offset) {
    if (!src || vis_chars <= 0) return NULL;
    size_t src_byte_len = strlen(src);
    if (src_byte_len == 0) return wm_dup_str("", NULL);

    size_t cp_count = wm_utf8_char_count(src);
    if ((int)cp_count <= vis_chars) {
        return wm_dup_str(src, NULL);
    }

    const char *sep = "       ";
    size_t sep_cp_count = 7u;
    size_t loop_cp_len = cp_count + sep_cp_count;
    if (loop_cp_len == 0) return wm_dup_str(src, NULL);

    size_t out_cap = (size_t)vis_chars * 4u + 16u;
    char *out = (char *)kmalloc(out_cap);
    if (!out) return NULL;

    uint16_t *cp_pos = (uint16_t *)kmalloc((cp_count + 1u) * sizeof(uint16_t));
    if (!cp_pos) {
        kfree(out);
        return wm_make_marquee_alloc(src, vis_chars, offset);
    }

    size_t p = 0;
    size_t ci = 0;
    while (p < src_byte_len && ci < cp_count) {
        cp_pos[ci] = (uint16_t)p;
        size_t cb = 0;
        utf8_to_vga_glyph(&src[p], src_byte_len - p, &cb);
        if (cb == 0) { p++; continue; }
        p += cb;
        ci++;
    }
    cp_pos[ci] = (uint16_t)p;

    size_t out_len = 0;
    for (int c = 0; c < vis_chars; c++) {
        size_t cp_idx = ((size_t)offset + (size_t)c) % loop_cp_len;
        if (cp_idx < cp_count) {
            uint16_t bstart = cp_pos[cp_idx];
            uint16_t bend = cp_pos[cp_idx + 1];
            for (uint16_t b = bstart; b < bend && out_len + 1 < out_cap; b++) {
                out[out_len++] = src[b];
            }
        } else {
            if (out_len + 1 < out_cap) {
                out[out_len++] = ' ';
            }
        }
    }
    out[out_len] = '\0';
    kfree(cp_pos);
    return out;
}

static void draw_decorations(wm_window_t *win, uint8_t *bb, int bw, int bh) {
    if (win->fullscreen || win->minimized || win->decor_style == WM_DECOR_NONE) return;
    bool focused = (wm_get_focused() == win);

    if (win->decor_style == WM_DECOR_THIN) {
        wm_buf_draw_rect(bb, bw, bh, (int)win->x-1, (int)win->y-1, (int)win->w+2, (int)win->h+2,
                         focused ? WIN_CLR_HILIGHT : WIN_CLR_SHADOW);
        return;
    }

    /* Classic 3D beveled frame */
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

    uint8_t tbg = focused ? WIN_CLR_ACTIVE_TITLE : WIN_CLR_INACT_TITLE;
    uint8_t tfg = focused ? WIN_CLR_ACTIVE_TEXT : WIN_CLR_INACT_TEXT;
    wm_buf_fill_rect(bb, bw, bh, (int)win->x, (int)win->y - TITLEBAR_H, (int)win->w, TITLEBAR_H, tbg);

    if (win->title) {
        int avail_w = (int)win->w - 40;
        int max_chars = avail_w > 0 ? avail_w / 6 : 0;
        if (max_chars > 0) {
            uint32_t now = timer_millis();
            char *disp_buf = wm_make_marquee_utf8_alloc(win->title, max_chars, now / 180u);
            if (disp_buf) {
                wm_buf_draw_string(bb, bw, bh, (int)win->x + 2, (int)win->y - TITLEBAR_H + 2, disp_buf, tfg);
                kfree(disp_buf);
            }
        }
    }

    int btn_y = (int)win->y - TITLEBAR_H + 1;

    /* Close [X] */
    if (wm_window_has_close_button(win)) {
        draw_titlebar_icon_btn(bb, bw, bh, (int)win->x + (int)win->w - 9, btn_y,
                               WM_ICON_CLOSE, WIN_CLR_DKSHADOW);
    }

    /* Maximize [^] / Restore [v] */
    draw_titlebar_icon_btn(bb, bw, bh, (int)win->x + (int)win->w - 18, btn_y,
                           win->maximized ? WM_ICON_RESTORE : WM_ICON_MAX, WIN_CLR_DKSHADOW);

    /* Minimize [_] */
    draw_titlebar_icon_btn(bb, bw, bh, (int)win->x + (int)win->w - 27, btn_y,
                           WM_ICON_MIN, WIN_CLR_DKSHADOW);

    /* Fullscreen [+] (covers taskbar and WM chrome) */
    if ((int)win->w >= 40) {
        draw_titlebar_icon_btn(bb, bw, bh, (int)win->x + (int)win->w - 36, btn_y,
                               WM_ICON_FULLSCREEN, WIN_CLR_DKSHADOW);
    }
}

static char *s_last_time_fmt = NULL;
static char *s_last_date_fmt = NULL;

static bool wm_refresh_clock_text(void) {
    rtc_poll();
    size_t tlen = 0u;
    size_t dlen = 0u;
    rtc_status_t terr = RTC_OK;
    rtc_status_t derr = RTC_OK;

    char *tfmt = rtc_get_time_format_alloc();
    if (tfmt) {
        if (!s_last_time_fmt || strcmp(tfmt, s_last_time_fmt) != 0) {
            if (s_last_time_fmt) kfree(s_last_time_fmt);
            s_last_time_fmt = tfmt;
            s_time_marquee_off = 0u;
        } else {
            kfree(tfmt);
        }
    }
    char *dfmt = rtc_get_date_format_alloc();
    if (dfmt) {
        if (!s_last_date_fmt || strcmp(dfmt, s_last_date_fmt) != 0) {
            if (s_last_date_fmt) kfree(s_last_date_fmt);
            s_last_date_fmt = dfmt;
            s_date_marquee_off = 0u;
        } else {
            kfree(dfmt);
        }
    }

    char *fresh_t = rtc_format_time_alloc(&tlen, &terr);
    if (!fresh_t) {
        const char *msg = (terr == RTC_ERR_NO_SUBSECOND) ? "NO MS (1s)" : "RTC ERR";
        fresh_t = wm_dup_str(msg, &tlen);
    }
    char *fresh_d = rtc_format_date_alloc(&dlen, &derr);
    if (!fresh_d) {
        fresh_d = wm_dup_str("RTC ERR", &dlen);
    }

    bool changed = false;
    if (fresh_t) {
        if (!s_tb_time_text || strcmp(fresh_t, s_tb_time_text) != 0) {
            if (s_tb_time_text) kfree(s_tb_time_text);
            s_tb_time_text = fresh_t;
            s_tb_time_len = tlen;
            changed = true;
        } else {
            kfree(fresh_t);
        }
    }
    if (fresh_d) {
        if (!s_tb_date_text || strcmp(fresh_d, s_tb_date_text) != 0) {
            if (s_tb_date_text) kfree(s_tb_date_text);
            s_tb_date_text = fresh_d;
            s_tb_date_len = dlen;
            changed = true;
        } else {
            kfree(fresh_d);
        }
    }

    s_last_clock_gen = rtc_generation();
    return changed;
}

static uint32_t wm_small_glyph_bits(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - ('a' - 'A'));
    switch (ch) {
        case '0': return 0x69996u;
        case '1': return 0x26227u;
        case '2': return 0xE168Fu;
        case '3': return 0xE161Eu;
        case '4': return 0x99F11u;
        case '5': return 0xF8E1Eu;
        case '6': return 0x78E96u;
        case '7': return 0xF1244u;
        case '8': return 0x69696u;
        case '9': return 0x6971Eu;
        case '-': return 0x00E00u;
        case '+': return 0x04E40u;
        case '.': return 0x00004u;
        case ',': return 0x00048u;
        case ':': return 0x04040u;
        case '/': return 0x12248u;
        case '*': return 0x0A4A0u;
        case '!': return 0x44404u;
        case '?': return 0x61202u;
        case '(': return 0x24442u;
        case ')': return 0x42224u;
        case '[': return 0x64446u;
        case ']': return 0x62226u;
        case '_': return 0x0000Fu;
        case '=': return 0x0E0E0u;
        case 'A': return 0x69F99u;
        case 'B': return 0xE9E9Eu;
        case 'C': return 0x78887u;
        case 'D': return 0xE999Eu;
        case 'E': return 0xF8E8Fu;
        case 'F': return 0xF8E88u;
        case 'G': return 0x78B97u;
        case 'H': return 0x99F99u;
        case 'I': return 0xE444Eu;
        case 'J': return 0x11196u;
        case 'K': return 0x9ACA9u;
        case 'L': return 0x8888Fu;
        case 'M': return 0x9F999u;
        case 'N': return 0x9DB99u;
        case 'O': return 0x69996u;
        case 'P': return 0xE9E88u;
        case 'Q': return 0x699A5u;
        case 'R': return 0xE9EA9u;
        case 'S': return 0x7861Eu;
        case 'T': return 0xE4444u;
        case 'U': return 0x99996u;
        case 'V': return 0x99966u;
        case 'W': return 0x99BF9u;
        case 'X': return 0x99699u;
        case 'Y': return 0x99644u;
        case 'Z': return 0xF248Fu;
        default:  return 0x00000u;
    }
}

static void wm_buf_draw_small_string(uint8_t *buf, uint16_t bw, uint16_t bh,
                                     int x, int y, const char *str, uint8_t color) {
    if (!buf || !str) return;
    int cx = x;
    for (size_t i = 0u; str[i] != '\0'; i++) {
        uint32_t bits = wm_small_glyph_bits(str[i]);
        for (int r = 0; r < WM_SMALL_FONT_H; r++) {
            uint32_t row_bits = (bits >> ((WM_SMALL_FONT_H - 1 - r) * 4)) & 0x0Fu;
            for (int c = 0; c < WM_SMALL_FONT_W; c++) {
                if (row_bits & (0x08u >> c)) {
                    wm_buf_put_pixel(buf, bw, bh, cx + c, y + r, color);
                }
            }
        }
        cx += WM_SMALL_FONT_ADV;
    }
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

    const char *text = "Close";

    int text_width = 5 * WM_FONT_W;
    int text_height = WM_FONT_H;

    int tx = x + (w - text_width) / 2;
    int ty = y + (h - text_height) / 2;

    wm_buf_draw_string(bb, bw, bh, tx, ty, text, clr_text);
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
    (void)wm_refresh_clock_text();
    wm_taskbar_layout_t tb;
    wm_get_taskbar_layout(bw, bh, &tb);

    const int box_y = tb.y + 2;
    const int box_h = 17;
    const int text_y = box_y + 5;

    wm_buf_fill_rect(bb, bw, bh, 0, tb.y, bw, TASKBAR_H, WIN_CLR_FACE);
    wm_buf_draw_line(bb, bw, bh, 0, tb.y, bw - 1, tb.y, WIN_CLR_HILIGHT);

    wm_draw_close_button(bb, bw, bh, tb.close_x, box_y, tb.close_w, box_h, false);

    draw_3d_box(bb, bw, bh, tb.scroll_l_x, box_y, tb.scroll_w, box_h, false);
    wm_buf_draw_string(bb, bw, bh, tb.scroll_l_x + 2, text_y, "<", WIN_CLR_DKSHADOW);

    int win_idx = 0;
    int visible_tab = 0;
    int tab_w = tb.tab_w;
    int tab_h = box_h;
    s_tb_tabs_overflow = false;
    for (wm_window_t *w = wm_head; w; w = w->next) {
        if (!wm_window_is_visible_for_fg(w)) continue;
        if (win_idx < taskbar_scroll_offset) { win_idx++; continue; }
        win_idx++;
        if (visible_tab >= tb.max_tabs) break;

        int tx = tb.tabs_x0 + visible_tab * tb.tab_stride;
        int ty = box_y;
        bool is_active = (w == wm_get_focused() && !w->minimized);

        if (is_active) {
            draw_3d_box(bb, bw, bh, tx, ty, tab_w, tab_h, true);
            wm_buf_fill_rect(bb, bw, bh, tx + 1, ty + 1, tab_w - 2, tab_h - 2, WIN_CLR_ACTIVE_TITLE);
        } else {
            draw_3d_box(bb, bw, bh, tx, ty, tab_w, tab_h, false);
        }

        int close_pad = wm_window_has_close_button(w) ? 10 : 2;
        int max_tab_chars = (tab_w - 2 - close_pad) / 6;
        if (max_tab_chars < 1) max_tab_chars = 1;
        const char *src_title = w->title ? w->title : "Win";
        uint8_t text_color = is_active ? WIN_CLR_ACTIVE_TEXT : (w->minimized ? WIN_CLR_SHADOW : WIN_CLR_DKSHADOW);

        int vis_title_chars = (w->minimized && max_tab_chars > 1) ? (max_tab_chars - 1) : max_tab_chars;
        if (vis_title_chars < 1) vis_title_chars = 1;

        size_t title_cp_cnt = wm_utf8_char_count(src_title);
        if ((int)title_cp_cnt > vis_title_chars) {
            s_tb_tabs_overflow = true;
        }

        uint32_t now = timer_millis();
        uint32_t tab_marquee_off = now / 180u;
        char *scrolled_title = wm_make_marquee_utf8_alloc(src_title, vis_title_chars, tab_marquee_off);
        if (scrolled_title) {
            if (w->minimized && max_tab_chars > 1) {
                size_t st_len = strlen(scrolled_title);
                char *min_buf = (char *)kmalloc(st_len + 2u);
                if (min_buf) {
                    min_buf[0] = '_';
                    memcpy(min_buf + 1, scrolled_title, st_len + 1u);
                    wm_buf_draw_string(bb, bw, bh, tx + 2, ty + 5, min_buf, text_color);
                    kfree(min_buf);
                }
            } else {
                wm_buf_draw_string(bb, bw, bh, tx + 2, ty + 5, scrolled_title, text_color);
            }
            kfree(scrolled_title);
        }

        /* Per-tab [x] close button */
        if (wm_window_has_close_button(w)) {
            int close_btn_x = tx + tab_w - 9;
            int close_btn_y = ty + (tab_h - 8) / 2;
            draw_3d_box(bb, bw, bh, close_btn_x, close_btn_y, 8, 8, false);
            wm_buf_draw_string(bb, bw, bh, close_btn_x + 1, close_btn_y, "x", is_active ? 15 : WIN_CLR_DKSHADOW);
        }

        visible_tab++;
    }

    draw_3d_box(bb, bw, bh, tb.scroll_r_x, box_y, tb.scroll_w, box_h, false);
    wm_buf_draw_string(bb, bw, bh, tb.scroll_r_x + 2, text_y, ">", WIN_CLR_DKSHADOW);

    draw_3d_box(bb, bw, bh, tb.lang_x, box_y, tb.lang_w, box_h, true);
    const char *lang_name = dynamic_keymap_get_name();
    if (!lang_name || lang_name[0] == '\0') {
        lang_name = default_keymap_name;
    }
    size_t lang_byte_len = strlen(lang_name);
    int vis_chars = 13; /* (85 - 4) / 6 = 13 character cells */

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
    char *disp_buf = (char *)kmalloc(disp_cap);
    if (disp_buf) {
        if ((int)lang_cp_count <= vis_chars) {
            memcpy(disp_buf, lang_name, lang_byte_len);
            disp_buf[lang_byte_len] = '\0';
        } else {
            const char *sep = "       ";
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
        wm_buf_draw_string(bb, bw, bh, tb.lang_x + 3, text_y, disp_buf, WIN_CLR_DKSHADOW);
        kfree(disp_buf);
    }

    /* Hardware RTC two-line panel: larger time above compact date, with ticker on overflow. */
    draw_3d_box(bb, bw, bh, tb.clock_x, box_y, tb.clock_w, box_h, true);
    int inner_w = tb.clock_w - 6;
    if (inner_w < 6) inner_w = 6;
    int vis_time_chars = inner_w / WM_FONT_W;
    if (vis_time_chars < 1) vis_time_chars = 1;
    int vis_date_chars = (inner_w + 1) / WM_SMALL_FONT_ADV;
    if (vis_date_chars < 1) vis_date_chars = 1;

    s_tb_time_overflow = (s_tb_time_text && s_tb_time_len > (size_t)vis_time_chars);
    s_tb_date_overflow = (s_tb_date_text && s_tb_date_len > (size_t)vis_date_chars);

    if (s_tb_time_text && s_tb_time_len > 0u) {
        char *t_draw = wm_make_marquee_alloc(s_tb_time_text, vis_time_chars, s_time_marquee_off);
        if (t_draw) {
            int t_px = (int)strlen(t_draw) * WM_FONT_W;
            int draw_x = tb.clock_x + 3 + ((t_px < inner_w) ? ((inner_w - t_px) / 2) : 0);
            wm_buf_draw_string(bb, bw, bh, draw_x, box_y + 2, t_draw, WIN_CLR_DKSHADOW);
            kfree(t_draw);
        }
    }
    if (s_tb_date_text && s_tb_date_len > 0u) {
        char *d_draw = wm_make_marquee_alloc(s_tb_date_text, vis_date_chars, s_date_marquee_off);
        if (d_draw) {
            size_t d_cnt = strlen(d_draw);
            int d_px = (d_cnt > 0u) ? ((int)d_cnt * WM_SMALL_FONT_ADV - 1) : 0;
            int draw_x = tb.clock_x + 3 + ((d_px < inner_w) ? ((inner_w - d_px) / 2) : 0);
            wm_buf_draw_small_string(bb, bw, bh, draw_x, box_y + 10, d_draw, WIN_CLR_DKSHADOW);
            kfree(d_draw);
        }
    }
}

static void wm_compose_dims(uint8_t *backbuf, int bw, int bh) {
    wm_window_t *fw = wm_get_focused();
    bool top_fullscreen = (fw && fw->fullscreen && !fw->minimized && wm_window_is_visible_for_fg(fw));
    int max_y = top_fullscreen ? bh : wm_taskbar_y(bh);

    bool opaque_full_cover = (top_fullscreen && fw->framebuf && !fw->shape_mask &&
                              fw->x <= 0 && fw->y <= 0 &&
                              (int)fw->w >= bw && (int)fw->h >= bh);
    if (!opaque_full_cover) {
        memset(backbuf, WIN_CLR_DESKTOP, (size_t)bw * (size_t)bh);
    }

    /* Composite visible windows back-to-front (or solo focused fullscreen) */
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
    if (!s_cursor_patch_valid || !s_cursor_patch) return;
    for (int r = 0; r < 10; r++) {
        int py = s_cursor_patch_y - 1 + r;
        if (py < 0 || py >= sh) continue;
        for (int c = 0; c < 7; c++) {
            int px = s_cursor_patch_x - 1 + c;
            if (px < 0 || px >= sw) continue;
            bb[(size_t)py * (size_t)sw + (size_t)px] = s_cursor_patch[(size_t)r * 7u + (size_t)c];
        }
    }
    s_cursor_patch_valid = false;
}

static void wm_cursor_patch_save(const uint8_t *bb, int sw, int sh, int cx, int cy) {
    if (!kgrow_array((void **)&s_cursor_patch, &s_cursor_patch_cap, 10u * 7u, sizeof(uint8_t), 70u)) {
        s_cursor_patch_valid = false;
        return;
    }
    s_cursor_patch_x = cx;
    s_cursor_patch_y = cy;
    for (int r = 0; r < 10; r++) {
        int py = cy - 1 + r;
        if (py < 0 || py >= sh) continue;
        for (int c = 0; c < 7; c++) {
            int px = cx - 1 + c;
            if (px < 0 || px >= sw) continue;
            s_cursor_patch[(size_t)r * 7u + (size_t)c] = bb[(size_t)py * (size_t)sw + (size_t)px];
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

    uint8_t sc;
    ipo_key_event_t kev;
    memset(&kev, 0, sizeof(kev));
    while ((sc = keyboard_get_scancode_raw()) != 0) {
        update_hot_key_state(sc);
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
            if (!is_e0 && make == 0x3Cu) { /* F2: cycle keymap */
                dynamic_keymap_cycle_next();
                continue;
            } else if (!is_e0 && make == 0x57u) { /* F11: toggle fullscreen */
                if (fw) wm_set_fullscreen(fw, !fw->fullscreen);
                continue;
            } else if (!is_e0 && make == 0x0Fu && keyboard_is_ctrl_pressed() && !keyboard_is_alt_pressed()) { /* Ctrl(+Shift)+Tab */
                if (keyboard_is_shift_pressed()) wm_focus_prev(); else wm_focus_next();
                continue;
            } else if (keyboard_is_ctrl_pressed() && (make == 0x49u || make == 0x51u)) {
                /* Ctrl+PageUp (0x49) / Ctrl+PageDown (0x51): switch batch workspace */
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
                if (kev.raw_bytes) kfree(kev.raw_bytes);
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
            for (uint32_t bi = 0u; bi < kev.raw_len && kev.raw_bytes; bi++) {
                uint8_t b = kev.raw_bytes[bi];
                uint32_t ev_t = (b == 0xE0u || b == 0xE1u) ? WM_EVENT_KEY_DOWN
                                : ((b & 0x80u) ? WM_EVENT_KEY_UP : WM_EVENT_KEY_DOWN);
                wm_push_event(fw, ev_t, (uint32_t)b);
            }
        }
    }
    if (kev.raw_bytes) kfree(kev.raw_bytes);
    if (!wm_sess_active) return;
    if (!process_wm_has_focus()) {
        wm_cancel_mouse_interaction();
        return;
    }

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

        if (ms.y >= tb.y && !top_fullscreen) {
            if (ms.x >= tb.close_x && ms.x <= tb.close_x + tb.close_w) {
                wm_handle_close_button_click();
                if (!wm_sess_active || !process_wm_has_focus() || !vga_is_graphics_mode()) {
                    return;
                }
                handled = true;
            }
            if (!handled && ms.x >= tb.scroll_l_x && ms.x <= tb.scroll_l_x + tb.scroll_w) {
                if (taskbar_scroll_offset > 0) taskbar_scroll_offset--;
                handled = true;
            }
            if (!handled && ms.x >= tb.scroll_r_x && ms.x <= tb.scroll_r_x + tb.scroll_w) {
                if (taskbar_scroll_offset + tb.max_tabs < wm_count_visible_windows()) taskbar_scroll_offset++;
                handled = true;
            }
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
            if (!handled && ms.x >= tb.lang_x && ms.x < tb.lang_x + tb.lang_w) {
                dynamic_keymap_cycle_next();
                handled = true;
            }
            handled = true;
        }

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
                        if (ms.y < (int)w->y) {
                            int titlebar_button_y = (int)w->y - TITLEBAR_H + 1;
                            bool in_titlebar_button_y =
                                ms.y >= titlebar_button_y && ms.y < titlebar_button_y + 8;

                            /* Fullscreen [+] */
                            if (in_titlebar_button_y && (int)w->w >= 40 &&
                                ms.x >= (int)w->x + (int)w->w - 36 &&
                                ms.x < (int)w->x + (int)w->w - 28) {
                                wm_set_fullscreen(w, true);
                                break;
                            }

                            /* Close [X] */
                            if (wm_window_has_close_button(w) && in_titlebar_button_y &&
                                ms.x >= (int)w->x + (int)w->w - 9 &&
                                ms.x < (int)w->x + (int)w->w - 1) {
                                wm_close_window_by_user(w);
                                break;
                            }

                            /* Maximize [^] / Restore [v] */
                            if (in_titlebar_button_y &&
                                ms.x >= (int)w->x + (int)w->w - 18 &&
                                ms.x < (int)w->x + (int)w->w - 10) {
                                wm_toggle_maximize(w);
                                break;
                            }

                            /* Minimize [_] */
                            if (in_titlebar_button_y &&
                                ms.x >= (int)w->x + (int)w->w - 27 &&
                                ms.x < (int)w->x + (int)w->w - 19) {
                                wm_minimize(w);
                                break;
                            }

                            uint32_t now = timer_millis();
                            if (last_title_click_win == w && (now - last_title_click_time) < 400) {
                                last_title_click_win = NULL;
                                wm_toggle_maximize(w);
                                break;
                            }
                            last_title_click_win = w;
                            last_title_click_time = now;

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

                        /* Restrict resize hit-testing to the outer frame so client-area
                         * clicks (including custom-cursor apps) never start a resize drag. */
                        bool on_outer_frame =
                            (ms.x < (int)w->x || ms.x >= (int)w->x + (int)w->w ||
                             ms.y >= (int)w->y + (int)w->h);

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
    } else if (left_now && drag_win != NULL) {
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

    rtc_poll();

    {
        uint32_t now = timer_millis();
        if ((s_tb_time_overflow || s_tb_date_overflow || s_tb_tabs_overflow) &&
            (uint32_t)(now - s_clock_marquee_last_tick) >= 180u) {
            s_clock_marquee_last_tick = now;
            if (s_tb_time_overflow && s_tb_time_len > 0u) {
                s_time_marquee_off = (s_time_marquee_off + 1u) % ((uint32_t)s_tb_time_len + 7u);
            }
            if (s_tb_date_overflow && s_tb_date_len > 0u) {
                s_date_marquee_off = (s_date_marquee_off + 1u) % ((uint32_t)s_tb_date_len + 7u);
            }
            wm_window_t *fw = wm_get_focused();
            bool top_fs = (fw && fw->fullscreen && !fw->minimized && wm_window_is_visible_for_fg(fw));
            if (!top_fs) wm_surface_dirty = true;
        }
    }

    if (!wm_surface_dirty) {
        uint32_t now = timer_millis();
        uint32_t cgen = rtc_generation();
        uint32_t clock_period = rtc_time_format_uses_subsecond() ? 50u : 180u;
        if (cgen != s_last_clock_gen || (uint32_t)(now - s_last_clock_check_ms) >= clock_period) {
            s_last_clock_check_ms = now;
            wm_window_t *fw = wm_get_focused();
            bool top_fs = (fw && fw->fullscreen && !fw->minimized && wm_window_is_visible_for_fg(fw));
            if (!top_fs && wm_refresh_clock_text()) {
                wm_surface_dirty = true;
            }
        }
        if (!wm_surface_dirty && (uint32_t)(now - s_last_slow_check_ms) >= 180u) {
            s_last_slow_check_ms = now;
            wm_window_t *fw = wm_get_focused();
            bool top_fs = (fw && fw->fullscreen && !fw->minimized && wm_window_is_visible_for_fg(fw));
            if (!top_fs) {
                const char *cur_lang = dynamic_keymap_get_name();
                if (!cur_lang || !cur_lang[0]) cur_lang = default_keymap_name;
                if (!s_last_lang_name || strcmp(cur_lang, s_last_lang_name) != 0) {
                    wm_set_lang_name(cur_lang);
                    wm_surface_dirty = true;
                }
                if (strlen(cur_lang) > 13u || s_tb_tabs_overflow) {
                    wm_surface_dirty = true;
                } else {
                    uint32_t mtick = now / 180u;
                    if (mtick != s_last_title_marquee_tick) {
                        for (wm_window_t *w = wm_head; w; w = w->next) {
                            if (!wm_window_is_visible_for_fg(w) || w->minimized || w->fullscreen || !w->title) continue;
                            if (w->decor_style != WM_DECOR_DEFAULT) continue;
                            int max_chars = ((int)w->w - 40) / 6;
                            if (max_chars > 0 && (int)wm_utf8_char_count(w->title) > max_chars) {
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

bool wm_session_active(void) { return wm_sess_active; }

void wm_get_screen_bounds(int *out_w, int *out_h) {
    vga_gfx_get_screen_bounds(out_w, out_h);
}

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

    wm_render_frame(screen_w, screen_h, screen_w / 2, screen_h / 2, should_switch_hw);
}

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
    /* Terminal apps render directly to VGA text mode behind the WM. */
    (void)c;
    (void)fg;
    (void)bg;
}
