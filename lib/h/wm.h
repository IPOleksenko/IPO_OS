/**
 * wm.h  —  Window Manager API for IPO_OS (VGA Mode 13h: 320×200, 256 colours)
 *
 * Architecture
 * ============
 * The WM runs as a persistent kernel async task — not inside any single app.
 *
 * wm_session_start()  — enters VGA mode, allocates backbuffer, registers the
 *                        compositor as an async task, returns immediately.
 * wm_session_stop()   — stops the async task, returns to text mode.
 *
 * Apps that want to show windows:
 *   1. Call wm_create_window() (or wm_register_window()) → window added to
 *      the shared global list.
 *   2. Return from main().  The compositor task keeps drawing their window.
 *   3. Optionally call wm_destroy_window() before returning to remove it.
 *
 * This means `run wm_demo & wm_demo2` works correctly:
 *   - wm_demo  creates its windows, returns.
 *   - wm_demo2 creates its windows, returns.
 *   - The WM async task composites both window sets every tick.
 *
 * System overlay
 * ==============
 * The compositor always draws a small [✕] button in the bottom-left corner.
 * Clicking it (left mouse button) calls wm_session_stop().
 *
 * Mouse cursor
 * ============
 * The compositor draws a software arrow cursor on every frame using the
 * position from mouse_get_state().  mouse_set_bounds(320,200) is called
 * during session start so coordinates are in Mode-13h screen space.
 */

#ifndef LIB_WM_H
#define LIB_WM_H

#include <stdint.h>
#include <stdbool.h>
#include <vga_gfx.h>

/* =========================================================================
 * Decoration style
 * ========================================================================= */
typedef enum {
    WM_DECOR_DEFAULT = 0, /* Standard title bar + border                     */
    WM_DECOR_THIN,        /* 1-pixel border, no title bar                    */
    WM_DECOR_NONE,        /* No border or title bar at all                   */
    WM_DECOR_CUSTOM,      /* Caller's draw_cb is responsible for all chrome  */
} wm_decor_style_t;

/* =========================================================================
 * Event codes passed to event_cb
 * ========================================================================= */
#define WM_EVENT_KEY_DOWN   0x00000001u
#define WM_EVENT_KEY_UP     0x00000002u
#define WM_EVENT_FOCUS_IN   0x00000010u
#define WM_EVENT_FOCUS_OUT  0x00000020u
#define WM_EVENT_CLOSE      0x00000100u
#define WM_EVENT_RESIZE     0x00000200u
#define WM_EVENT_MOVE       0x00000400u
#define WM_EVENT_CLICK      0x00001000u  /* left mouse button down inside window */

/* =========================================================================
 * Forward declaration
 * ========================================================================= */
typedef struct wm_window wm_window_t;

/* =========================================================================
 * Callback signatures
 * ========================================================================= */
typedef void (*wm_draw_cb_t) (wm_window_t *win);
typedef void (*wm_event_cb_t)(wm_window_t *win, uint32_t event, uint32_t data);

/* =========================================================================
 * Window options – pass to wm_create_window()
 * ========================================================================= */
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

    /* Shape mask (NULL = rectangular) */
    const uint8_t *shape_mask;
    uint16_t mask_w, mask_h;

    /* NULL = auto-allocate */
    uint8_t *framebuf;

    wm_draw_cb_t  draw_cb;
    wm_event_cb_t event_cb;
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
    .draw_cb     = NULL,              \
    .event_cb    = NULL,              \
    .user_data   = NULL,              \
}

/* =========================================================================
 * Window structure
 * ========================================================================= */
struct wm_window {
    int16_t  x, y;
    uint16_t w, h;
    bool     fullscreen;
    bool     minimized;
    bool     maximized;
    int16_t  saved_x, saved_y;
    uint16_t saved_w, saved_h;

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
    bool     shape_mask_owned;  /* true if shape_mask was kmalloc'd by wm_create_window */

    wm_draw_cb_t  draw_cb;
    wm_event_cb_t event_cb;
    void         *user_data;
    uint32_t      owner_pid;

    bool dirty;

    wm_window_t *prev;
    wm_window_t *next;
};

