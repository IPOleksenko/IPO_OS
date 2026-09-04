#ifndef LIB_VGA_GFX_H
#define LIB_VGA_GFX_H

#include <stdint.h>
#include <stdbool.h>

#define VGA_GFX_WIDTH   320
#define VGA_GFX_HEIGHT  200
#define VGA_GFX_SIZE    (VGA_GFX_WIDTH * VGA_GFX_HEIGHT)

/* Mode switching */
void vga_set_mode_13h(void);
void vga_set_mode_text(void);
bool vga_is_graphics_mode(void);

/* Palette */
void vga_gfx_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);
void vga_gfx_init_default_palette(void);

/* Drawing primitives (Direct to VRAM at 0xA0000) */
void vga_gfx_put_pixel(int x, int y, uint8_t color);
uint8_t vga_gfx_get_pixel(int x, int y);
void vga_gfx_clear(uint8_t color);
void vga_gfx_draw_line(int x0, int y0, int x1, int y1, uint8_t color);
void vga_gfx_draw_rect(int x, int y, int w, int h, uint8_t color);
void vga_gfx_fill_rect(int x, int y, int w, int h, uint8_t color);
void vga_gfx_draw_circle(int xc, int yc, int r, uint8_t color);
void vga_gfx_fill_circle(int xc, int yc, int r, uint8_t color);

/* Double-buffering offscreen render */
void vga_gfx_buf_put_pixel(uint8_t *buf, int x, int y, uint8_t color);
void vga_gfx_buf_clear(uint8_t *buf, uint8_t color);
void vga_gfx_buf_fill_rect(uint8_t *buf, int x, int y, int w, int h, uint8_t color);
void vga_gfx_buf_draw_rect(uint8_t *buf, int x, int y, int w, int h, uint8_t color);
void vga_gfx_buf_draw_circle(uint8_t *buf, int xc, int yc, int r, uint8_t color);
void vga_gfx_buf_fill_circle(uint8_t *buf, int xc, int yc, int r, uint8_t color);
void vga_gfx_buf_draw_line(uint8_t *buf, int x0, int y0, int x1, int y1, uint8_t color);
void vga_gfx_flip(const uint8_t *buf);

#endif /* LIB_VGA_GFX_H */
