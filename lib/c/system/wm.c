/**
 * wm.c  —  Window Manager for IPO_OS
 *
 * Architecture
 * ============
 *
 * The WM compositor runs as an ASYNC TASK ("wm_session") that fires
 * every 16 ms inside the kernel's main loop:
 *
 *   for(;;) {
 *       async_scheduler_tick();    ← compositor fires here
 *       terminal_console();        ← skips keyboard when WM active
 *   }
 *
 * This means the terminal stays alive between compositor frames and apps
 * can be launched, add their windows, and exit immediately.
 *
 * Window creation
 * ===============
 * wm_create_window() does THREE things:
 *   1. Auto-starts the WM session if it isn't running yet.
 *   2. Allocates the window struct and kernel-heap framebuffer.
 *   3. Calls opts->draw_cb(win) ONCE (while the app's code is still in
 *      memory) to render the initial frame into the framebuffer.
 *      After that the draw_cb pointer is kept in win->draw_cb for
 *      wm_session_run() (single-app blocking mode), but the async
 *      compositor never calls it — it only blits the pre-rendered buffer.
 *
 * startx / stopx
 * ==============
 *   startx  → wm_session_start() — starts async compositor, returns.
 *   stopx   → wm_session_stop()  — stops compositor, restores text mode.
 *
 * Single-app blocking mode (for self-hosted event loops)
 * ======================================================
 *   wm_session_run() — enters VGA mode, runs compositor loop with
 *   per-frame draw_cb re-render (for animation), blocks until exit.
 *   Equivalent to the old architecture.  Use if you do NOT need other
 *   apps to run simultaneously.
 *
 * Keyboard routing while WM is active
 * =====================================
 * keyboard_set_app_input_mode(false) — scancodes stay in shell_queue.
 * terminal_console() checks wm_session_active() and skips its keyboard
 * read, leaving scancodes for the compositor task to consume.
 * This avoids any race between terminal and compositor.
 *
 * Exit button
 * ===========
 * A red [X Exit] button is always drawn in the bottom-left corner of the
 * screen.  Left-clicking it (or pressing ESC/Q) stops the session.
 */

#include <wm.h>
#include <vga_gfx.h>
#include <vga.h>
#include <memory/kmalloc.h>
#include <driver/input/keyboard.h>
#include <driver/input/keymap/keymap.h>
#include <driver/input/keymap/dynamic_keymap.h>
#include <driver/input/mouse.h>
#include <system/timer.h>
#include <system/state.h>
#include <ioport.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * Section 1: 6×8 bitmap font  (printable ASCII 0x20–0x7E)
 * ========================================================================= */
#define WM_FONT_W 6
#define WM_FONT_H 8

