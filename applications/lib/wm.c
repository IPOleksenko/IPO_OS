#include <wm.h>
#include <syscall.h>
#include <string.h>
#include <vga.h>

static int wm_call_window(uint32_t syscall_num, wm_window_t *win) {
    if (!win) return 0;
    uint32_t args[1] = { (uint32_t)(uintptr_t)win };
    return ipo_syscall(syscall_num, 1u, args);
}

wm_window_t *wm_create_window(const wm_window_options_t *opts) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)opts;
    return (wm_window_t *)(uintptr_t)ipo_syscall(IPO_SYSCALL_WM_CREATE_WINDOW, 1u, args);
}

void wm_destroy_window(wm_window_t *win) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)win;
    ipo_syscall(IPO_SYSCALL_WM_DESTROY_WINDOW, 1u, args);
}

bool wm_is_window_valid(wm_window_t *win) {
    if (!win) return false;
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)win;
    return (bool)ipo_syscall(IPO_SYSCALL_WM_IS_VALID, 1u, args);
}

void wm_session_start(void) {
    ipo_syscall(IPO_SYSCALL_WM_SESSION_START, 0u, NULL);
}

void wm_session_stop(void) {
    ipo_syscall(IPO_SYSCALL_WM_SESSION_STOP, 0u, NULL);
}

bool wm_session_active(void) {
    return (bool)ipo_syscall(IPO_SYSCALL_WM_SESSION_ACTIVE, 0u, NULL);
}

int wm_get_window_count(void) {
    return (int)ipo_syscall(IPO_SYSCALL_WM_GET_COUNT, 0u, NULL);
}

int wm_get_window_count_for_pid(uint32_t pid) {
    uint32_t args[1] = { pid };
    return ipo_syscall(IPO_SYSCALL_WM_GET_COUNT_PID, 1u, args);
}

bool wm_has_windows_for_pid(uint32_t pid) {
    uint32_t args[1] = { pid };
    return ipo_syscall(IPO_SYSCALL_WM_HAS_WINDOWS_PID, 1u, args) != 0;
}

wm_window_t *wm_get_focused(void) {
    return (wm_window_t *)(uintptr_t)ipo_syscall(IPO_SYSCALL_WM_GET_FOCUSED, 0u, NULL);
}

wm_window_t *wm_get_window_list(void) {
    return (wm_window_t *)(uintptr_t)ipo_syscall(IPO_SYSCALL_WM_GET_LIST, 0u, NULL);
}

void wm_set_focus(wm_window_t *win) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)win;
    ipo_syscall(IPO_SYSCALL_WM_SET_FOCUS, 1u, args);
}

void wm_focus_window_for_pid(uint32_t pid) {
    uint32_t args[1] = { pid };
    ipo_syscall(IPO_SYSCALL_WM_FOCUS_PID, 1u, args);
}

void wm_focus_next(void) {
    ipo_syscall(IPO_SYSCALL_WM_FOCUS_NEXT, 0u, NULL);
}

void wm_focus_prev(void) {
    ipo_syscall(IPO_SYSCALL_WM_FOCUS_PREV, 0u, NULL);
}

void wm_get_screen_bounds(int *out_w, int *out_h) {
    if (out_w) *out_w = 320;
    if (out_h) *out_h = 200;
}

void wm_invalidate(wm_window_t *win) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)win;
    ipo_syscall(IPO_SYSCALL_WM_INVALIDATE, 1u, args);
}

void wm_invalidate_all(void) {
    ipo_syscall(IPO_SYSCALL_WM_INVALIDATE_ALL, 0u, NULL);
}

void wm_init(void) {
    ipo_syscall(IPO_SYSCALL_WM_INIT, 0u, NULL);
}

void wm_set_fullscreen(wm_window_t *win, bool enable) {
    if (!win) return;
    uint32_t args[2] = { (uint32_t)(uintptr_t)win, (uint32_t)enable };
    ipo_syscall(IPO_SYSCALL_WM_SET_FULLSCREEN, 2u, args);
}

void wm_minimize(wm_window_t *win) {
    wm_call_window(IPO_SYSCALL_WM_MINIMIZE, win);
}

void wm_restore(wm_window_t *win) {
    wm_call_window(IPO_SYSCALL_WM_RESTORE, win);
}

void wm_toggle_maximize(wm_window_t *win) {
    if (!win) return;
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)win;
    ipo_syscall(IPO_SYSCALL_WM_TOGGLE_MAXIMIZE, 1u, args);
}

void wm_move(wm_window_t *win, int16_t nx, int16_t ny) {
    if (!win) return;
    uint32_t args[3] = { (uint32_t)(uintptr_t)win, (uint32_t)(int32_t)nx,
                         (uint32_t)(int32_t)ny };
    ipo_syscall(IPO_SYSCALL_WM_MOVE, 3u, args);
}

