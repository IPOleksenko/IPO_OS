#ifndef LIB_VGA_GFX_H
#define LIB_VGA_GFX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <types.h>
#include <vga.h>

#define VGA_GFX_WIDTH         320
#define VGA_GFX_HEIGHT        200
#define VGA_GFX_SIZE          ((size_t)VGA_GFX_WIDTH * (size_t)VGA_GFX_HEIGHT)
#define VGA_GFX_VRAM_ADDR     ((ipo_paddr_t)0xA0000u)
#define VGA_TEXT_PIXEL_WIDTH  (vga_get_pixel_width())
#define VGA_TEXT_PIXEL_HEIGHT (vga_get_pixel_height())

#define VGA_OVERLAY_SHOW_ALL     0x00u
#define VGA_OVERLAY_HIDE_BUTTON  0x01u
#define VGA_OVERLAY_HIDE_CURSOR  0x02u
#define VGA_OVERLAY_HIDE_ALL     (VGA_OVERLAY_HIDE_BUTTON | VGA_OVERLAY_HIDE_CURSOR)

void vga_set_mode_13h(void);
void vga_set_mode_text(void);
void vga_set_mode_13h_hardware(void);
void vga_set_mode_text_hardware(void);
bool vga_is_graphics_mode(void);
void vga_save_text_state(void);
bool vga_gfx_has_saved_text(void);
bool vga_gfx_get_saved_text_buffer(uint16_t **out_vram, size_t *out_cells, uint16_t **out_cursor_pos);
void vga_gfx_scroll_saved_text(void);

void vga_gfx_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);
void vga_gfx_init_default_palette(void);

void vga_gfx_flip(const uint8_t *buf);
void vga_gfx_flip_desktop(const uint8_t *buf);
void vga_gfx_flip_scaled_buffer(const uint8_t *buf,
                                int source_width,
                                int source_height);

/* Direct blit to linear framebuffer or 0xA0000 without touching backup buffers
 * or yielding (used by the compositor and workspace switching). */
void vga_gfx_blit_hardware(const uint8_t *buf,
                           bool scale_to_display,
                           int source_width,
                           int source_height);

void vga_gfx_init(void);
void vga_gfx_get_screen_bounds(int *out_w, int *out_h);
int  vga_gfx_get_width(void);
int  vga_gfx_get_height(void);
int  vga_gfx_get_init_width(void);
int  vga_gfx_get_init_height(void);
int  vga_gfx_get_mode13h_width(void);
int  vga_gfx_get_mode13h_height(void);
bool vga_gfx_set_resolution(int width, int height);
void vga_gfx_get_resolution(int *out_w, int *out_h);

struct process;

void vga_set_overlay_flags(uint32_t flags);
uint32_t vga_get_overlay_flags(void);
void vga_hide_close_button(void);
void vga_show_close_button(void);
void vga_hide_overlay_cursor(void);
void vga_show_overlay_cursor(void);

#endif /* LIB_VGA_GFX_H */