static const uint8_t wm_font[95][WM_FONT_H] = {
/*sp*/{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
/* ! */{0x20,0x20,0x20,0x20,0x00,0x20,0x00,0x00},
/* " */{0x50,0x50,0x00,0x00,0x00,0x00,0x00,0x00},
/* # */{0x50,0xF8,0x50,0x50,0xF8,0x50,0x00,0x00},
/* $ */{0x20,0x78,0xA0,0x70,0x28,0xF0,0x20,0x00},
/* % */{0xC0,0xC8,0x10,0x20,0x40,0x98,0x18,0x00},
/* & */{0x40,0xA0,0xA0,0x40,0xA8,0x90,0x68,0x00},
/* ' */{0x20,0x40,0x00,0x00,0x00,0x00,0x00,0x00},
/* ( */{0x10,0x20,0x40,0x40,0x40,0x20,0x10,0x00},
/* ) */{0x40,0x20,0x10,0x10,0x10,0x20,0x40,0x00},
/* * */{0x00,0x20,0xA8,0x70,0xA8,0x20,0x00,0x00},
/* + */{0x00,0x20,0x20,0xF8,0x20,0x20,0x00,0x00},
/* , */{0x00,0x00,0x00,0x00,0x00,0x20,0x40,0x00},
/* - */{0x00,0x00,0x00,0xF8,0x00,0x00,0x00,0x00},
/* . */{0x00,0x00,0x00,0x00,0x00,0x60,0x60,0x00},
/* / */{0x08,0x10,0x10,0x20,0x40,0x40,0x80,0x00},
/* 0 */{0x70,0x88,0x98,0xA8,0xC8,0x88,0x70,0x00},
/* 1 */{0x20,0x60,0x20,0x20,0x20,0x20,0x70,0x00},
/* 2 */{0x70,0x88,0x08,0x30,0x40,0x80,0xF8,0x00},
/* 3 */{0xF8,0x08,0x10,0x30,0x08,0x88,0x70,0x00},
/* 4 */{0x10,0x30,0x50,0x90,0xF8,0x10,0x10,0x00},
/* 5 */{0xF8,0x80,0xF0,0x08,0x08,0x88,0x70,0x00},
/* 6 */{0x38,0x40,0x80,0xF0,0x88,0x88,0x70,0x00},
/* 7 */{0xF8,0x08,0x10,0x20,0x20,0x40,0x40,0x00},
/* 8 */{0x70,0x88,0x88,0x70,0x88,0x88,0x70,0x00},
/* 9 */{0x70,0x88,0x88,0x78,0x08,0x10,0xE0,0x00},
/* : */{0x00,0x60,0x60,0x00,0x60,0x60,0x00,0x00},
/* ; */{0x00,0x20,0x00,0x20,0x20,0x40,0x00,0x00},
/* < */{0x10,0x20,0x40,0x80,0x40,0x20,0x10,0x00},
/* = */{0x00,0x00,0xF8,0x00,0xF8,0x00,0x00,0x00},
/* > */{0x80,0x40,0x20,0x10,0x20,0x40,0x80,0x00},
/* ? */{0x70,0x88,0x10,0x20,0x20,0x00,0x20,0x00},
/* @ */{0x70,0x88,0xA8,0xB8,0xB0,0x80,0x70,0x00},
/* A */{0x20,0x50,0x88,0x88,0xF8,0x88,0x88,0x00},
/* B */{0xF0,0x88,0x88,0xF0,0x88,0x88,0xF0,0x00},
/* C */{0x70,0x88,0x80,0x80,0x80,0x88,0x70,0x00},
/* D */{0xE0,0x90,0x88,0x88,0x88,0x90,0xE0,0x00},
/* E */{0xF8,0x80,0x80,0xF0,0x80,0x80,0xF8,0x00},
/* F */{0xF8,0x80,0x80,0xF0,0x80,0x80,0x80,0x00},
/* G */{0x70,0x88,0x80,0xB8,0x88,0x88,0x70,0x00},
/* H */{0x88,0x88,0x88,0xF8,0x88,0x88,0x88,0x00},
/* I */{0x70,0x20,0x20,0x20,0x20,0x20,0x70,0x00},
/* J */{0x38,0x10,0x10,0x10,0x10,0x90,0x60,0x00},
/* K */{0x88,0x90,0xA0,0xC0,0xA0,0x90,0x88,0x00},
/* L */{0x80,0x80,0x80,0x80,0x80,0x80,0xF8,0x00},
/* M */{0x88,0xD8,0xA8,0xA8,0x88,0x88,0x88,0x00},
/* N */{0x88,0xC8,0xA8,0x98,0x88,0x88,0x88,0x00},
/* O */{0x70,0x88,0x88,0x88,0x88,0x88,0x70,0x00},
/* P */{0xF0,0x88,0x88,0xF0,0x80,0x80,0x80,0x00},
/* Q */{0x70,0x88,0x88,0x88,0xA8,0x90,0x68,0x00},
/* R */{0xF0,0x88,0x88,0xF0,0xA0,0x90,0x88,0x00},
/* S */{0x70,0x88,0x80,0x70,0x08,0x88,0x70,0x00},
/* T */{0xF8,0x20,0x20,0x20,0x20,0x20,0x20,0x00},
/* U */{0x88,0x88,0x88,0x88,0x88,0x88,0x70,0x00},
/* V */{0x88,0x88,0x88,0x50,0x50,0x20,0x20,0x00},
/* W */{0x88,0x88,0x88,0xA8,0xA8,0xD8,0x88,0x00},
/* X */{0x88,0x88,0x50,0x20,0x50,0x88,0x88,0x00},
/* Y */{0x88,0x88,0x50,0x20,0x20,0x20,0x20,0x00},
/* Z */{0xF8,0x08,0x10,0x20,0x40,0x80,0xF8,0x00},
/* [ */{0x70,0x40,0x40,0x40,0x40,0x40,0x70,0x00},
/*  \*/{0x80,0x40,0x40,0x20,0x10,0x10,0x08,0x00},
/* ] */{0x70,0x10,0x10,0x10,0x10,0x10,0x70,0x00},
/* ^ */{0x20,0x50,0x88,0x00,0x00,0x00,0x00,0x00},
/* _ */{0x00,0x00,0x00,0x00,0x00,0x00,0xF8,0x00},
/* ` */{0x40,0x20,0x00,0x00,0x00,0x00,0x00,0x00},
/* a */{0x00,0x00,0x70,0x08,0x78,0x88,0x78,0x00},
/* b */{0x80,0x80,0xF0,0x88,0x88,0x88,0xF0,0x00},
/* c */{0x00,0x00,0x70,0x88,0x80,0x88,0x70,0x00},
/* d */{0x08,0x08,0x78,0x88,0x88,0x88,0x78,0x00},
/* e */{0x00,0x00,0x70,0x88,0xF8,0x80,0x70,0x00},
/* f */{0x30,0x48,0x40,0xF0,0x40,0x40,0x40,0x00},
/* g */{0x00,0x00,0x78,0x88,0x78,0x08,0x70,0x00},
/* h */{0x80,0x80,0xF0,0x88,0x88,0x88,0x88,0x00},
/* i */{0x20,0x00,0x60,0x20,0x20,0x20,0x70,0x00},
/* j */{0x10,0x00,0x30,0x10,0x10,0x90,0x60,0x00},
/* k */{0x80,0x80,0x90,0xA0,0xC0,0xA0,0x90,0x00},
/* l */{0x60,0x20,0x20,0x20,0x20,0x20,0x70,0x00},
/* m */{0x00,0x00,0xD0,0xA8,0xA8,0xA8,0x88,0x00},
/* n */{0x00,0x00,0xF0,0x88,0x88,0x88,0x88,0x00},
/* o */{0x00,0x00,0x70,0x88,0x88,0x88,0x70,0x00},
/* p */{0x00,0x00,0xF0,0x88,0x88,0xF0,0x80,0x00},
/* q */{0x00,0x00,0x78,0x88,0x88,0x78,0x08,0x00},
/* r */{0x00,0x00,0xB0,0xC8,0x80,0x80,0x80,0x00},
/* s */{0x00,0x00,0x78,0x80,0x70,0x08,0xF0,0x00},
/* t */{0x40,0x40,0xF0,0x40,0x40,0x48,0x30,0x00},
/* u */{0x00,0x00,0x88,0x88,0x88,0x88,0x78,0x00},
/* v */{0x00,0x00,0x88,0x88,0x50,0x50,0x20,0x00},
/* w */{0x00,0x00,0x88,0xA8,0xA8,0xD8,0x88,0x00},
/* x */{0x00,0x00,0x88,0x50,0x20,0x50,0x88,0x00},
/* y */{0x00,0x00,0x88,0x88,0x78,0x08,0x70,0x00},
/* z */{0x00,0x00,0xF8,0x10,0x20,0x40,0xF8,0x00},
/* { */{0x18,0x20,0x20,0x60,0x20,0x20,0x18,0x00},
/* | */{0x20,0x20,0x20,0x00,0x20,0x20,0x20,0x00},
/* } */{0xC0,0x20,0x20,0x30,0x20,0x20,0xC0,0x00},
/* ~ */{0x48,0x90,0x00,0x00,0x00,0x00,0x00,0x00},
};

/* =========================================================================
 * Section 2: Drawing helpers (buf = per-window framebuf or backbuf)
 * ========================================================================= */

void wm_buf_put_pixel(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, uint8_t c) {
    if (x<0||y<0||x>=(int)bw||y>=(int)bh) return;
    buf[(size_t)y*bw+x]=c;
}
void wm_buf_draw_line(uint8_t *buf, uint16_t bw, uint16_t bh, int x0, int y0, int x1, int y1, uint8_t c) {
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
    if(w<=0||h<=0) return;
    int x2=x+w, y2=y+h;
    if(x<0) x=0; if(y<0) y=0;
    if(x2>(int)bw) x2=(int)bw; if(y2>(int)bh) y2=(int)bh;
    if(x2<=x || y2<=y) return;
    for(int cy=y;cy<y2;cy++) memset(buf+(size_t)cy*bw+x,c,(size_t)(x2-x));
}
void wm_buf_draw_rect(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, int w, int h, uint8_t c) {
    if(w<=0||h<=0) return;
    for(int cx=x;cx<x+w;cx++){wm_buf_put_pixel(buf,bw,bh,cx,y,c);wm_buf_put_pixel(buf,bw,bh,cx,y+h-1,c);}
    for(int cy=y;cy<y+h;cy++){wm_buf_put_pixel(buf,bw,bh,x,cy,c);wm_buf_put_pixel(buf,bw,bh,x+w-1,cy,c);}
}
void wm_buf_draw_char(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, char ch, uint8_t c) {
    uint8_t u=(uint8_t)ch; if(u<0x20||u>0x7E) return;
    const uint8_t *g=wm_font[u-0x20];
    for(int row=0;row<WM_FONT_H;row++)
        for(int col=0;col<WM_FONT_W;col++)
            if(g[row]&(0x80u>>col)) wm_buf_put_pixel(buf,bw,bh,x+col,y+row,c);
}
void wm_buf_draw_string(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, const char *str, uint8_t c) {
    if(!str) return;
    int cx=x;
    while(*str){
        if(*str=='\n'){cx=x;y+=WM_FONT_H;}
        else{wm_buf_draw_char(buf,bw,bh,cx,y,*str,c);cx+=WM_FONT_W;}
        str++;
    }
}
#ifdef IPO_APP
/* =========================================================================
 * Application-side Window Manager API (Syscall stubs)
 * =========================================================================
 * Applications invoke the OS API to create and manage windows.
 * The kernel hosts the actual window structures, compositor, and VRAM.
 */
#include <syscall.h>

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

wm_window_t *wm_get_focused(void) {
    return (wm_window_t *)(uintptr_t)ipo_syscall(IPO_SYSCALL_WM_GET_FOCUSED, 0u, NULL);
}

void wm_set_focus(wm_window_t *win) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)win;
    ipo_syscall(IPO_SYSCALL_WM_SET_FOCUS, 1u, args);
}

