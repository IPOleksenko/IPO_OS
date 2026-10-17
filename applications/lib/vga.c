#include <vga.h>
#include <vga_gfx.h>
#include <driver/vbe_bga.h>
#include <syscall.h>
#include <string.h>

void vga_set_mode_13h(void) {
    uint32_t arg = 1u;
    ipo_syscall(IPO_SYSCALL_VGA_SET_MODE, 1u, &arg);
}

void vga_set_mode_text(void) {
    uint32_t arg = 0u;
    ipo_syscall(IPO_SYSCALL_VGA_SET_MODE, 1u, &arg);
}

bool vga_is_graphics_mode(void) {
    return ipo_syscall(IPO_SYSCALL_VGA_GET_MODE, 0u, NULL) != 0;
}

void vga_set_cursor(uint16_t offset) {
    uint32_t arg = (uint32_t)offset;
    ipo_syscall(IPO_SYSCALL_VGA_SET_CURSOR, 1u, &arg);
}

uint16_t vga_get_cursor_position(void) {
    return (uint16_t)ipo_syscall(IPO_SYSCALL_VGA_GET_CURSOR, 0u, NULL);
}

void vga_show_cursor(void) {
    ipo_syscall(IPO_SYSCALL_VGA_SHOW_CURSOR, 0u, NULL);
}

void vga_hide_cursor(void) {
    ipo_syscall(IPO_SYSCALL_VGA_HIDE_CURSOR, 0u, NULL);
}

void vga_gfx_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    uint32_t args[4] = { (uint32_t)idx, (uint32_t)r, (uint32_t)g, (uint32_t)b };
    ipo_syscall(IPO_SYSCALL_VGA_SET_PALETTE, 4u, args);
}

void vga_gfx_flip(const uint8_t *buf) {
    if (buf) {
        memcpy((void *)0xA0000, buf, 320 * 200);
    }
}

void vga_gfx_get_screen_bounds(int *out_w, int *out_h) {
    if (out_w) *out_w = 320;
    if (out_h) *out_h = 200;
}

void vga_font_set_app_mode(bool is_app) {
    (void)is_app;
}

int vga_font_load_database(const char *path) {
    uint32_t args[1] = { (uint32_t)(uintptr_t)path };
    return ipo_syscall(IPO_SYSCALL_FONT_LOAD, 1u, args);
}

void vga_load_font(uint8_t start_code, uint32_t count, const uint8_t *glyphs) {
    uint32_t args[3] = { (uint32_t)start_code, count, (uint32_t)(uintptr_t)glyphs };
    ipo_syscall(IPO_SYSCALL_FONT_LOAD, 3u, args);
}

const char* vga_font_get_current_name(void) {
    static char font_name[64] = "Default";
    uint32_t cnt = 0;
    uint32_t args[3] = { (uint32_t)(uintptr_t)font_name, (uint32_t)sizeof(font_name), (uint32_t)(uintptr_t)&cnt };
    if (ipo_syscall(IPO_SYSCALL_FONT_GET_INFO, 3u, args) == 0) {
        return font_name;
    }
    return "Default";
}

uint32_t vga_font_get_glyph_count(void) {
    char font_name[64];
    uint32_t cnt = 256;
    uint32_t args[3] = { (uint32_t)(uintptr_t)font_name, (uint32_t)sizeof(font_name), (uint32_t)(uintptr_t)&cnt };
    if (ipo_syscall(IPO_SYSCALL_FONT_GET_INFO, 3u, args) == 0) {
        return cnt;
    }
    return 256;
}

uint8_t utf8_to_vga_glyph(const char *str, size_t str_len, size_t *out_bytes) {
    if (!str || str_len == 0) {
        if (out_bytes) *out_bytes = 0;
        return ' ';
    }
    unsigned char b1 = (unsigned char)str[0];
    if (b1 < 0x80) {
        if (out_bytes) *out_bytes = 1;
        return b1;
    }

    uint32_t codepoint = 0;
    size_t bytes = 1;

    if ((b1 & 0xE0) == 0xC0) {
        bytes = 2;
        if (str_len >= 2) {
            unsigned char b2 = (unsigned char)str[1];
            codepoint = ((b1 & 0x1F) << 6) | (b2 & 0x3F);
        } else {
            codepoint = b1;
        }
    } else if ((b1 & 0xF0) == 0xE0) {
        bytes = 3;
        if (str_len >= 3) {
            unsigned char b2 = (unsigned char)str[1];
            unsigned char b3 = (unsigned char)str[2];
            codepoint = ((b1 & 0x0F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
        } else {
            codepoint = b1;
        }
    } else if ((b1 & 0xF8) == 0xF0) {
        bytes = 4;
        if (str_len >= 4) {
            unsigned char b2 = (unsigned char)str[1];
            unsigned char b3 = (unsigned char)str[2];
            unsigned char b4 = (unsigned char)str[3];
            codepoint = ((b1 & 0x07) << 18) | ((b2 & 0x3F) << 12) | ((b3 & 0x3F) << 6) | (b4 & 0x3F);
        } else {
            codepoint = b1;
        }
    } else {
        bytes = 1;
        codepoint = b1;
    }

    if (out_bytes) *out_bytes = bytes;

    uint32_t bytes_consumed = (uint32_t)bytes;
    uint32_t args[3] = { (uint32_t)(uintptr_t)str, (uint32_t)str_len, (uint32_t)(uintptr_t)&bytes_consumed };
    int glyph = ipo_syscall(IPO_SYSCALL_VGA_GLYPH, 3u, args);
    if (glyph >= 0 && glyph <= 255) {
        if (out_bytes && bytes_consumed > 0) *out_bytes = bytes_consumed;
        return (uint8_t)glyph;
    }

    int slot = ipo_vga_glyph(codepoint);
    if (slot >= 0 && slot <= 255) {
        return (uint8_t)slot;
    }

    if (codepoint == 0x2014 || codepoint == 0x2013 || codepoint == 0x2015 || codepoint == 0x2212 ||
        codepoint == 0x2010 || codepoint == 0x2011 || codepoint == 0x2012) {
        return '-';
    }
    if (codepoint == 0x2026) return '.';
    if (codepoint == 0x00AB || codepoint == 0x00BB || codepoint == 0x201C || codepoint == 0x201D || codepoint == 0x201E) return '"';
    if (codepoint == 0x2022) return '*';

    return (codepoint < 0x80) ? (uint8_t)codepoint : '?';
}

const uint8_t* vga_font_get_cached_glyph(uint8_t char_code) {
    /* Standard 8x16 IBM PC font fallback data */
    extern const uint8_t vga_default_font_data[256][16];
    return vga_default_font_data[char_code];
}

/* VBE BGA userland stubs */
bool vbe_bga_is_available(void) { return false; }
int vbe_bga_get_bpp(void) { return 0; }
void *vbe_bga_get_framebuffer(void) { return NULL; }
bool vbe_bga_is_enabled(void) { return false; }
uint32_t vbe_bga_get_lfb_address(void) { return 0; }
bool vbe_bga_set_mode(int width, int height, int bpp) { (void)width; (void)height; (void)bpp; return false; }
void vbe_bga_restore_text_mode(void) {}
int vbe_bga_get_width(void) { return 0; }
int vbe_bga_get_height(void) { return 0; }
bool vbe_bga_probe(vbe_bga_info_t *out_info) { (void)out_info; return false; }

