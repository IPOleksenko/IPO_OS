/**
 * wm.h  —  Window Manager API for IPO_OS (VGA Mode 13h: 320×200, 256 colours)
 *
 * Architecture
 * ============
 * The WM session is serviced by kernel paths that invoke
 * wm_compositor_tick(); it does not register a periodic async task.
 *
 * wm_session_start()  — activates the session, conditionally switches to
 *                        VGA mode, and returns immediately.
 * wm_session_stop()   — ends the session, destroys its windows, and restores
 *                        text mode.
 *
 * Apps that want to show windows:
 *   1. Call wm_create_window() → window added to
 *      the shared global list.
 *   2. Return from main().  Kernel paths continue servicing the WM session.
 *   3. Optionally call wm_destroy_window() before returning to remove it.
 *
 * Multiple separate programs running simultaneously can each create windows
 * independently:
 *   - each program creates its windows and returns or runs an event loop.
 *   - WM ticks composite the visible windows.
 *
 * System overlay
 * ==============
 * The close button in the bottom-left corner can be hidden with
 * VGA_OVERLAY_HIDE_BUTTON. Clicking it invokes the close handler, which
 * closes the displayed workspace or stops the WM session depending on mode.
 *
 * Mouse cursor
 * ============
 * The compositor draws a software arrow cursor using the position from
 * mouse_get_state(), unless VGA_OVERLAY_HIDE_CURSOR is set. Mouse boundaries
 * are derived from the active display area during session start/mode switches.
 */

#ifndef LIB_WM_H
#define LIB_WM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <vga_gfx.h>

/* Decoration style */
typedef enum {
    WM_DECOR_DEFAULT = 0, /* Standard title bar + border                     */
    WM_DECOR_THIN,        /* 1-pixel border, no title bar                    */
    WM_DECOR_NONE,        /* No border or title bar at all                   */
    WM_DECOR_CUSTOM,      /* Caller's draw_cb is responsible for all chrome  */
} wm_decor_style_t;

/* Event codes passed to event_cb */
#define WM_EVENT_KEY_DOWN   0x00000001u
#define WM_EVENT_KEY_UP     0x00000002u
#define WM_EVENT_KEY_EXT    0x00000004u  /* full 32-bit keycode in low 31 bits; bit 31 = break */
#define WM_EVENT_FOCUS_IN   0x00000010u
#define WM_EVENT_FOCUS_OUT  0x00000020u
#define WM_EVENT_CLOSE      0x00000100u
#define WM_EVENT_RESIZE     0x00000200u
#define WM_EVENT_MOVE       0x00000400u
#define WM_EVENT_CLICK      0x00001000u  /* left mouse click inside window without dragging */
#define WM_EVENT_SHORTCUT   0x00002000u  /* custom registered shortcut fired (data = tag) */

typedef struct wm_window wm_window_t;

/* Callback signatures */
typedef void (*wm_draw_cb_t) (wm_window_t *win);
typedef void (*wm_event_cb_t)(wm_window_t *win, uint32_t event, uint32_t data);

/* Window options passed to wm_create_window() */
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

/* Window structure */
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
    bool     shape_mask_owned;  /* true if the WM owns a copied shape mask */

    wm_draw_cb_t  draw_cb;
    wm_event_cb_t event_cb;
    void         *user_data;
    uint32_t      owner_pid;
    int           window_id; // Workspace ID in batch mode

    bool dirty;

    wm_window_t *prev;
    wm_window_t *next;

    /* Kernel-only fields (kept at the end so the user-space layout of the
     * fields above stays ABI compatible).
     * OS-level bridge for raw graphics apps (Mode 13h / vga_gfx_flip*) that
     * share a workspace with other processes: the kernel renders each such
     * app into its own window instead of letting it draw fullscreen. */
    bool     is_raw_gfx_bridge;
    uint8_t *raw_lut;             /* dynamic 256-entry palette map (app -> WM DAC) */
    uint8_t *raw_kbd_queue;       /* routed scancode queue */
    size_t   raw_kbd_cap;
    size_t   raw_kbd_head;
    size_t   raw_kbd_tail;
};

