/* Window manager API (VGA Mode 13h 320x200x8 / linear framebuffer).
 * Driven synchronously by wm_compositor_tick() rather than an async task.
 * Apps register windows via wm_create_window() and either poll events or return. */

#ifndef LIB_WM_H
#define LIB_WM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <vga_gfx.h>

typedef enum {
    WM_DECOR_DEFAULT = 0, /* title bar + border */
    WM_DECOR_THIN,        /* 1px border, no title bar */
    WM_DECOR_NONE,        /* borderless */
} wm_decor_style_t;

#define WM_EVENT_KEY_DOWN   0x00000001u
#define WM_EVENT_KEY_UP     0x00000002u
#define WM_EVENT_KEY_EXT    0x00000004u  /* low 31 bits = keycode; bit 31 = break */
#define WM_EVENT_FOCUS_IN   0x00000010u
#define WM_EVENT_FOCUS_OUT  0x00000020u
#define WM_EVENT_CLOSE      0x00000100u
#define WM_EVENT_RESIZE     0x00000200u
#define WM_EVENT_MOVE       0x00000400u
#define WM_EVENT_CLICK      0x00001000u  /* non-drag left click inside window */
#define WM_EVENT_SHORTCUT   0x00002000u  /* registered shortcut fired (data = tag) */

typedef struct wm_window wm_window_t;

typedef struct {
    uint32_t type;
    uint32_t data;
} wm_event_t;

typedef struct {
    int16_t  x, y;
    uint16_t w, h;
    bool     fullscreen;

    const char *title;

    wm_decor_style_t decor_style;
    uint8_t  bg_color;
    uint8_t  border_color;
    uint8_t  title_bg;
    uint8_t  title_fg;

    /* NULL = rectangular */
    const uint8_t *shape_mask;
    uint16_t mask_w, mask_h;

    /* NULL = heap-allocated */
    uint8_t *framebuf;

    void         *user_data;
} wm_window_options_t;

#define WM_WINDOW_OPTIONS_DEFAULT {   \
    .x = 10, .y = 10,                 \
    .w = 100, .h = 60,                \
    .fullscreen  = false,             \
    .title       = NULL,              \
    .decor_style = WM_DECOR_DEFAULT,  \
    .bg_color    = 7,                 \
    .border_color= 15,                \
    .title_bg    = 1,                 \
    .title_fg    = 15,                \
    .shape_mask  = NULL,              \
    .mask_w = 0, .mask_h = 0,         \
    .framebuf    = NULL,              \
    .user_data   = NULL,              \
}

struct wm_window {
    int16_t  x, y;
    uint16_t w, h;
    bool     fullscreen;
    bool     minimized;
    bool     maximized;
    int16_t  saved_x, saved_y;
    uint16_t saved_w, saved_h;
    int16_t  fullscreen_saved_x, fullscreen_saved_y;
    uint16_t fullscreen_saved_w, fullscreen_saved_h;

    char *title;

    wm_decor_style_t decor_style;
    uint8_t  bg_color;
    uint8_t  border_color;
    uint8_t  title_bg;
    uint8_t  title_fg;

    uint8_t *framebuf;
    bool     framebuf_owned;

    const uint8_t *shape_mask;
    uint16_t mask_w, mask_h;
    bool     shape_mask_owned;  /* true when WM owns a copied shape mask */

    void         *user_data;
    uint32_t      owner_pid;
    int           window_id; /* workspace ID in batch mode */

    bool dirty;

    wm_window_t *prev;
    wm_window_t *next;

    /* Kernel-only tail; fields above form the user-space ABI. */
    wm_event_t *event_queue;
    uint16_t    event_cap;
    uint16_t    event_head;
    uint16_t    event_tail;
    uint32_t   close_requested_ms;
    bool       cursor_hidden;      /* hide WM cursor inside focused client area */
};

/* wm_session_start() switches mode and returns immediately; wm_create_window()
 * starts a session on demand if none is active. */
void wm_session_start(void);
void wm_session_stop(void);
bool wm_session_active(void);

/* Reset window list without entering video mode */
void wm_init(void);

wm_window_t *wm_create_window(const wm_window_options_t *opts);
void         wm_destroy_window(wm_window_t *win);
void         wm_destroy_windows_for_pid(uint32_t pid);
bool         wm_is_window_valid(wm_window_t *win);

int          wm_get_window_count(void);
int          wm_get_window_count_for_pid(uint32_t pid);
bool         wm_has_windows_for_pid(uint32_t pid);
wm_window_t *wm_get_window_list(void);
wm_window_t *wm_get_focused(void);
void         wm_set_focus(wm_window_t *win);
void         wm_focus_window_for_pid(uint32_t pid);
void         wm_focus_next(void);
void         wm_focus_prev(void);

void wm_invalidate(wm_window_t *win);
void wm_invalidate_all(void);
void wm_notify_window_event(wm_window_t *win, uint32_t event, uint32_t data);
bool wm_poll_event(wm_window_t *win, wm_event_t *out);

void wm_set_fullscreen(wm_window_t *win, bool enable);
void wm_minimize(wm_window_t *win);
void wm_restore(wm_window_t *win);
void wm_toggle_maximize(wm_window_t *win);
void wm_move(wm_window_t *win, int16_t nx, int16_t ny);
bool wm_resize(wm_window_t *win, uint16_t nw, uint16_t nh);
void wm_set_title(wm_window_t *win, const char *title);
void wm_set_shape(wm_window_t *win, const uint8_t *mask, uint16_t mw, uint16_t mh);

/* Show/hide WM software cursor while win is focused and pointer is in client area */
void wm_set_cursor_visible(wm_window_t *win, bool visible);
bool wm_get_cursor_visible(const wm_window_t *win);

/* Driven from terminal_console loop: reads input, composites, and flips */
void wm_compositor_tick(void);
void wm_terminal_putc(char c, uint8_t fg, uint8_t bg);

void wm_get_screen_bounds(int *out_w, int *out_h);
void wm_on_resolution_changed(void);

#endif /* LIB_WM_H */
