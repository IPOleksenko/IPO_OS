/**
 * vga_gfx.c - VGA Graphics Engine (Mode 13h: 320x200 256 colors) for IPO_OS
 *
 * Direct hardware register programming with clean Plane 2 font and register restoration.
 */

#include <vga_gfx.h>
#include <stdio.h>
#include <vga.h>
#include <ioport.h>
#include <string.h>
#include <driver/input/keymap/dynamic_keymap.h>

#define VGA_VRAM_ADDR 0xA0000

static uint8_t saved_misc = 0x67;
static uint8_t saved_seq[5] = {0x03, 0x00, 0x03, 0x00, 0x02};
static uint8_t saved_crtc[25];
static uint8_t saved_gc[9];
static uint8_t saved_ac[21];
static uint8_t saved_dac[16][3];
static uint8_t saved_font_plane2[8192];
static uint16_t saved_text_vram[80 * 25];
static uint16_t saved_cursor_pos = 0;
static bool saved_cursor_visible = false;
static bool has_saved_text_vram = false;
static bool text_state_saved = false;
static bool is_gfx = false;

static inline void crtc_write(uint8_t index, uint8_t val) {
    outb(0x3D4, index);
    outb(0x3D5, val);
}

static inline void seq_write(uint8_t index, uint8_t val) {
    outb(0x3C4, index);
    outb(0x3C5, val);
}

static inline void gc_write(uint8_t index, uint8_t val) {
    outb(0x3CE, index);
    outb(0x3CF, val);
}

static inline void ac_write(uint8_t index, uint8_t val) {
    inb(0x3DA);
    outb(0x3C0, index);
    outb(0x3C0, val);
}

void vga_save_text_state(void) {
    if (text_state_saved) return;

    /* Capture true hardware cursor position and visibility */
    saved_cursor_pos = vga_get_cursor_position();
    saved_cursor_visible = vga_is_cursor_visible();

    /* Erase any active cursor so text VRAM contains the true character */
    vga_hide_cursor();

    /* Save text screen buffer (4000 bytes at 0xB8000) */
    volatile uint16_t *text_vram = (volatile uint16_t *)0xB8000;
    for (int i = 0; i < 80 * 25; i++) {
        uint16_t entry = text_vram[i];
        if ((entry & 0xFF) == VGA_CURSOR_GLYPH_SLOT) {
            entry = (entry & 0xFF00) | ' ';
        }
        saved_text_vram[i] = entry;
    }
    has_saved_text_vram = true;

    /* Misc Output */
    saved_misc = inb(0x3CC);

    /* Sequencer */
    for (uint8_t i = 0; i < 5; i++) {
        outb(0x3C4, i);
        saved_seq[i] = inb(0x3C5);
    }

    /* CRTC */
    for (uint8_t i = 0; i < 25; i++) {
        outb(0x3D4, i);
        saved_crtc[i] = inb(0x3D5);
    }

    /* Graphics Controller */
    for (uint8_t i = 0; i < 9; i++) {
        outb(0x3CE, i);
        saved_gc[i] = inb(0x3CF);
    }

    /* Attribute Controller */
    for (uint8_t i = 0; i < 21; i++) {
        inb(0x3DA);
        outb(0x3C0, i);
        saved_ac[i] = inb(0x3C1);
    }
    inb(0x3DA);
    outb(0x3C0, 0x20);

    /* DAC Palette (16 colors) */
    for (int i = 0; i < 16; i++) {
        outb(0x3C7, (uint8_t)i);
        saved_dac[i][0] = inb(0x3C9);
        saved_dac[i][1] = inb(0x3C9);
        saved_dac[i][2] = inb(0x3C9);
    }

    /* Save Plane 2 Font (8KB: 256 glyphs * 32 bytes) */
    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x04); // Plane 2
    outb(0x3C4, 0x04); outb(0x3C5, 0x07); // Sequential addressing (disable odd/even)
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);

    outb(0x3CE, 0x00); outb(0x3CF, 0x00);
    outb(0x3CE, 0x01); outb(0x3CF, 0x00);
    outb(0x3CE, 0x03); outb(0x3CF, 0x00);
    outb(0x3CE, 0x04); outb(0x3CF, 0x02); // Read map = Plane 2
    outb(0x3CE, 0x05); outb(0x3CF, 0x00); // Write mode 0
    outb(0x3CE, 0x06); outb(0x3CF, 0x00); // Map memory to 0xA0000
    outb(0x3CE, 0x08); outb(0x3CF, 0xFF);

    volatile uint8_t *font_vram = (volatile uint8_t *)0xA0000;
    for (int i = 0; i < 8192; i++) {
        saved_font_plane2[i] = font_vram[i];
    }

    /* Restore normal text mode operation (Plane 0 & 1, 0xB8000) */
    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x03);
    outb(0x3C4, 0x04); outb(0x3C5, 0x03);
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);

    outb(0x3CE, 0x00); outb(0x3CF, 0x00);
    outb(0x3CE, 0x01); outb(0x3CF, 0x00);
    outb(0x3CE, 0x03); outb(0x3CF, 0x00);
    outb(0x3CE, 0x05); outb(0x3CF, 0x10);
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E); // 0xB8000
    outb(0x3CE, 0x08); outb(0x3CF, 0xFF);

    text_state_saved = true;
}

