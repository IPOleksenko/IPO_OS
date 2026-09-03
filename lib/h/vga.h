#ifndef ARCH_I386_VGA_H
#define ARCH_I386_VGA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum vga_color {
    VGA_COLOR_BLACK = 0,
    VGA_COLOR_BLUE = 1,
    VGA_COLOR_GREEN = 2,
    VGA_COLOR_CYAN = 3,
    VGA_COLOR_RED = 4,
    VGA_COLOR_MAGENTA = 5,
    VGA_COLOR_BROWN = 6,
    VGA_COLOR_LIGHT_GREY = 7,
    VGA_COLOR_DARK_GREY = 8,
    VGA_COLOR_LIGHT_BLUE = 9,
    VGA_COLOR_LIGHT_GREEN = 10,
    VGA_COLOR_LIGHT_CYAN = 11,
    VGA_COLOR_LIGHT_RED = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_LIGHT_BROWN = 14,
    VGA_COLOR_WHITE = 15,
};

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY ((volatile uint16_t*)0xB8000)

#define VGA_START_CURSOR_POSITION (VGA_WIDTH * 2)

// Creates a 16-bit value of a character + attribute
static inline uint16_t vga_entry(unsigned char c, enum vga_color fg, enum vga_color bg) {
    return (uint16_t)c | ((bg << 4 | fg) << 8);
}

// Sets the cursor position
// offset = row * VGA_WIDTH + col
void vga_set_cursor(uint16_t offset);

// Shows the cursor
void vga_show_cursor(void);

// Hides the cursor
void vga_hide_cursor(void);

// Clears the VGA screen with specified foreground and background colors and cursor settings
void vga_clear(enum vga_color fg, enum vga_color bg, bool show_cursor, int cursor_position);

uint16_t vga_get_cursor_position(void);

uint16_t vga_increment_cursor_position(void);

uint16_t vga_decrement_cursor_position(void);

typedef struct {
    uint32_t codepoint;    /* Unicode codepoint (e.g. 0x0410, 0x4E2D, 0x03B1) */
    uint8_t  bitmap[16];   /* 8x16 1-bit font bitmap */
} dynamic_glyph_def_t;

void vga_load_font(uint8_t start_code, uint32_t count, const uint8_t *glyphs);
int vga_load_cyrillic_font(const char *path);
void vga_font_clear_registry(void);
int vga_font_register_glyph(uint32_t codepoint, const uint8_t *bitmap);
void vga_font_apply_glyphs(const dynamic_glyph_def_t *glyphs, uint32_t count);
uint8_t utf8_to_vga_glyph(const char *str, size_t str_len, size_t *out_bytes);

#endif