void wm_focus_next(void) {
    ipo_syscall(IPO_SYSCALL_WM_FOCUS_NEXT, 0u, NULL);
}

void wm_focus_prev(void) {
    ipo_syscall(IPO_SYSCALL_WM_FOCUS_PREV, 0u, NULL);
}

void wm_invalidate(wm_window_t *win) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)win;
    ipo_syscall(IPO_SYSCALL_WM_INVALIDATE, 1u, args);
}

void wm_invalidate_all(void) {}
void wm_init(void) {}
void wm_set_fullscreen(wm_window_t *win, bool enable) { (void)win; (void)enable; }
void wm_minimize(wm_window_t *win) { (void)win; }
void wm_restore(wm_window_t *win) { (void)win; }
void wm_toggle_maximize(wm_window_t *win) { (void)win; }
void wm_move(wm_window_t *win, int16_t nx, int16_t ny) { (void)win; (void)nx; (void)ny; }
bool wm_resize(wm_window_t *win, uint16_t nw, uint16_t nh) { (void)win; (void)nw; (void)nh; return false; }
void wm_set_title(wm_window_t *win, const char *title) { (void)win; (void)title; }
void wm_set_shape(wm_window_t *win, const uint8_t *mask, uint16_t mw, uint16_t mh) { (void)win; (void)mask; (void)mw; (void)mh; }
bool wm_dispatch_key(uint8_t scancode) { (void)scancode; return true; }
void wm_compose(uint8_t *backbuf) { (void)backbuf; }
void wm_compositor_tick(void) {}
void wm_session_run(void) {}

wm_window_t *wm_register_window(const char *title, int16_t x, int16_t y, uint16_t w, uint16_t h,
                                  wm_draw_cb_t draw_cb, wm_event_cb_t event_cb, void *user_data) {
    wm_window_options_t opts = WM_WINDOW_OPTIONS_DEFAULT;
    opts.title=title; opts.x=x; opts.y=y; opts.w=w; opts.h=h;
    opts.draw_cb=draw_cb; opts.event_cb=event_cb; opts.user_data=user_data;
    return wm_create_window(&opts);
}
void wm_remove_window(wm_window_t *win) { wm_destroy_window(win); }

#else
/* =========================================================================
 * Kernel-side Window Manager Implementation
 * ========================================================================= */
#include <kernel/process.h>

/* =========================================================================
 * Section 3: Window list
 * ========================================================================= */
static wm_window_t *wm_head  = NULL;
static wm_window_t *wm_tail  = NULL;
static int          wm_count = 0;

static void list_append(wm_window_t *w) {
    w->prev=wm_tail; w->next=NULL;
    if(wm_tail) wm_tail->next=w; else wm_head=w;
    wm_tail=w; wm_count++;
}
static void list_remove(wm_window_t *w) {
    if(w->prev) w->prev->next=w->next; else wm_head=w->next;
    if(w->next) w->next->prev=w->prev; else wm_tail=w->prev;
    w->prev=w->next=NULL; wm_count--;
}
static void list_front(wm_window_t *w) {
    if(wm_tail==w) return; list_remove(w); list_append(w);
}

/* =========================================================================
 * Section 4: Session state
 * ========================================================================= */
static bool wm_sess_active = false;
static bool wm_prev_left   = false;
static bool wm_prev_right  = false;

/* Static 64 KB backbuffer in BSS (never freed, always available) */
static uint8_t wm_backbuf[VGA_GFX_SIZE];

/* =========================================================================
 * Section 5: Core window API
 * ========================================================================= */

void wm_init(void) { wm_head=wm_tail=NULL; wm_count=0; }

