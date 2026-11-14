#ifndef LIB_VGA_GFX_H
#define LIB_VGA_GFX_H

#include <stdint.h>
#include <stdbool.h>

#define VGA_GFX_WIDTH   320
#define VGA_GFX_HEIGHT  200
#define VGA_GFX_SIZE    (VGA_GFX_WIDTH * VGA_GFX_HEIGHT)
#define VGA_GFX_VRAM_ADDR 0xA0000
#define VGA_TEXT_PIXEL_WIDTH  720
#define VGA_TEXT_PIXEL_HEIGHT 400

/* VGA Raw Graphics Overlay Flags */
#define VGA_OVERLAY_SHOW_ALL     0x00u
#define VGA_OVERLAY_HIDE_BUTTON  0x01u
#define VGA_OVERLAY_HIDE_CURSOR  0x02u
#define VGA_OVERLAY_HIDE_ALL     (VGA_OVERLAY_HIDE_BUTTON | VGA_OVERLAY_HIDE_CURSOR)

/* Mode switching */
void vga_set_mode_13h(void);
void vga_set_mode_text(void);
void vga_set_mode_13h_hardware(void);
void vga_set_mode_text_hardware(void);
bool vga_is_graphics_mode(void);
void vga_save_text_state(void);

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
void vga_gfx_flip_desktop(const uint8_t *buf);
void vga_gfx_flip_scaled_buffer(const uint8_t *buf,
                                int source_width,
                                int source_height);

/* Pure hardware blit: writes @buf to the BGA framebuffer or 0xA0000 without
 * touching any per-process backup buffers and without yielding. Used by the
 * kernel itself (WM compositor, workspace switching). */
void vga_gfx_blit_hardware(const uint8_t *buf,
                           bool scale_to_display,
                           int source_width,
                           int source_height);

/* Active display bounds */
void vga_gfx_get_screen_bounds(int *out_w, int *out_h);
int  vga_gfx_get_width(void);
int  vga_gfx_get_height(void);
bool vga_gfx_set_resolution(int width, int height);
void vga_gfx_get_resolution(int *out_w, int *out_h);

/* Overlay controls */
void vga_set_overlay_flags(uint32_t flags);
uint32_t vga_get_overlay_flags(void);
void vga_hide_close_button(void);
void vga_show_close_button(void);
void vga_hide_overlay_cursor(void);
void vga_show_overlay_cursor(void);

#endif /* LIB_VGA_GFX_H */