void vga_set_mode_13h(void) {
    vga_save_text_state();

    /* Misc Output */
    outb(0x3C2, 0x63);

    /* Sequencer */
    seq_write(0x00, 0x03);
    seq_write(0x01, 0x01);
    seq_write(0x02, 0x0F);
    seq_write(0x03, 0x00);
    seq_write(0x04, 0x0E); /* Chain-4 */

    /* Unlock CRTC registers */
    outb(0x3D4, 0x03);
    outb(0x3D5, inb(0x3D5) | 0x80);
    outb(0x3D4, 0x11);
    outb(0x3D5, inb(0x3D5) & ~0x80);

    /* CRTC registers */
    crtc_write(0x00, 0x5F);
    crtc_write(0x01, 0x4F);
    crtc_write(0x02, 0x50);
    crtc_write(0x03, 0x82);
    crtc_write(0x04, 0x54);
    crtc_write(0x05, 0x80);
    crtc_write(0x06, 0xBF);
    crtc_write(0x07, 0x1F);
    crtc_write(0x08, 0x00);
    crtc_write(0x09, 0x41);
    crtc_write(0x0A, 0x00);
    crtc_write(0x0B, 0x00);
    crtc_write(0x0C, 0x00);
    crtc_write(0x0D, 0x00);
    crtc_write(0x0E, 0x00);
    crtc_write(0x0F, 0x00);
    crtc_write(0x10, 0x9C);
    crtc_write(0x11, 0x0E);
    crtc_write(0x12, 0x8F);
    crtc_write(0x13, 0x28);
    crtc_write(0x14, 0x40);
    crtc_write(0x15, 0x96);
    crtc_write(0x16, 0xB9);
    crtc_write(0x17, 0xA3);
    crtc_write(0x18, 0xFF);

    /* Graphics Controller */
    gc_write(0x00, 0x00);
    gc_write(0x01, 0x00);
    gc_write(0x02, 0x00);
    gc_write(0x03, 0x00);
    gc_write(0x04, 0x00);
    gc_write(0x05, 0x40); /* 256 colors */
    gc_write(0x06, 0x05); /* 0xA0000 64KB */
    gc_write(0x07, 0x0F);
    gc_write(0x08, 0xFF);

    /* Attribute Controller */
    for (uint8_t i = 0; i < 16; i++) {
        ac_write(i, i);
    }
    ac_write(0x10, 0x41); /* 8-bit color */
    ac_write(0x11, 0x00);
    ac_write(0x12, 0x0F);
    ac_write(0x13, 0x00);
    ac_write(0x14, 0x00);

    /* Re-enable video */
    inb(0x3DA);
    outb(0x3C0, 0x20);

    /* Initialize palette */
    vga_gfx_init_default_palette();
    vga_gfx_clear(0);
    is_gfx = true;
}