wm_window_t *wm_create_window(const wm_window_options_t *opts) {
    if (!opts) return NULL;

    /* Auto-start WM session if not already running */
    if (!wm_sess_active) wm_session_start();

    wm_window_t *win = kmalloc(sizeof(wm_window_t));
    if (!win) return NULL;
    memset(win, 0, sizeof(wm_window_t));

    if (opts->fullscreen) {
        win->x=0; win->y=0; win->w=VGA_GFX_WIDTH; win->h=VGA_GFX_HEIGHT; win->fullscreen=true;
    } else {
        win->x=opts->x; win->y=opts->y;
        win->w = opts->w>0 ? opts->w : 80;
        win->h = opts->h>0 ? opts->h : 50;
        if(win->x<0) win->x=0; if(win->y<0) win->y=0;
        if(win->x>=VGA_GFX_WIDTH)  win->x=(int16_t)(VGA_GFX_WIDTH-1);
        if(win->y>=VGA_GFX_HEIGHT) win->y=(int16_t)(VGA_GFX_HEIGHT-1);
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
    process_t *cur_proc = process_get_current();
    win->owner_pid    = cur_proc ? cur_proc->pid : 0;

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
        }
        /* if kmalloc fails, fall through with no mask (window still shows) */
    }

    size_t fsz = (size_t)win->w * win->h;
    if (opts->framebuf) {
        win->framebuf = opts->framebuf; win->framebuf_owned = false;
    } else {
        win->framebuf = kmalloc(fsz);
        if (!win->framebuf) { if(win->title) kfree(win->title); kfree(win); return NULL; }
        win->framebuf_owned = true;
        memset(win->framebuf, win->bg_color, fsz);
    }

    list_append(win);

    /*
     * Render initial frame NOW, while the calling app's code is still
     * in memory.  The async compositor will blit this framebuffer and
     * will NOT call draw_cb again — so there's no dangling-pointer risk
     * when the app exits after returning from wm_create_window().
     */
    if (win->draw_cb) {
        win->draw_cb(win);
    }

    return win;
}

static wm_window_t *drag_win = NULL;
static int drag_mode = 0;

void wm_destroy_window(wm_window_t *win) {
    if (!win) return;
    if (drag_win == win) {
        drag_win = NULL;
        drag_mode = 0;
    }
    if (win->event_cb && (win->owner_pid == 0 || process_is_alive(win->owner_pid))) {
        win->event_cb(win, WM_EVENT_CLOSE, 0);
    }
    list_remove(win);
    if (win->framebuf_owned && win->framebuf) kfree(win->framebuf);
    if (win->shape_mask_owned && win->shape_mask) kfree((void *)win->shape_mask);
    if (win->title) kfree(win->title);
    kfree(win);
}

int          wm_get_window_count(void) { return wm_count; }
wm_window_t *wm_get_focused(void)      { return wm_tail; }

void wm_set_focus(wm_window_t *win) {
    if (!win || wm_tail==win) return;
    wm_window_t *old=wm_tail;
    if (old && old->event_cb) old->event_cb(old, WM_EVENT_FOCUS_OUT, 0);
    list_front(win);
    if (win->event_cb) win->event_cb(win, WM_EVENT_FOCUS_IN, 0);
}
void wm_focus_next(void) { if(wm_head && wm_count>=2) wm_set_focus(wm_head); }
void wm_focus_prev(void) { if(wm_tail && wm_count>=2 && wm_tail->prev) wm_set_focus(wm_tail->prev); }

void wm_invalidate(wm_window_t *win) { if(win) win->dirty=true; }
void wm_invalidate_all(void)         { for(wm_window_t *w=wm_head;w;w=w->next) w->dirty=true; }

void wm_move(wm_window_t *win, int16_t nx, int16_t ny) {
    if (!win || win->fullscreen) return;
    if (nx<-(int16_t)win->w+8)         nx=-(int16_t)win->w+8;
    if (ny<0)                           ny=0;
    if (nx>(int16_t)(VGA_GFX_WIDTH-8))  nx=(int16_t)(VGA_GFX_WIDTH-8);
    if (ny>(int16_t)(VGA_GFX_HEIGHT-8)) ny=(int16_t)(VGA_GFX_HEIGHT-8);
    win->x=nx; win->y=ny;
    if (!win->maximized) {
        win->saved_x = nx;
        win->saved_y = ny;
    }
    if (win->event_cb) win->event_cb(win, WM_EVENT_MOVE, 0);
}