/* Session API (video-mode lifecycle)
 *
 * wm_session_start() — NON-BLOCKING.
 *   Activates the session and switches to VGA Mode 13h only when required.
 *   Returns immediately. Kernel/process paths invoke wm_compositor_tick() to
 *   service the session. wm_create_window() starts the session automatically
 *   if no session is running.
 *   ESC/Q are not session-exit shortcuts; close-button behavior depends on
 *   the current mode.
 *
 * wm_session_run() — BLOCKING animated mode.
 *   Blocks in a loop and calls each window's draw_cb every frame. Callback
 *   code must remain valid while the session is running.
 *   The loop ends when the session is stopped.
 *
 * wm_session_stop() — end the session, destroy all windows, and restore
 *   text mode.
 *
 * wm_session_active() — true while either mode is running.
 */
void wm_session_start(void);   /* async, non-blocking */
void wm_session_run(void);     /* blocking (animated single-app mode) */
void wm_session_stop(void);
bool wm_session_active(void);

/* Core window API */

/** wm_init - (Re)initialise window list without entering video mode. */
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

void wm_set_fullscreen(wm_window_t *win, bool enable);
void wm_minimize(wm_window_t *win);
void wm_restore(wm_window_t *win);
void wm_toggle_maximize(wm_window_t *win);
void wm_move(wm_window_t *win, int16_t nx, int16_t ny);
bool wm_resize(wm_window_t *win, uint16_t nw, uint16_t nh);
void wm_set_title(wm_window_t *win, const char *title);
void wm_set_shape(wm_window_t *win, const uint8_t *mask, uint16_t mw, uint16_t mh);

/**
 * Route a keyboard scancode. Returns true for every scancode.
 * Call explicitly; wm_compositor_tick() handles keyboard input separately.
 */
bool wm_dispatch_key(uint8_t scancode);

/**
 * Composite visible windows and the taskbar onto @backbuf. The cursor is
 * drawn separately.
 */
void wm_compose(uint8_t *backbuf);

/* Drawing helpers (operate on any uint8_t pixel buffer) */
void wm_buf_put_pixel   (uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, uint8_t color);
void wm_buf_draw_line   (uint8_t *buf, uint16_t bw, uint16_t bh, int x0, int y0, int x1, int y1, uint8_t color);
void wm_buf_fill_rect   (uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, int w, int h, uint8_t color);
void wm_buf_draw_rect   (uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, int w, int h, uint8_t color);
void wm_buf_draw_char   (uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, char c,         uint8_t color);
void wm_buf_draw_char_bg(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, char c,         uint8_t fg, uint8_t bg);
void wm_buf_draw_string (uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, const char *str, uint8_t color);
void wm_buf_draw_string_bg(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, const char *str, uint8_t fg, uint8_t bg);

/* Compositor tick (called from terminal_console each kernel loop iteration)
 *
 * Called by terminal_console() and other kernel/process paths. When
 * applicable, the tick processes input and composes/flips a frame; overlay
 * flags may hide the cursor or button. ESC/Q are not session-exit shortcuts,
 * and close-button behavior depends on mode.
 */
void wm_compositor_tick(void);
void wm_raw_gfx_overlay_tick(void);
void wm_terminal_putc(char c, uint8_t fg, uint8_t bg);

/* Raw graphics bridge (kernel-internal)
 * Used by vga_gfx / keyboard so that any raw graphics application running
 * in a shared workspace (`run a && b`) is shown in its own non-overlapping
 * window, with its own palette and its own keyboard queue.
 */
struct process;
wm_window_t *wm_ensure_raw_gfx_window(struct process *proc);
void         wm_present_raw_gfx_process(struct process *proc, const uint8_t *buf,
                                        int src_w, int src_h);
void         wm_raw_gfx_set_palette(struct process *proc, uint8_t idx,
                                    uint8_t r, uint8_t g, uint8_t b);
bool         wm_has_raw_gfx_window_for_pid(uint32_t pid);
void         wm_destroy_raw_gfx_window_for_pid(uint32_t pid);
uint8_t      wm_raw_gfx_get_scancode(struct process *proc);

/**
 * Get current display/compositor screen bounds in pixels.
 */
void wm_get_screen_bounds(int *out_w, int *out_h);

#endif /* LIB_WM_H */