void vga_set_mode_text(void) {
    /* Misc */
    outb(0x3C2, saved_misc ? saved_misc : 0x67);

    /* Sequencer */
    for (uint8_t i = 0; i < 5; i++) {
        outb(0x3C4, i);
        uint8_t val = inb(0x3C5);
        serial_printf("[vga_gfx] read seq[%d] = 0x%x\n", i, val);
    }
    for (uint8_t i = 0; i < 5; i++) {
        seq_write(i, saved_seq[i]);
    }

    /* Unlock CRTC */
    outb(0x3D4, 0x11);
    outb(0x3D5, inb(0x3D5) & ~0x80);

    /* CRTC */
    for (uint8_t i = 0; i < 25; i++) {
        crtc_write(i, saved_crtc[i]);
    }

    /* GC */
    for (uint8_t i = 0; i < 9; i++) {
        gc_write(i, saved_gc[i]);
    }

    /* AC */
    for (uint8_t i = 0; i < 21; i++) {
        ac_write(i, saved_ac[i]);
    }
    inb(0x3DA);
    outb(0x3C0, 0x20);

    /* DAC */
    for (int i = 0; i < 16; i++) {
        outb(0x3C8, (uint8_t)i);
        outb(0x3C9, saved_dac[i][0]);
        outb(0x3C9, saved_dac[i][1]);
        outb(0x3C9, saved_dac[i][2]);
    }

    /* Restore Plane 2 Font */
    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x04); // Write only to Plane 2
    outb(0x3C4, 0x04); outb(0x3C5, 0x07); // Sequential addressing
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);

    outb(0x3CE, 0x00); outb(0x3CF, 0x00);
    outb(0x3CE, 0x01); outb(0x3CF, 0x00);
    outb(0x3CE, 0x03); outb(0x3CF, 0x00);
    outb(0x3CE, 0x04); outb(0x3CF, 0x02); // Read map = Plane 2
    outb(0x3CE, 0x05); outb(0x3CF, 0x00); // Write mode 0
    outb(0x3CE, 0x06); outb(0x3CF, 0x00); // Map memory to 0xA0000
    outb(0x3CE, 0x08); outb(0x3CF, 0xFF);

    volatile uint8_t *dest = (volatile uint8_t *)0xA0000;
    for (int i = 0; i < 8192; i++) {
        dest[i] = saved_font_plane2[i];
    }

    /* Restore normal text mode operation (Plane 0 & 1, 0xB8000) */
    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x03); // Enable planes 0 & 1
    outb(0x3C4, 0x04); outb(0x3C5, 0x03); // Odd/even addressing
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);

    outb(0x3CE, 0x00); outb(0x3CF, 0x00);
    outb(0x3CE, 0x01); outb(0x3CF, 0x00);
    outb(0x3CE, 0x03); outb(0x3CF, 0x00);
    outb(0x3CE, 0x04); outb(0x3CF, 0x00);
    outb(0x3CE, 0x05); outb(0x3CF, 0x10); // Normal host mode
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E); // Map memory back to 0xB8000
    outb(0x3CE, 0x08); outb(0x3CF, 0xFF);

    /* Re-apply active custom glyphs directly from source bitmaps to eliminate any mode 13h residues */
    vga_font_reapply_active();
    dynamic_keymap_reapply_fonts();

    /* Drain keyboard controller buffer */
    for (int d = 0; d < 16 && (inb(0x64) & 0x01); d++) {
        (void)inb(0x60);
    }

    if (has_saved_text_vram) {
        volatile uint16_t *text_vram = (volatile uint16_t *)0xB8000;
        for (int i = 0; i < 80 * 25; i++) {
            if ((saved_text_vram[i] & 0xFF) == VGA_CURSOR_GLYPH_SLOT) {
                saved_text_vram[i] = (saved_text_vram[i] & 0xFF00) | ' ';
            }
            text_vram[i] = saved_text_vram[i];
        }
        vga_set_cursor(saved_cursor_pos);
        if (saved_cursor_visible) {
            vga_show_cursor();
        } else {
            vga_hide_cursor();
        }
    } else {
        vga_hide_cursor();
    }

    is_gfx = false;
    text_state_saved = false;
    has_saved_text_vram = false;
}