bool wm_resize(wm_window_t *win, uint16_t nw, uint16_t nh) {
    if (!win || win->fullscreen || !nw || !nh) return false;
    if (win->w == nw && win->h == nh) return true;

    /* Dynamic nearest-neighbor 2D scaling of framebuffer */
    if (win->framebuf_owned && win->framebuf && win->w > 0 && win->h > 0) {
        uint8_t *nb = kmalloc((size_t)nw * nh);
        if (!nb) return false;
        uint16_t old_w = win->w;
        uint16_t old_h = win->h;
        for (uint16_t dy = 0; dy < nh; dy++) {
            uint16_t sy = (uint16_t)(((uint32_t)dy * old_h) / nh);
            size_t dst_row = (size_t)dy * nw;
            size_t src_row = (size_t)sy * old_w;
            for (uint16_t dx = 0; dx < nw; dx++) {
                uint16_t sx = (uint16_t)(((uint32_t)dx * old_w) / nw);
                nb[dst_row + dx] = win->framebuf[src_row + sx];
            }
        }
        kfree(win->framebuf);
        win->framebuf = nb;
    } else if (win->framebuf_owned) {
        uint8_t *nb = kmalloc((size_t)nw * nh);
        if (!nb) return false;
        memset(nb, win->bg_color, (size_t)nw * nh);
        if (win->framebuf) kfree(win->framebuf);
        win->framebuf = nb;
    }

    /* Scale shape mask if window has custom shape */
    if (win->shape_mask && win->shape_mask_owned && win->mask_w > 0 && win->mask_h > 0) {
        uint8_t *nm = kmalloc((size_t)nw * nh);
        if (nm) {
            uint16_t old_mw = win->mask_w;
            uint16_t old_mh = win->mask_h;
            for (uint16_t dy = 0; dy < nh; dy++) {
                uint16_t sy = (uint16_t)(((uint32_t)dy * old_mh) / nh);
                size_t dst_row = (size_t)dy * nw;
                size_t src_row = (size_t)sy * old_mw;
                for (uint16_t dx = 0; dx < nw; dx++) {
                    uint16_t sx = (uint16_t)(((uint32_t)dx * old_mw) / nw);
                    nm[dst_row + dx] = win->shape_mask[src_row + sx];
                }
            }
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
    if (win->event_cb) win->event_cb(win, WM_EVENT_RESIZE, 0);
    return true;
}

void wm_minimize(wm_window_t *win) {
    if (!win) return;
    win->minimized = true;
    for (wm_window_t *w = wm_tail; w; w = w->prev) {
        if (!w->minimized) {
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
        win->x = win->saved_x;
        win->y = win->saved_y;
        win->maximized = false;
        wm_resize(win, win->saved_w, win->saved_h);
    } else {
        win->saved_x = win->x;
        win->saved_y = win->y;
        win->saved_w = win->w;
        win->saved_h = win->h;
        win->x = 2;
        win->y = 13;
        win->maximized = true;
        wm_resize(win, 316, 171);
    }
}


void wm_set_title(wm_window_t *win, const char *title) {
    if (!win) return;
    if (win->title) { kfree(win->title); win->title=NULL; }
    if (title && *title) {
        size_t tl=strlen(title);
        win->title=kmalloc(tl+1);
        if (win->title) memcpy(win->title, title, tl+1);
    }
}

void wm_set_shape(wm_window_t *win, const uint8_t *mask, uint16_t mw, uint16_t mh) {
    if (!win) return;
    win->shape_mask=mask; win->mask_w=mw; win->mask_h=mh;
}

void wm_set_fullscreen(wm_window_t *win, bool enable) {
    if (!win || win->fullscreen==enable) return;
    win->fullscreen=enable;
    if (enable) {
        win->x=0; win->y=0;
        if (win->framebuf_owned) {
            uint8_t *nb=kmalloc(VGA_GFX_SIZE);
            if (nb) { if(win->framebuf) kfree(win->framebuf); win->framebuf=nb; memset(nb,win->bg_color,VGA_GFX_SIZE); }
        }
        win->w=VGA_GFX_WIDTH; win->h=VGA_GFX_HEIGHT;
    }
    if (win->event_cb) win->event_cb(win, WM_EVENT_RESIZE, 0);
}

/* =========================================================================
 * Section 6: Windows 95 Colors and 3D primitives
 * ========================================================================= */
#define TITLEBAR_H   11
#define TASKBAR_Y    186
#define TASKBAR_H    14

#define WIN_CLR_DESKTOP        3    /* Windows 95 Teal / Cyan */
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

/* =========================================================================
 * Section 7: Mouse cursor (white arrow, black outline)
 * ========================================================================= */
static const uint8_t wm_cursor[8] = {
    0b10000000, 0b11000000, 0b11100000, 0b11110000,
    0b11111000, 0b11100000, 0b10100000, 0b00000000,
};

static void draw_cursor(uint8_t *bb, int cx, int cy) {
    static const int dr[4]={-1,1,0,0}, dc[4]={0,0,-1,1};
    for (int row=0;row<8;row++)
        for (int col=0;col<5;col++)
            if (wm_cursor[row]&(0x80u>>col)) {
                for (int k=0;k<4;k++) {
                    int nr=row+dr[k], nc=col+dc[k];
                    bool in=nr>=0&&nr<8&&nc>=0&&nc<5&&(wm_cursor[nr]&(0x80u>>nc));
                    if (!in) {
                        int px=cx+col+dc[k], py=cy+row+dr[k];
                        if(px>=0&&px<VGA_GFX_WIDTH&&py>=0&&py<VGA_GFX_HEIGHT)
                            bb[(size_t)py*VGA_GFX_WIDTH+px]=0;
                    }
                }
            }
    for (int row=0;row<8;row++)
        for (int col=0;col<5;col++)
            if (wm_cursor[row]&(0x80u>>col)) {
                int px=cx+col, py=cy+row;
                if(px>=0&&px<VGA_GFX_WIDTH&&py>=0&&py<VGA_GFX_HEIGHT)
                    bb[(size_t)py*VGA_GFX_WIDTH+px]=15;
            }
}

/* =========================================================================
 * Section 8: Window decorations, Taskbar and Wallpaper
 * ========================================================================= */
static void draw_decorations(wm_window_t *win, uint8_t *bb) {
    if (win->fullscreen || win->minimized || win->decor_style==WM_DECOR_NONE || win->decor_style==WM_DECOR_CUSTOM) return;
    bool focused = (wm_tail==win);

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
        int avail_w = (int)win->w - 32;
        int max_chars = avail_w > 0 ? avail_w / 6 : 0;
        if (max_chars > 0) {
            size_t title_len = strlen(win->title);
            if (max_chars > 60) max_chars = 60;
            char disp_buf[64];
            if (title_len <= (size_t)max_chars) {
                strncpy(disp_buf, win->title, (size_t)max_chars);
                disp_buf[max_chars] = '\0';
            } else {
                const char sep[] = "   *   ";
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

    if (!(reg_b & 0x02) && (hr & 0x80)) {
        hr = (uint8_t)(((hr & 0x7F) + 12) % 24);
    }

    *out_h = (int)(hr % 24);
    *out_m = (int)(min % 60);
}

static int taskbar_scroll_offset = 0;
static uint32_t wm_lang_offset = 0;
static uint32_t wm_lang_last_tick = 0;

static void draw_taskbar(uint8_t *bb) {
    /* 3D raised taskbar across the bottom */
    wm_buf_fill_rect(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, 0, TASKBAR_Y, VGA_GFX_WIDTH, TASKBAR_H, WIN_CLR_FACE);
    wm_buf_draw_line(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, 0, TASKBAR_Y, VGA_GFX_WIDTH - 1, TASKBAR_Y, WIN_CLR_HILIGHT);

    /* Start / Exit button on the left */
    draw_3d_box(bb, 2, TASKBAR_Y + 2, 40, 10, false);
    wm_buf_fill_rect(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, 4, TASKBAR_Y + 4, 6, 6, 4);
    wm_buf_draw_string(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, 5, TASKBAR_Y + 3, "x", 15);
    wm_buf_draw_string(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, 12, TASKBAR_Y + 3, "Exit", WIN_CLR_DKSHADOW);

    /* Left scroll button '<' */
    draw_3d_box(bb, 44, TASKBAR_Y + 2, 9, 10, false);
    wm_buf_draw_string(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, 46, TASKBAR_Y + 3, "<", WIN_CLR_DKSHADOW);

    /* Window tabs between x = 54 and x = 182 (up to 3 visible tabs, width 40) */
    int win_idx = 0;
    int visible_tab = 0;
    int tab_w = 40;
    int tab_h = 10;
    for (wm_window_t *w = wm_head; w; w = w->next, win_idx++) {
        if (win_idx < taskbar_scroll_offset) continue;
        if (visible_tab >= 3) break;

        int tx = 54 + visible_tab * 43;
        int ty = TASKBAR_Y + 2;
        bool is_active = (w == wm_tail && !w->minimized);

        if (is_active) {
            draw_3d_box(bb, tx, ty, tab_w, tab_h, true);
            wm_buf_fill_rect(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, tx + 1, ty + 1, tab_w - 2, tab_h - 2, WIN_CLR_ACTIVE_TITLE);
        } else {
            draw_3d_box(bb, tx, ty, tab_w, tab_h, false);
        }

        char title_buf[8];
        const char *src_title = w->title ? w->title : "Window";
        if (w->minimized) {
            title_buf[0] = '_';
            strncpy(title_buf + 1, src_title, 5);
            title_buf[6] = '\0';
        } else {
            strncpy(title_buf, src_title, 6);
            title_buf[6] = '\0';
        }
        uint8_t text_color = is_active ? WIN_CLR_ACTIVE_TEXT : (w->minimized ? WIN_CLR_SHADOW : WIN_CLR_DKSHADOW);
        wm_buf_draw_string(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, tx + 2, ty + 3, title_buf, text_color);

        visible_tab++;
    }

    /* Right scroll button '>' */
    draw_3d_box(bb, 184, TASKBAR_Y + 2, 9, 10, false);
    wm_buf_draw_string(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, 186, TASKBAR_Y + 3, ">", WIN_CLR_DKSHADOW);

    /* Language box in tray (x = 195 .. 279, width 85) */
    draw_3d_box(bb, 195, TASKBAR_Y + 2, 85, 10, true);
    const char *lang_name = dynamic_keymap_get_name();
    if (!lang_name || lang_name[0] == '\0') {
        lang_name = "English (US)";
    }
    size_t lang_len = strlen(lang_name);
    int vis_chars = 13; // (85 - 4) / 6 = 13 characters
    char disp_buf[16];

    if ((int)lang_len <= vis_chars) {
        strncpy(disp_buf, lang_name, sizeof(disp_buf) - 1);
        disp_buf[sizeof(disp_buf) - 1] = '\0';
    } else {
        const char sep[] = "   *   ";
        size_t sep_len = strlen(sep);
        size_t loop_len = lang_len + sep_len;
        uint32_t now = timer_millis();
        if (now - wm_lang_last_tick >= 180u) {
            wm_lang_last_tick = now;
            wm_lang_offset = (wm_lang_offset + 1u) % (uint32_t)loop_len;
        }
        for (int c = 0; c < vis_chars; c++) {
            size_t idx = (wm_lang_offset + (size_t)c) % loop_len;
            if (idx < lang_len) {
                disp_buf[c] = lang_name[idx];
            } else {
                disp_buf[c] = sep[idx - lang_len];
            }
        }
        disp_buf[vis_chars] = '\0';
    }
    wm_buf_draw_string(bb, VGA_GFX_WIDTH, VGA_GFX_HEIGHT, 198, TASKBAR_Y + 3, disp_buf, WIN_CLR_DKSHADOW);

    /* System Tray Clock (Real CMOS RTC time from hardware processor) (x = 282 .. 317, width 36) */
    draw_3d_box(bb, 282, TASKBAR_Y + 2, 36, 10, true);
    int rtc_h = 0, rtc_m = 0;
    wm_get_rtc_time(&rtc_h, &rtc_m);
    char clock_buf[8];
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

    /* 2. Windows (back to front) */
    for (wm_window_t *win = wm_head; win; win = win->next) {
        if (win->minimized) continue;
        for (int row = 0; row < (int)win->h; row++) {
            int sy = (int)win->y + row;
            if (sy < 0 || sy >= TASKBAR_Y) continue;
            for (int col = 0; col < (int)win->w; col++) {
                int sx = (int)win->x + col;
                if (sx < 0 || sx >= VGA_GFX_WIDTH) continue;
                if (win->shape_mask && win->mask_w == win->w && win->mask_h == win->h) {
                    if (!win->shape_mask[(size_t)row * win->w + col]) continue;
                }
                backbuf[(size_t)sy * VGA_GFX_WIDTH + sx] = win->framebuf[(size_t)row * win->w + col];
            }
        }
        draw_decorations(win, backbuf);
    }

    /* 4. Taskbar at bottom */
    draw_taskbar(backbuf);
}

/* =========================================================================
 * Section 9: Keyboard dispatch (for wm_session_run blocking mode)
 * ========================================================================= */
bool wm_dispatch_key(uint8_t scancode) {
    bool is_break=(scancode&0x80)!=0;
    if (is_break) return true;
    uint8_t make=scancode&0x7F;
    if (make==0x0F) {
        if(keyboard_is_shift_pressed()) wm_focus_prev(); else wm_focus_next();
        return true;
    }
    if ((keyboard_is_alt_pressed() && make == 0x3E /* F4 */) ||
        (keyboard_is_ctrl_pressed() && (make == 0x11 /* W */ || make == 0x10 /* Q */))) {
        wm_window_t *fw = wm_get_focused();
        if (fw) {
            wm_destroy_window(fw);
        }
        return true;
    }
    if (keyboard_is_ctrl_pressed()) {
        wm_window_t *fw=wm_get_focused();
        if (fw) {
            if(make==0x48){wm_move(fw,fw->x,(int16_t)(fw->y-4));return true;}
            if(make==0x50){wm_move(fw,fw->x,(int16_t)(fw->y+4));return true;}
            if(make==0x4B){wm_move(fw,(int16_t)(fw->x-4),fw->y);return true;}
            if(make==0x4D){wm_move(fw,(int16_t)(fw->x+4),fw->y);return true;}
        }
    }
    wm_window_t *fw=wm_get_focused();
    if (fw && fw->event_cb) fw->event_cb(fw, WM_EVENT_KEY_DOWN, (uint32_t)scancode);
    return true;
}

/* =========================================================================
 * Section 10: Compositor tick and Mouse interactions
 * ========================================================================= */
enum {
    DRAG_NONE = 0,
    DRAG_MOVE,
    DRAG_RESIZE_BR,
    DRAG_RESIZE_R,
    DRAG_RESIZE_B
};
static int16_t drag_offset_x = 0, drag_offset_y = 0;
static uint16_t drag_start_w = 0, drag_start_h = 0;
static int32_t drag_start_mx = 0, drag_start_my = 0;

void wm_compositor_tick(void) {
    if (!wm_sess_active) return;

    /* Keyboard */
    uint8_t sc;
    while ((sc = keyboard_get_scancode()) != 0) {
        update_hot_key_state(sc);
        bool is_break=(sc&0x80)!=0;
        uint8_t make=sc&0x7F;
        if (!is_break) {
            if (make == 0x3C) { /* F2 key */
                dynamic_keymap_cycle_next();
            } else if (make == 0x0F) { /* Tab / Shift+Tab */
                if(keyboard_is_shift_pressed()) wm_focus_prev(); else wm_focus_next();
            } else if ((keyboard_is_alt_pressed() && make == 0x3E /* F4 */) ||
                       (keyboard_is_ctrl_pressed() && (make == 0x11 /* W */ || make == 0x10 /* Q */))) {
                /* Close focused window */
                wm_window_t *fw = wm_get_focused();
                if (fw) {
                    wm_destroy_window(fw);
                }
            } else if (keyboard_is_ctrl_pressed()) {
                wm_window_t *fw=wm_get_focused();
                if (fw) {
                    if(make==0x48) wm_move(fw,fw->x,(int16_t)(fw->y-4));
                    else if(make==0x50) wm_move(fw,fw->x,(int16_t)(fw->y+4));
                    else if(make==0x4B) wm_move(fw,(int16_t)(fw->x-4),fw->y);
                    else if(make==0x4D) wm_move(fw,(int16_t)(fw->x+4),fw->y);
                }
            } else {
                wm_window_t *fw=wm_get_focused();
                if (fw && fw->event_cb && (fw->owner_pid == 0 || process_is_alive(fw->owner_pid))) {
                    fw->event_cb(fw, WM_EVENT_KEY_DOWN, (uint32_t)sc);
                }
            }
        }
    }
    if (!wm_sess_active) return;

    /* Mouse */
    mouse_state_t ms;
    mouse_get_state(&ms);
    bool left_now = ms.left_button;
    bool right_now = ms.right_button;

    if (right_now && !wm_prev_right) {
        if (ms.y >= TASKBAR_Y) {
            /* 1. Window tabs right-click (ПКМ - close clicked window directly!) */
            if (ms.x >= 54 && ms.x <= 182) {
                int rel_x = (int)ms.x - 54;
                int tab_idx = rel_x / 43;
                if (rel_x % 43 <= 40) {
                    int target_idx = taskbar_scroll_offset + tab_idx;
                    int cur_idx = 0;
                    for (wm_window_t *w = wm_head; w; w = w->next, cur_idx++) {
                        if (cur_idx == target_idx) {
                            wm_destroy_window(w);
                            if (taskbar_scroll_offset > 0 && taskbar_scroll_offset >= wm_count) {
                                taskbar_scroll_offset = wm_count > 0 ? wm_count - 1 : 0;
                            }
                            break;
                        }
                    }
                }
            }
            /* 2. Language box right click (ПКМ - previous layout) */
            else if (ms.x >= 195 && ms.x <= 279) {
                dynamic_keymap_cycle_prev();
            }
        }
    }
    wm_prev_right = right_now;

    if (left_now && !wm_prev_left) {
        bool handled = false;

        /* 1. Check Taskbar */
        if (ms.y >= TASKBAR_Y) {
            /* Start / Exit button */
            if (ms.x >= 2 && ms.x <= 42) {
                wm_session_stop();
                return;
            }
            /* Scroll left '<' */
            if (ms.x >= 44 && ms.x <= 53) {
                if (taskbar_scroll_offset > 0) taskbar_scroll_offset--;
                handled = true;
            }
            /* Scroll right '>' */
            if (ms.x >= 184 && ms.x <= 193) {
                if (taskbar_scroll_offset + 3 < wm_count) taskbar_scroll_offset++;
                handled = true;
            }
            /* Window tabs */
            if (!handled && ms.x >= 54 && ms.x <= 182) {
                int rel_x = (int)ms.x - 54;
                int tab_idx = rel_x / 43;
                if (rel_x % 43 <= 40) {
                    int target_idx = taskbar_scroll_offset + tab_idx;
                    int cur_idx = 0;
                    for (wm_window_t *w = wm_head; w; w = w->next, cur_idx++) {
                        if (cur_idx == target_idx) {
                            if (w->minimized) {
                                w->minimized = false;
                                wm_set_focus(w);
                            } else if (w != wm_tail) {
                                wm_set_focus(w);
                            } else {
                                /* Already active and focused -> minimize */
                                w->minimized = true;
                                for (wm_window_t *pw = wm_tail; pw; pw = pw->prev) {
                                    if (!pw->minimized) {
                                        wm_set_focus(pw);
                                        break;
                                    }
                                }
                            }
                            handled = true;
                            break;
                        }
                    }
                }
            }
            /* Language box click (ЛКМ - next layout) */
            if (!handled && ms.x >= 195 && ms.x <= 279) {
                dynamic_keymap_cycle_next();
                handled = true;
            }
            handled = true;
        }

        /* 2. Check Windows (top to bottom) */
        if (!handled) {
            for (wm_window_t *w = wm_tail; w; w = w->prev) {
                if (w->minimized) continue;

                int wx0 = (int)w->x - 2;
                int wy0 = (int)w->y - TITLEBAR_H - 2;
                int wx1 = (int)w->x + (int)w->w + 2;
                int wy1 = (int)w->y + (int)w->h + 2;

                if (ms.x >= wx0 && ms.x <= wx1 && ms.y >= wy0 && ms.y <= wy1) {
                    wm_set_focus(w);
                    handled = true;

                    if (w->decor_style == WM_DECOR_DEFAULT) {
                        int btn_y = (int)w->y - TITLEBAR_H + 1;

                        /* Close button [X] */
                        int bx_close = (int)w->x + (int)w->w - 9;
                        if (ms.x >= bx_close && ms.x <= bx_close + 8 && ms.y >= btn_y && ms.y <= btn_y + 8) {
                            wm_destroy_window(w);
                            break;
                        }

                        /* Maximize button [^] */
                        int bx_max = (int)w->x + (int)w->w - 18;
                        if (ms.x >= bx_max && ms.x <= bx_max + 8 && ms.y >= btn_y && ms.y <= btn_y + 8) {
                            wm_toggle_maximize(w);
                            break;
                        }

                        /* Minimize button [_] */
                        int bx_min = (int)w->x + (int)w->w - 27;
                        if (ms.x >= bx_min && ms.x <= bx_min + 8 && ms.y >= btn_y && ms.y <= btn_y + 8) {
                            wm_minimize(w);
                            break;
                        }

                        /* Bottom-right corner resize */
                        if (ms.x >= (int)w->x + (int)w->w - 6 && ms.y >= (int)w->y + (int)w->h - 6) {
                            drag_mode = DRAG_RESIZE_BR;
                            drag_win = w;
                            drag_start_w = w->w;
                            drag_start_h = w->h;
                            drag_start_mx = ms.x;
                            drag_start_my = ms.y;
                            break;
                        }

                        /* Right border resize */
                        if (ms.x >= (int)w->x + (int)w->w - 2) {
                            drag_mode = DRAG_RESIZE_R;
                            drag_win = w;
                            drag_start_w = w->w;
                            drag_start_h = w->h;
                            drag_start_mx = ms.x;
                            drag_start_my = ms.y;
                            break;
                        }

                        /* Bottom border resize */
                        if (ms.y >= (int)w->y + (int)w->h - 2) {
                            drag_mode = DRAG_RESIZE_B;
                            drag_win = w;
                            drag_start_w = w->w;
                            drag_start_h = w->h;
                            drag_start_mx = ms.x;
                            drag_start_my = ms.y;
                            break;
                        }

                        /* Title bar drag to move */
                        if (ms.y < (int)w->y) {
                            drag_mode = DRAG_MOVE;
                            drag_win = w;
                            drag_offset_x = (int16_t)(ms.x - w->x);
                            drag_offset_y = (int16_t)(ms.y - w->y);
                            drag_start_mx = ms.x;
                            drag_start_my = ms.y;
                            break;
                        }
                    }

                    /* Client area click */
                    if (w->event_cb && (w->owner_pid == 0 || process_is_alive(w->owner_pid))) {
                        w->event_cb(w, WM_EVENT_CLICK, 0);
                    }
                    break;
                }
            }
        }

        /* Desktop Exit button removed - only single Start/Exit button on taskbar */
    } else if (left_now && drag_win != NULL) {
        /* Mouse drag in progress */
        if (drag_mode == DRAG_MOVE) {
            int dx = (int)ms.x - (int)drag_start_mx;
            int dy = (int)ms.y - (int)drag_start_my;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (drag_win->maximized && (dx > 3 || dy > 3)) {
                drag_win->maximized = false;
                wm_resize(drag_win, drag_win->saved_w, drag_win->saved_h);
            }
            int16_t nx = (int16_t)(ms.x - drag_offset_x);
            int16_t ny = (int16_t)(ms.y - drag_offset_y);
            if (ny < 13) ny = 13;
            if (ny > 175) ny = 175;
            if (nx < -(int16_t)drag_win->w + 30) nx = -(int16_t)drag_win->w + 30;
            if (nx > (int16_t)(VGA_GFX_WIDTH - 30)) nx = (int16_t)(VGA_GFX_WIDTH - 30);
            wm_move(drag_win, nx, ny);
        } else if (drag_mode == DRAG_RESIZE_BR) {
            int nw = (int)drag_start_w + ((int)ms.x - (int)drag_start_mx);
            int nh = (int)drag_start_h + ((int)ms.y - (int)drag_start_my);
            if (nw < 40) nw = 40; if (nw > 316) nw = 316;
            if (nh < 20) nh = 20; if (nh > 165) nh = 165;
            drag_win->maximized = false;
            wm_resize(drag_win, (uint16_t)nw, (uint16_t)nh);
        } else if (drag_mode == DRAG_RESIZE_R) {
            int nw = (int)drag_start_w + ((int)ms.x - (int)drag_start_mx);
            if (nw < 40) nw = 40; if (nw > 316) nw = 316;
            drag_win->maximized = false;
            wm_resize(drag_win, (uint16_t)nw, drag_win->h);
        } else if (drag_mode == DRAG_RESIZE_B) {
            int nh = (int)drag_start_h + ((int)ms.y - (int)drag_start_my);
            if (nh < 20) nh = 20; if (nh > 165) nh = 165;
            drag_win->maximized = false;
            wm_resize(drag_win, drag_win->w, (uint16_t)nh);
        }
    } else if (!left_now && wm_prev_left) {
        if (drag_win && (drag_mode == DRAG_RESIZE_BR || drag_mode == DRAG_RESIZE_R || drag_mode == DRAG_RESIZE_B)) {
            drag_win->saved_w = drag_win->w;
            drag_win->saved_h = drag_win->h;
            drag_win->saved_x = drag_win->x;
            drag_win->saved_y = drag_win->y;
            drag_win->maximized = false;
        }
        drag_mode = DRAG_NONE;
        drag_win = NULL;
    }

    wm_prev_left = left_now;

    /* Compose & flip */
    wm_compose(wm_backbuf);
    draw_cursor(wm_backbuf, (int)ms.x, (int)ms.y);
    vga_gfx_flip(wm_backbuf);
}

/* =========================================================================
 * Section 11: Session API
 * ========================================================================= */

bool wm_session_active(void) { return wm_sess_active; }

/** wm_session_start — start async compositor, return immediately. */
void wm_session_start(void) {
    if (wm_sess_active) return;
    vga_set_mode_13h_hardware();
    vga_gfx_init_default_palette();
    mouse_set_bounds(VGA_GFX_WIDTH, VGA_GFX_HEIGHT);
    keyboard_flush_queue();
    keyboard_flush_hardware();
    wm_sess_active = true;
    wm_prev_left   = false;
    drag_mode      = DRAG_NONE;
    drag_win       = NULL;
    taskbar_scroll_offset = 0;

    /* Draw initial frame */
    wm_compose(wm_backbuf);
    vga_gfx_flip(wm_backbuf);
}

/** wm_session_stop — stop compositor, destroy all windows, restore text mode. */
void wm_session_stop(void) {
    if (!wm_sess_active) return;
    wm_sess_active = false;
    drag_mode      = DRAG_NONE;
    drag_win       = NULL;
    taskbar_scroll_offset = 0;
    while (wm_head) wm_destroy_window(wm_head);
    keyboard_flush_queue();
    vga_set_mode_text_hardware();
    dynamic_keymap_reapply_fonts();
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
            if (w->draw_cb) w->draw_cb(w);
        }
        wm_compositor_tick();
    }
}


/* =========================================================================
 * Section 12: Legacy shim
 * ========================================================================= */
wm_window_t *wm_register_window(const char *title, int16_t x, int16_t y, uint16_t w, uint16_t h,
                                  wm_draw_cb_t draw_cb, wm_event_cb_t event_cb, void *user_data) {
    wm_window_options_t opts = WM_WINDOW_OPTIONS_DEFAULT;
    opts.title=title; opts.x=x; opts.y=y; opts.w=w; opts.h=h;
    opts.draw_cb=draw_cb; opts.event_cb=event_cb; opts.user_data=user_data;
    return wm_create_window(&opts);
}
void wm_remove_window(wm_window_t *win) { wm_destroy_window(win); }
#endif
