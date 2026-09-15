/**
 * fps_meter.c — Real-Time FPS and System Performance Monitor for IPO_OS
 *
 * Displays live frame rate, frame render latencies, rolling histogram,
 * active window count, and system uptime in a dedicated WM desktop window.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include <wm.h>
#include <syscall.h>
#include <system/state.h>
#include <system/timer.h>

#define WIN_W 220
#define WIN_H 120
#define HIST_LEN 96

#define CLR_BLACK         0
#define CLR_BLUE          1
#define CLR_GREEN         2
#define CLR_CYAN          3
#define CLR_RED           4
#define CLR_MAGENTA       5
#define CLR_BROWN         6
#define CLR_LIGHT_GRAY    7
#define CLR_DARK_GRAY     8
#define CLR_LIGHT_BLUE    9
#define CLR_LIGHT_GREEN  10
#define CLR_LIGHT_CYAN   11
#define CLR_LIGHT_RED    12
#define CLR_LIGHT_MAGENTA 13
#define CLR_YELLOW       14
#define CLR_WHITE        15

static volatile bool running = true;
static volatile bool reset_requested = false;

static void on_window_event(wm_window_t *w, uint32_t event, uint32_t data) {
    (void)w;
    if (event == WM_EVENT_CLOSE) {
        running = false;
    } else if (event == WM_EVENT_KEY_DOWN) {
        uint8_t sc = (uint8_t)(data & 0x7F);
        if (sc == 1) { /* Esc */
            running = false;
        } else if (sc == 19) { /* 'R' key */
            reset_requested = true;
        }
    } else if (event == WM_EVENT_CLICK) {
        /* Click anywhere to reset stats */
        reset_requested = true;
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("[fps_meter] Launching System Performance Monitor...\n");

    /* Ensure WM session is running */
    if (!wm_session_active()) {
        wm_session_start();
    }

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = "Performance Monitor";
    opt.x = 90;
    opt.y = 35;
    opt.w = WIN_W;
    opt.h = WIN_H;
    opt.event_cb = on_window_event;

    wm_window_t *win = wm_create_window(&opt);
    if (!win) {
        printf("[fps_meter] Failed to create window\n");
        return 1;
    }

    uint16_t history[HIST_LEN];
    memset(history, 0, sizeof(history));
    int hist_head = 0;

    uint32_t frame_count = 0;
    uint32_t stats_start_time = timer_millis();
    uint32_t last_frame_time = timer_millis();

    float cur_fps = 0.0f;
    float avg_fps = 0.0f;
    float min_fps = 999.0f;
    float max_fps = 0.0f;
    uint32_t cur_dt = 33;

    while (running && wm_session_active() && !system_is_interrupted() && wm_is_window_valid(win)) {
        uint32_t loop_start = timer_millis();

        /* Calculate delta time */
        cur_dt = loop_start - last_frame_time;
        last_frame_time = loop_start;
        if (cur_dt == 0) cur_dt = 1;

        /* Add to history */
        history[hist_head] = (uint16_t)cur_dt;
        hist_head = (hist_head + 1) % HIST_LEN;

        /* Calculate stats */
        frame_count++;
        cur_fps = 1000.0f / (float)cur_dt;
        if (cur_fps > 999.0f) cur_fps = 999.0f;

        uint32_t total_elapsed = loop_start - stats_start_time;
        if (total_elapsed >= 500) {
            avg_fps = ((float)frame_count * 1000.0f) / (float)total_elapsed;
            if (frame_count > 5) {
                if (cur_fps < min_fps) min_fps = cur_fps;
                if (cur_fps > max_fps) max_fps = cur_fps;
            }
        } else {
            avg_fps = cur_fps;
        }

        if (reset_requested) {
            reset_requested = false;
            frame_count = 0;
            stats_start_time = loop_start;
            min_fps = cur_fps;
            max_fps = cur_fps;
            avg_fps = cur_fps;
            memset(history, 0, sizeof(history));
        }

        /* Render UI */
        uint8_t *fb = win->framebuf;
        if (fb) {
            /* Background: dark theme */
            wm_buf_fill_rect(fb, WIN_W, WIN_H, 0, 0, WIN_W, WIN_H, CLR_BLACK);

            /* Top info bar */
            char buf[64];
            uint8_t fps_col = (cur_fps >= 28.0f) ? CLR_LIGHT_GREEN : ((cur_fps >= 18.0f) ? CLR_YELLOW : CLR_LIGHT_RED);

            snprintf(buf, sizeof(buf), "FPS: %d.%d", (int)cur_fps, (int)(cur_fps * 10.0f) % 10);
            wm_buf_draw_string(fb, WIN_W, WIN_H, 8, 4, buf, fps_col);

            snprintf(buf, sizeof(buf), "dt: %u ms", (unsigned int)cur_dt);
            wm_buf_draw_string(fb, WIN_W, WIN_H, 95, 4, buf, CLR_LIGHT_CYAN);

            snprintf(buf, sizeof(buf), "Avg: %d.%d", (int)avg_fps, (int)(avg_fps * 10.0f) % 10);
            wm_buf_draw_string(fb, WIN_W, WIN_H, 155, 4, buf, CLR_WHITE);

            /* Line 2: Min / Max / Windows count */
            int win_cnt = wm_get_window_count();
            snprintf(buf, sizeof(buf), "Min: %d.%d", (int)min_fps, (int)(min_fps * 10.0f) % 10);
            wm_buf_draw_string(fb, WIN_W, WIN_H, 8, 14, buf, CLR_LIGHT_GRAY);

            snprintf(buf, sizeof(buf), "Max: %d.%d", (int)max_fps, (int)(max_fps * 10.0f) % 10);
            wm_buf_draw_string(fb, WIN_W, WIN_H, 85, 14, buf, CLR_LIGHT_GRAY);

            snprintf(buf, sizeof(buf), "Wins: %d", win_cnt);
            wm_buf_draw_string(fb, WIN_W, WIN_H, 160, 14, buf, CLR_LIGHT_MAGENTA);

            /* Divider line */
            wm_buf_draw_line(fb, WIN_W, WIN_H, 6, 24, WIN_W - 7, 24, CLR_DARK_GRAY);

            /* Performance Histogram Box (x: 8..200, y: 28..88, h: 60) */
            int gx = 10, gy = 27, gw = 196, gh = 58;
            wm_buf_fill_rect(fb, WIN_W, WIN_H, gx, gy, gw, gh, 23); /* Very dark navy */
            wm_buf_draw_rect(fb, WIN_W, WIN_H, gx - 1, gy - 1, gw + 2, gh + 2, CLR_DARK_GRAY);

            /* Target reference lines:
             * 1 ms = 0.8 pixel height (scale: max dt 70ms = 56px)
             * 33 ms (30 FPS) -> y = (gy + gh) - (33 * 56 / 70) = gy + gh - 26
             * 16 ms (60 FPS) -> y = gy + gh - 13
             */
            int y_30fps = gy + gh - 27;
            int y_60fps = gy + gh - 13;

            /* 60 FPS reference line (dotted) */
            for (int lx = gx; lx < gx + gw; lx += 4) {
                wm_buf_put_pixel(fb, WIN_W, WIN_H, lx, y_60fps, CLR_DARK_GRAY);
            }
            wm_buf_draw_string(fb, WIN_W, WIN_H, gx + 2, y_60fps - 7, "60fps", CLR_DARK_GRAY);

            /* 30 FPS reference line (dotted) */
            for (int lx = gx; lx < gx + gw; lx += 4) {
                wm_buf_put_pixel(fb, WIN_W, WIN_H, lx, y_30fps, CLR_LIGHT_BLUE);
            }
            wm_buf_draw_string(fb, WIN_W, WIN_H, gx + 2, y_30fps - 7, "30fps", CLR_LIGHT_BLUE);

            /* Draw history bars */
            int bar_w = 2;
            int num_bars = gw / bar_w;
            if (num_bars > HIST_LEN) num_bars = HIST_LEN;

            for (int i = 0; i < num_bars; i++) {
                int idx = (hist_head - num_bars + i + HIST_LEN) % HIST_LEN;
                uint16_t dt_val = history[idx];
                if (dt_val == 0) continue;

                int bar_h = (int)((dt_val * (gh - 2)) / 70);
                if (bar_h < 1) bar_h = 1;
                if (bar_h > gh - 2) bar_h = gh - 2;

                uint8_t bar_col = CLR_LIGHT_GREEN;
                if (dt_val > 50) bar_col = CLR_LIGHT_RED;
                else if (dt_val > 34) bar_col = CLR_YELLOW;

                int bx = gx + i * bar_w;
                int by = gy + gh - 1 - bar_h;
                wm_buf_fill_rect(fb, WIN_W, WIN_H, bx, by, bar_w - 1, bar_h, bar_col);
            }

            /* Bottom status / controls */
            wm_buf_draw_line(fb, WIN_W, WIN_H, 6, 90, WIN_W - 7, 90, CLR_DARK_GRAY);

            uint32_t uptime = timer_seconds();
            snprintf(buf, sizeof(buf), "Uptime: %02u:%02u", uptime / 60, uptime % 60);
            wm_buf_draw_string(fb, WIN_W, WIN_H, 8, 95, buf, CLR_LIGHT_GRAY);

            wm_buf_draw_string(fb, WIN_W, WIN_H, 115, 95, "[R]eset  [Esc]Exit", CLR_WHITE);

            wm_invalidate(win);
        }

        /* Frame pacing: update at ~30 FPS, yield CPU */
        do {
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        } while (timer_elapsed_ms(loop_start) < 33);
    }

    wm_destroy_window(win);
    printf("[fps_meter] Performance Monitor closed.\n");
    return 0;
}