bool wm_resize(wm_window_t *win, uint16_t nw, uint16_t nh) {
    if (!win) return false;
    uint32_t args[3];
    args[0] = (uint32_t)(uintptr_t)win;
    args[1] = (uint32_t)nw;
    args[2] = (uint32_t)nh;
    return (bool)ipo_syscall(IPO_SYSCALL_WM_RESIZE, 3u, args);
}

void wm_set_title(wm_window_t *win, const char *title) {
    if (!win) return;
    uint32_t args[2] = { (uint32_t)(uintptr_t)win, (uint32_t)(uintptr_t)title };
    ipo_syscall(IPO_SYSCALL_WM_SET_TITLE, 2u, args);
}

void wm_set_shape(wm_window_t *win, const uint8_t *mask, uint16_t mw, uint16_t mh) {
    if (!win) return;
    uint32_t args[4] = { (uint32_t)(uintptr_t)win, (uint32_t)(uintptr_t)mask,
                         (uint32_t)mw, (uint32_t)mh };
    ipo_syscall(IPO_SYSCALL_WM_SET_SHAPE, 4u, args);
}

bool wm_dispatch_key(uint8_t scancode) {
    uint32_t args[1] = { (uint32_t)scancode };
    return ipo_syscall(IPO_SYSCALL_WM_DISPATCH_KEY, 1u, args) != 0;
}

void wm_compose(uint8_t *backbuf) {
    if (!backbuf) return;
    uint32_t args[1] = { (uint32_t)(uintptr_t)backbuf };
    ipo_syscall(IPO_SYSCALL_WM_COMPOSE, 1u, args);
}

void wm_compositor_tick(void) {
    ipo_syscall(IPO_SYSCALL_WM_COMPOSITOR_TICK, 0u, NULL);
}

void wm_session_run(void) {
    ipo_syscall(IPO_SYSCALL_WM_SESSION_RUN, 0u, NULL);
}

/* Userland buffer drawing helpers */
void wm_buf_put_pixel(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, uint8_t c) {
    if (x >= 0 && x < (int)bw && y >= 0 && y < (int)bh && buf) {
        buf[y * bw + x] = c;
    }
}

void wm_buf_fill_rect(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, int w, int h, uint8_t c) {
    if (!buf || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    int x2 = x + w;
    int y2 = y + h;
    if (x2 > (int)bw) x2 = (int)bw;
    if (y2 > (int)bh) y2 = (int)bh;
    if (x2 <= x || y2 <= y) return;
    for (int cy = y; cy < y2; cy++) {
        memset(buf + (size_t)cy * bw + x, c, (size_t)(x2 - x));
    }
}

void wm_buf_draw_rect(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, int w, int h, uint8_t c) {
    if (!buf || w <= 0 || h <= 0) return;
    for (int cx = x; cx < x + w; cx++) {
        wm_buf_put_pixel(buf, bw, bh, cx, y, c);
        wm_buf_put_pixel(buf, bw, bh, cx, y + h - 1, c);
    }
    for (int cy = y; cy < y + h; cy++) {
        wm_buf_put_pixel(buf, bw, bh, x, cy, c);
        wm_buf_put_pixel(buf, bw, bh, x + w - 1, cy, c);
    }
}

void wm_buf_draw_line(uint8_t *buf, uint16_t bw, uint16_t bh, int x0, int y0, int x1, int y1, uint8_t c) {
    if (!buf) return;
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    while (1) {
        wm_buf_put_pixel(buf, bw, bh, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}


void wm_buf_draw_glyph(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, uint8_t glyph, uint8_t c) {
    const uint8_t *bitmap = vga_font_get_cached_glyph(glyph);
    if (!bitmap) return;
    for (int row = 0; row < WM_FONT_H; row++) {
        uint8_t line = bitmap[row];
        for (int col = 0; col < WM_FONT_W; col++) {
            if (line & (0x80 >> col)) {
                wm_buf_put_pixel(buf, bw, bh, x + col, y + row, c);
            }
        }
    }
}

void wm_buf_draw_char(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, char ch, uint8_t c) {
    uint8_t u = (uint8_t)ch;
    if (u < 0x20 || u > 0x7E) return;
    wm_buf_draw_glyph(buf, bw, bh, x, y, u, c);
}

void wm_buf_draw_string(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, const char *str, uint8_t c) {
    if (!str) return;
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
        size_t bytes = 0;
        uint8_t slot = utf8_to_vga_glyph(&str[i], len - i, &bytes);
        if (bytes == 0) {
            i++;
            continue;
        }
        wm_buf_draw_glyph(buf, bw, bh, cx, y, slot, c);
        cx += WM_FONT_W;
        i += bytes;
    }
}