/* =========================================================================
 * Session API  (video-mode lifecycle)
 * =========================================================================
 *
 * wm_session_start() — NON-BLOCKING (async compositor).
 *   Enters VGA Mode 13h, registers an async compositor task that fires
 *   every 16 ms.  Returns immediately so the calling app / terminal
 *   can continue running.  All windows added via wm_create_window() are
 *   composited by this task.  wm_create_window() calls wm_session_start()
 *   automatically if no session is running yet.
 *   Exit: left-click [X Exit] button or press ESC/Q.
 *
 * wm_session_run() — BLOCKING (single-app animated mode).
 *   Like wm_session_start() but blocks in a loop, calling draw_cb every
 *   frame so windows can animate.  Only the calling app's code is alive,
 *   so only its draw_cb pointers are guaranteed valid.
 *   Exit: same as above.
 *
 * wm_session_stop() — stop compositor (or blocking loop), destroy all
 *   windows, restore text mode.  Safe to call from event/draw callbacks.
 *
 * wm_session_active() — true while either mode is running.
 */
void wm_session_start(void);   /* async, non-blocking */
void wm_session_run(void);     /* blocking (animated single-app mode) */
void wm_session_stop(void);
bool wm_session_active(void);

/* =========================================================================
 * Core window API
 * ========================================================================= */

/** wm_init - (Re)initialise window list without entering video mode. */
void wm_init(void);

wm_window_t *wm_create_window(const wm_window_options_t *opts);
void         wm_destroy_window(wm_window_t *win);

int          wm_get_window_count(void);
wm_window_t *wm_get_focused(void);
void         wm_set_focus(wm_window_t *win);
void         wm_focus_next(void);
void         wm_focus_prev(void);

void wm_invalidate(wm_window_t *win);
void wm_invalidate_all(void);

void wm_set_fullscreen(wm_window_t *win, bool enable);
void wm_minimize(wm_window_t *win);
void wm_restore(wm_window_t *win);
void wm_toggle_maximize(wm_window_t *win);
void wm_move(wm_window_t *win, int16_t nx, int16_t ny);
bool wm_resize(wm_window_t *win, uint16_t nw, uint16_t nh);
void wm_set_title(wm_window_t *win, const char *title);
void wm_set_shape(wm_window_t *win, const uint8_t *mask, uint16_t mw, uint16_t mh);

/**
 * wm_dispatch_key - Route a keyboard scancode.
 * Returns false if ESC/Q pressed and caller should exit.
 * NOTE: The session's async tick calls this automatically.
 * Apps using the old single-loop model can call it too.
 */
bool wm_dispatch_key(uint8_t scancode);

/**
 * wm_compose - Composite all windows + system overlay + cursor onto @backbuf.
 */
void wm_compose(uint8_t *backbuf);

/* =========================================================================
 * Drawing helpers  (operate on any uint8_t pixel buffer)
 * ========================================================================= */
void wm_buf_put_pixel   (uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, uint8_t color);
void wm_buf_draw_line   (uint8_t *buf, uint16_t bw, uint16_t bh, int x0, int y0, int x1, int y1, uint8_t color);
void wm_buf_fill_rect   (uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, int w, int h, uint8_t color);
void wm_buf_draw_rect   (uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, int w, int h, uint8_t color);
void wm_buf_draw_char   (uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, char c,         uint8_t color);
void wm_buf_draw_string (uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, const char *str, uint8_t color);

/* =========================================================================
 * Compositor tick  (called from terminal_console each kernel loop iteration)
 * =========================================================================
 *
 * Polls keyboard and mouse, composites all windows, draws cursor and the
 * system exit button, then flips the backbuffer to VRAM.
 * Stops the session on ESC/Q or left-click of the [X Exit] button.
 *
 * Called automatically by terminal_console() when wm_session_active().
 * Kernel / driver code MUST NOT call this directly.
 */
void wm_compositor_tick(void);

/* =========================================================================
 * Legacy / convenience shim
 * ========================================================================= */
wm_window_t *wm_register_window(const char *title,
                                  int16_t x, int16_t y,
                                  uint16_t w, uint16_t h,
                                  wm_draw_cb_t  draw_cb,
                                  wm_event_cb_t event_cb,
                                  void *user_data);
void wm_remove_window(wm_window_t *win);

#endif /* LIB_WM_H */