bool vga_is_graphics_mode(void) {
    return is_gfx;
}

void vga_gfx_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    outb(0x3C8, idx);
    outb(0x3C9, r & 0x3F);
    outb(0x3C9, g & 0x3F);
    outb(0x3C9, b & 0x3F);
}

void vga_gfx_init_default_palette(void) {
    /* 16 Standard Colors */
    vga_gfx_set_palette(0,  0,  0,  0);
    vga_gfx_set_palette(1,  0,  0, 42);
    vga_gfx_set_palette(2,  0, 42,  0);
    vga_gfx_set_palette(3,  0, 42, 42);
    vga_gfx_set_palette(4, 42,  0,  0);
    vga_gfx_set_palette(5, 42,  0, 42);
    vga_gfx_set_palette(6, 42, 21,  0);
    vga_gfx_set_palette(7, 42, 42, 42);
    vga_gfx_set_palette(8, 21, 21, 21);
    vga_gfx_set_palette(9, 21, 21, 63);
    vga_gfx_set_palette(10, 21, 63, 21);
    vga_gfx_set_palette(11, 21, 63, 63);
    vga_gfx_set_palette(12, 63, 21, 21);
    vga_gfx_set_palette(13, 63, 21, 63);
    vga_gfx_set_palette(14, 63, 63, 21);
    vga_gfx_set_palette(15, 63, 63, 63);

    /* 16..31: Grayscale */
    for (int i = 0; i < 16; i++) {
        uint8_t v = (uint8_t)(i * 4);
        vga_gfx_set_palette((uint8_t)(16 + i), v, v, v);
    }

    /* 32..247: Vibrant 6x6x6 color cube */
    int idx = 32;
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                vga_gfx_set_palette((uint8_t)idx++,
                                    (uint8_t)(r * 12 + 3),
                                    (uint8_t)(g * 12 + 3),
                                    (uint8_t)(b * 12 + 3));
            }
        }
    }
}

/* ================= Direct VRAM Primitives ================= */

void vga_gfx_put_pixel(int x, int y, uint8_t color) {
    if (x < 0 || x >= VGA_GFX_WIDTH || y < 0 || y >= VGA_GFX_HEIGHT) return;
    volatile uint8_t *vram = (volatile uint8_t *)VGA_VRAM_ADDR;
    vram[y * VGA_GFX_WIDTH + x] = color;
}

uint8_t vga_gfx_get_pixel(int x, int y) {
    if (x < 0 || x >= VGA_GFX_WIDTH || y < 0 || y >= VGA_GFX_HEIGHT) return 0;
    volatile uint8_t *vram = (volatile uint8_t *)VGA_VRAM_ADDR;
    return vram[y * VGA_GFX_WIDTH + x];
}

void vga_gfx_clear(uint8_t color) {
    volatile uint8_t *vram = (volatile uint8_t *)VGA_VRAM_ADDR;
    memset((void *)vram, color, VGA_GFX_SIZE);
}

