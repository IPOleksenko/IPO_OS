/**
 * figure - Interactive VGA Graphics Figure Mover for IPO_OS
 *
 * Demonstrates:
 * - Direct VGA Mode 13h (320x200 256 colors) hardware switching
 * - High-speed double buffering (60 FPS tear-free rendering)
 * - Interactive movement (Arrow keys / WASD) & Auto-Bounce mode
 * - Colorful figures (Diamond, Multi-layer Cube, Spaceship, Glowing Circle)
 * - Dynamic color shifting, motion trails, and safe restoration of text mode
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ioport.h>
#include <vga_gfx.h>
#include <vga.h>
#include <system/timer.h>
#include <system/state.h>
#include <memory/kmalloc.h>
#include <driver/input/keyboard.h>
#include <driver/input/keymap/keymap.h>
#include <driver/input/keymap/dynamic_keymap.h>
#include <kernel/terminal.h>

/* Simple 5x7 bitmap font for graphics HUD */
static const uint8_t font5x7[128][7] = {
    [' '] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['!'] = {0x04,0x04,0x04,0x04,0x00,0x00,0x04},
    [':'] = {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
    ['-'] = {0x00,0x00,0x1F,0x00,0x00,0x00,0x00},
    ['+'] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
    ['0'] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    ['1'] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    ['2'] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    ['3'] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
    ['4'] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    ['5'] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    ['6'] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    ['7'] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    ['8'] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    ['9'] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    ['A'] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['B'] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    ['C'] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    ['D'] = {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
    ['E'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    ['F'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    ['G'] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
    ['H'] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['I'] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    ['L'] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    ['M'] = {0x11,0x1B,0x15,0x11,0x11,0x11,0x11},
    ['N'] = {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
    ['O'] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['P'] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    ['Q'] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    ['R'] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    ['S'] = {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E},
    ['T'] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    ['U'] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['V'] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    ['W'] = {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
    ['X'] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    ['Y'] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    ['Z'] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    ['['] = {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
    [']'] = {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
    [','] = {0x00,0x00,0x00,0x00,0x0C,0x04,0x08}
};

static void draw_char(uint8_t *buf, int x, int y, char c, uint8_t color) {
    if (c < 0 || c >= 128) return;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    for (int r = 0; r < 7; r++) {
        uint8_t row = font5x7[(uint8_t)c][r];
        for (int b = 0; b < 5; b++) {
            if (row & (0x10 >> b)) {
                vga_gfx_buf_put_pixel(buf, x + b, y + r, color);
            }
        }
    }
}

static void draw_string(uint8_t *buf, int x, int y, const char *s, uint8_t color) {
    while (*s) {
        draw_char(buf, x, y, *s, color);
        x += 6;
        s++;
    }
}

/* Figure Shapes */
enum figure_shape {
    SHAPE_DIAMOND = 0,
    SHAPE_CUBE,
    SHAPE_CIRCLE,
    SHAPE_SPACESHIP,
    SHAPE_MAX
};

/* Color Themes */
static const uint8_t theme_colors[][4] = {
    {14, 12, 10, 9},   /* Yellow / Light Red / Light Green / Light Blue */
    {11, 3,  9,  1},   /* Light Cyan / Cyan / Light Blue / Blue */
    {13, 5,  12, 4},   /* Magenta / Dark Magenta / Light Red / Red */
    {10, 2,  14, 6},   /* Light Green / Green / Yellow / Brown */
    {15, 14, 12, 9}    /* White / Yellow / Red / Blue */
};
#define THEME_COUNT (sizeof(theme_colors) / sizeof(theme_colors[0]))

static void draw_figure(uint8_t *buf, int x, int y, int size, int shape, int theme) {
    uint8_t c1 = theme_colors[theme % THEME_COUNT][0];
    uint8_t c2 = theme_colors[theme % THEME_COUNT][1];
    uint8_t c3 = theme_colors[theme % THEME_COUNT][2];
    uint8_t c4 = theme_colors[theme % THEME_COUNT][3];

    int half = size / 2;

    switch (shape) {
        case SHAPE_DIAMOND:
            /* Concentric multi-colored diamond */
            for (int r = half; r >= 0; r--) {
                uint8_t col = (r > half * 2 / 3) ? c1 : ((r > half / 3) ? c2 : c3);
                for (int dy = -r; dy <= r; dy++) {
                    int span = r - ((dy >= 0) ? dy : -dy);
                    for (int dx = -span; dx <= span; dx++) {
                        vga_gfx_buf_put_pixel(buf, x + dx, y + dy, col);
                    }
                }
            }
            /* Glowing center point */
            vga_gfx_buf_fill_circle(buf, x, y, 2, 15);
            break;

        case SHAPE_CUBE:
            /* Multi-layer glowing cube */
            vga_gfx_buf_fill_rect(buf, x - half, y - half, size, size, c1);
            vga_gfx_buf_fill_rect(buf, x - half + 3, y - half + 3, size - 6, size - 6, c2);
            vga_gfx_buf_fill_rect(buf, x - half + 6, y - half + 6, size - 12, size - 12, c3);
            vga_gfx_buf_fill_rect(buf, x - 2, y - 2, 5, 5, 15);
            vga_gfx_buf_draw_rect(buf, x - half, y - half, size, size, 15);
            break;

        case SHAPE_CIRCLE:
            /* Glowing concentric sphere / orb */
            vga_gfx_buf_fill_circle(buf, x, y, half, c1);
            vga_gfx_buf_fill_circle(buf, x, y, half * 2 / 3, c2);
            vga_gfx_buf_fill_circle(buf, x, y, half / 3, c3);
            vga_gfx_buf_fill_circle(buf, x - half / 3, y - half / 3, 2, 15);
            break;

        case SHAPE_SPACESHIP:
            /* Retro triangle spaceship */
            for (int dy = -half; dy <= half; dy++) {
                int w = (dy + half) * half / (size > 0 ? size : 1);
                for (int dx = -w; dx <= w; dx++) {
                    vga_gfx_buf_put_pixel(buf, x + dx, y + dy, c2);
                }
            }
            /* Wings */
            vga_gfx_buf_draw_line(buf, x - half, y + half, x, y - half, c1);
            vga_gfx_buf_draw_line(buf, x + half, y + half, x, y - half, c1);
            vga_gfx_buf_draw_line(buf, x - half, y + half, x + half, y + half, c3);
            /* Engine flame */
            vga_gfx_buf_fill_circle(buf, x, y + half + 2, 3, c4);
            vga_gfx_buf_put_pixel(buf, x, y + half + 3, 14);
            break;
    }
}

static uint16_t saved_screen[80 * 25];
static uint16_t saved_cursor = 0;

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Save text screen buffer and cursor before switching video mode */
    memcpy(saved_screen, (const void *)0xB8000, sizeof(saved_screen));
    saved_cursor = vga_get_cursor_position();

    /* Put keyboard in dedicated app mode and clean hardware queue */
    keyboard_set_app_input_mode(true);
    keyboard_flush_hardware();
    keyboard_flush_queue();
    keyboard_clear_key_state();

    uint8_t backbuffer[VGA_GFX_SIZE];

    /* Switch VGA hardware to Mode 13h (320x200 256 colors) */
    vga_set_mode_13h();

    int fig_x = 160;
    int fig_y = 100;
    int fig_size = 24;
    int vel_x = 3;
    int vel_y = 2;
    int shape = SHAPE_DIAMOND;
    int theme = 0;
    bool auto_bounce = true;
    int speed = 3;

    /* Motion trail */
    #define TRAIL_LEN 8
    int trail_x[TRAIL_LEN];
    int trail_y[TRAIL_LEN];
    for (int i = 0; i < TRAIL_LEN; i++) {
        trail_x[i] = fig_x;
        trail_y[i] = fig_y;
    }
    int trail_head = 0;

    static bool ctrl_held = false;
    bool running = true;

    while (running) {
        if (system_is_interrupted()) {
            goto exit_figure;
        }

        /* 1. Process keyboard inputs */
        while (1) {
            uint8_t sc = 0;
            if (inb(0x64) & 0x01) {
                sc = inb(0x60);
            }
            if (sc == 0) break;
            update_hot_key_state(sc);

            if (sc == 0x1D) {
                ctrl_held = true;
                continue;
            } else if (sc == 0x9D) {
                ctrl_held = false;
                continue;
            }

            /* Quick exit on Q or ESC release */
            if (sc == 0x81 || sc == 0x90) {
                goto exit_figure;
            }

            if (sc & 0x80) continue; // ignore other key releases

            /* Check exit triggers: ESC (0x01), Q (0x10), Enter (0x1C), or Ctrl+C */
            if (sc == 0x01 || sc == 0x10 || sc == 0x1C) {
                goto exit_figure;
            }
            if (sc == 0x2E) { // 'C' key
                if (ctrl_held) {
                    goto exit_figure;
                }
                theme = (theme + 1) % THEME_COUNT;
            } else if (sc == 0x39) { // SPACE -> Toggle Auto-Bounce / Manual
                auto_bounce = !auto_bounce;
            } else if (sc == 0x21) { // F -> Change Shape
                shape = (shape + 1) % SHAPE_MAX;
            } else if (sc == 0x48 || sc == 0x11) { // UP (0x48) / W (0x11)
                fig_y -= speed * 3;
                auto_bounce = false;
            } else if (sc == 0x50 || sc == 0x1F) { // DOWN (0x50) / S (0x1F)
                fig_y += speed * 3;
                auto_bounce = false;
            } else if (sc == 0x4B || sc == 0x1E) { // LEFT (0x4B) / A (0x1E)
                fig_x -= speed * 3;
                auto_bounce = false;
            } else if (sc == 0x4D || sc == 0x20) { // RIGHT (0x4D) / D (0x20)
                fig_x += speed * 3;
                auto_bounce = false;
            } else if (sc == 0x0D || sc == 0x4E) { // + / = or Keypad + -> Increase speed
                if (speed < 10) speed++;
            } else if (sc == 0x0C || sc == 0x4A) { // - or Keypad - -> Decrease speed
                if (speed > 1) speed--;
            }
        }

        /* 2. Physics / Movement update */
        int half = fig_size / 2;
        int min_x = half + 2;
        int max_x = VGA_GFX_WIDTH - half - 2;
        int min_y = half + 14; /* under top HUD */
        int max_y = VGA_GFX_HEIGHT - half - 2;

        if (auto_bounce) {
            fig_x += (vel_x >= 0 ? speed : -speed);
            fig_y += (vel_y >= 0 ? speed : -speed);

            bool bounced = false;
            if (fig_x <= min_x) {
                fig_x = min_x;
                vel_x = -vel_x;
                bounced = true;
            } else if (fig_x >= max_x) {
                fig_x = max_x;
                vel_x = -vel_x;
                bounced = true;
            }

            if (fig_y <= min_y) {
                fig_y = min_y;
                vel_y = -vel_y;
                bounced = true;
            } else if (fig_y >= max_y) {
                fig_y = max_y;
                vel_y = -vel_y;
                bounced = true;
            }

            if (bounced) {
                theme = (theme + 1) % THEME_COUNT;
            }
        } else {
            /* Keep within bounds in manual mode */
            if (fig_x < min_x) fig_x = min_x;
            if (fig_x > max_x) fig_x = max_x;
            if (fig_y < min_y) fig_y = min_y;
            if (fig_y > max_y) fig_y = max_y;
        }

        /* Store trail */
        trail_x[trail_head] = fig_x;
        trail_y[trail_head] = fig_y;
        trail_head = (trail_head + 1) % TRAIL_LEN;

        /* 3. Render Frame */
        /* Background: Deep dark space */
        vga_gfx_buf_clear(backbuffer, 0);

        /* Starry background dots */
        static const int stars[][2] = {
            {20, 30}, {85, 45}, {140, 25}, {210, 50}, {280, 35},
            {40, 95}, {110, 130}, {190, 110}, {260, 85}, {300, 140},
            {30, 170}, {90, 180}, {160, 160}, {240, 175}, {295, 190}
        };
        for (size_t s = 0; s < sizeof(stars)/sizeof(stars[0]); s++) {
            vga_gfx_buf_put_pixel(backbuffer, stars[s][0], stars[s][1], 8);
        }

        /* Top HUD Bar */
        vga_gfx_buf_fill_rect(backbuffer, 0, 0, VGA_GFX_WIDTH, 12, 1);
        vga_gfx_buf_draw_line(backbuffer, 0, 12, VGA_GFX_WIDTH - 1, 12, 9);

        draw_string(backbuffer, 4, 3, "IPO_OS GRAPHICS", 14);
        if (auto_bounce) {
            draw_string(backbuffer, 96, 3, "[AUTO-BOUNCE]", 10);
        } else {
            draw_string(backbuffer, 96, 3, "[MANUAL MOVE]", 11);
        }
        draw_string(backbuffer, 180, 3, "ARROWS/WASD:MOVE", 15);
        draw_string(backbuffer, 275, 3, "Q/ESC:QUIT", 12);

        /* Draw Motion Trail */
        for (int i = 0; i < TRAIL_LEN; i++) {
            int idx = (trail_head + i) % TRAIL_LEN;
            int r = (i + 1) * 2;
            uint8_t tr_col = 16 + (i * 2); // Dark to light grey/blue
            vga_gfx_buf_draw_circle(backbuffer, trail_x[idx], trail_y[idx], r, tr_col);
        }

        /* Draw Colored Figure */
        draw_figure(backbuffer, fig_x, fig_y, fig_size, shape, theme);

        /* Bottom Info Bar */
        static const char *shape_names[] = {"DIAMOND", "CUBE", "CIRCLE", "SHIP"};
        char info_str[64];
        snprintf(info_str, sizeof(info_str), "SPACE:MODE C:CLR(%d) F:%s SPD:%d(+/-)", theme + 1, shape_names[shape], speed);
        draw_string(backbuffer, 4, 190, info_str, 7);

        /* Copy backbuffer to VGA memory (0xA0000) */
        vga_gfx_flip(backbuffer);

        /* Frame pacing (60 FPS ~ 16ms) with bounded hardware vsync */
        uint32_t frame_start = timer_millis();
        int vsync_limit = 50000;
        while ((inb(0x3DA) & 0x08) && --vsync_limit > 0) {}
        vsync_limit = 50000;
        while (!(inb(0x3DA) & 0x08) && --vsync_limit > 0) {}

        /* Frame pacing delay without consuming keystrokes */
        while (timer_elapsed_ms(frame_start) < 16u) {
            __asm__ volatile ("pause");
        }
    }

exit_figure:
    /* Restore keyboard mode and flush hardware/buffers */
    keyboard_clear_key_state();
    keyboard_flush_hardware();
    keyboard_flush_queue();

    /* Clean exit: Restore 80x25 text mode and previous screen */
    vga_set_mode_text();
    memcpy((void *)0xB8000, saved_screen, sizeof(saved_screen));
    vga_set_cursor(saved_cursor);
    return 0;
}