void vga_gfx_draw_line(int x0, int y0, int x1, int y1, uint8_t color) {
    int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        vga_gfx_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void vga_gfx_draw_rect(int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    vga_gfx_draw_line(x, y, x + w - 1, y, color);
    vga_gfx_draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    vga_gfx_draw_line(x, y, x, y + h - 1, color);
    vga_gfx_draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

void vga_gfx_fill_rect(int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > VGA_GFX_WIDTH) x2 = VGA_GFX_WIDTH;
    if (y2 > VGA_GFX_HEIGHT) y2 = VGA_GFX_HEIGHT;

    volatile uint8_t *vram = (volatile uint8_t *)VGA_VRAM_ADDR;
    for (int cy = y; cy < y2; cy++) {
        memset((void *)(vram + (cy * VGA_GFX_WIDTH + x)), color, (size_t)(x2 - x));
    }
}

void vga_gfx_draw_circle(int xc, int yc, int r, uint8_t color) {
    int x = 0;
    int y = r;
    int d = 3 - 2 * r;

    while (y >= x) {
        vga_gfx_put_pixel(xc + x, yc + y, color);
        vga_gfx_put_pixel(xc - x, yc + y, color);
        vga_gfx_put_pixel(xc + x, yc - y, color);
        vga_gfx_put_pixel(xc - x, yc - y, color);
        vga_gfx_put_pixel(xc + y, yc + x, color);
        vga_gfx_put_pixel(xc - y, yc + x, color);
        vga_gfx_put_pixel(xc + y, yc - x, color);
        vga_gfx_put_pixel(xc - y, yc - x, color);

        if (d <= 0) {
            d = d + 4 * x + 6;
        } else {
            d = d + 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

void vga_gfx_fill_circle(int xc, int yc, int r, uint8_t color) {
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                vga_gfx_put_pixel(xc + dx, yc + dy, color);
            }
        }
    }
}

/* ================= Double-Buffering Primitives ================= */

void vga_gfx_buf_put_pixel(uint8_t *buf, int x, int y, uint8_t color) {
    if (x < 0 || x >= VGA_GFX_WIDTH || y < 0 || y >= VGA_GFX_HEIGHT) return;
    buf[y * VGA_GFX_WIDTH + x] = color;
}

void vga_gfx_buf_clear(uint8_t *buf, uint8_t color) {
    memset(buf, color, VGA_GFX_SIZE);
}

void vga_gfx_buf_fill_rect(uint8_t *buf, int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > VGA_GFX_WIDTH) x2 = VGA_GFX_WIDTH;
    if (y2 > VGA_GFX_HEIGHT) y2 = VGA_GFX_HEIGHT;

    for (int cy = y; cy < y2; cy++) {
        memset(buf + (cy * VGA_GFX_WIDTH + x), color, (size_t)(x2 - x));
    }
}

void vga_gfx_buf_draw_rect(uint8_t *buf, int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    vga_gfx_buf_draw_line(buf, x, y, x + w - 1, y, color);
    vga_gfx_buf_draw_line(buf, x, y + h - 1, x + w - 1, y + h - 1, color);
    vga_gfx_buf_draw_line(buf, x, y, x, y + h - 1, color);
    vga_gfx_buf_draw_line(buf, x + w - 1, y, x + w - 1, y + h - 1, color);
}

void vga_gfx_buf_draw_line(uint8_t *buf, int x0, int y0, int x1, int y1, uint8_t color) {
    int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        vga_gfx_buf_put_pixel(buf, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void vga_gfx_buf_draw_circle(uint8_t *buf, int xc, int yc, int r, uint8_t color) {
    int x = 0;
    int y = r;
    int d = 3 - 2 * r;

    while (y >= x) {
        vga_gfx_buf_put_pixel(buf, xc + x, yc + y, color);
        vga_gfx_buf_put_pixel(buf, xc - x, yc + y, color);
        vga_gfx_buf_put_pixel(buf, xc + x, yc - y, color);
        vga_gfx_buf_put_pixel(buf, xc - x, yc - y, color);
        vga_gfx_buf_put_pixel(buf, xc + y, yc + x, color);
        vga_gfx_buf_put_pixel(buf, xc - y, yc + x, color);
        vga_gfx_buf_put_pixel(buf, xc + y, yc - x, color);
        vga_gfx_buf_put_pixel(buf, xc - y, yc - x, color);

        if (d <= 0) {
            d = d + 4 * x + 6;
        } else {
            d = d + 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

void vga_gfx_buf_fill_circle(uint8_t *buf, int xc, int yc, int r, uint8_t color) {
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                vga_gfx_buf_put_pixel(buf, xc + dx, yc + dy, color);
            }
        }
    }
}

void vga_gfx_flip(const uint8_t *buf) {
    volatile uint8_t *vram = (volatile uint8_t *)VGA_VRAM_ADDR;
    memcpy((void *)vram, buf, VGA_GFX_SIZE);
}
