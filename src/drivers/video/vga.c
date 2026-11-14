#include <vga.h>
#include <vga_gfx.h>
#include <driver/vbe_bga.h>
#include <ioport.h>
#include <system/timer.h>
#include <memory/kmalloc.h>

static uint16_t s_vga_hw_width = 80;
static uint16_t s_vga_hw_height = 25;
static uint32_t s_vga_hw_vram_addr = 0xB8000u;
static uint8_t  s_vga_hw_char_height = 16;
static bool     s_vga_hw_probed = false;

static const uint8_t s_cursor_bar_glyph[16] = {
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
};

static uint16_t vga_get_crtc_port(void) {
    return (inb(0x3CC) & 0x01u) ? 0x3D4u : 0x3B4u;
}

static void vga_disable_hw_cursor(void) {
    uint16_t crtc_idx = vga_get_crtc_port();
    uint16_t crtc_dat = (uint16_t)(crtc_idx + 1u);
    outb(crtc_idx, 0x0Au);
    uint8_t crtc_val = inb(crtc_dat);
    outb(crtc_dat, crtc_val | 0x20u);
}

void vga_probe_text_hardware(void) {
    /* Miscellaneous Output Register (0x3CC): bit 0 selects CRTC address 0x3D4 vs 0x3B4 */
    uint16_t crtc_idx = vga_get_crtc_port();
    uint16_t crtc_dat = (uint16_t)(crtc_idx + 1u);

    /* Graphics Controller register 0x06: Miscellaneous Graphics Register */
    outb(0x3CE, 0x06);
    uint8_t gc6 = inb(0x3CF);

    /* Only sample text-mode geometry while hardware is in alphanumeric mode */
    if ((gc6 & 0x01u) != 0u || vbe_bga_is_enabled()) {
        return;
    }

    /* Bits 3..2 (Memory Map Select): 0/1 -> 0xA0000, 2 -> 0xB0000, 3 -> 0xB8000 */
    uint32_t aperture = 0u;
    switch ((gc6 >> 2) & 0x03u) {
        case 0:
        case 1: aperture = 0xA0000u; break;
        case 2: aperture = 0xB0000u; break;
        case 3:
        default: aperture = 0xB8000u; break;
    }

    /* CRTC 0x0C/0x0D: Display Start Address (in character words) */
    outb(crtc_idx, 0x0Cu);
    uint16_t start_hi = inb(crtc_dat);
    outb(crtc_idx, 0x0Du);
    uint16_t start_lo = inb(crtc_dat);
    uint16_t start_cell = (uint16_t)((start_hi << 8) | start_lo);

    /* CRTC 0x01: Horizontal Display End = columns - 1 */
    outb(crtc_idx, 0x01u);
    uint8_t hde = inb(crtc_dat);
    uint16_t cols = (uint16_t)hde + 1u;

    /* CRTC 0x12 (VDE low 8) + 0x07 (Overflow bits 1,6) + 0x09 (Max Scan Line) */
    outb(crtc_idx, 0x12u);
    uint8_t vde_lo = inb(crtc_dat);
    outb(crtc_idx, 0x07u);
    uint8_t ovf = inb(crtc_dat);
    uint32_t vde = (uint32_t)vde_lo
                 | (((uint32_t)ovf & 0x02u) << 7)
                 | (((uint32_t)ovf & 0x40u) << 3);

    outb(crtc_idx, 0x09u);
    uint8_t msl = inb(crtc_dat);
    uint32_t total_scanlines = vde + 1u;
    if (msl & 0x80u) {
        total_scanlines /= 2u;
    }
    uint32_t char_h = (uint32_t)(msl & 0x1Fu) + 1u;
    uint16_t rows = (char_h > 0u) ? (uint16_t)(total_scanlines / char_h) : 0u;

    if (cols > 0u && rows > 0u && aperture != 0u) {
        s_vga_hw_width = cols;
        s_vga_hw_height = rows;
        s_vga_hw_vram_addr = aperture + (uint32_t)start_cell * 2u;
        s_vga_hw_char_height = (uint8_t)char_h;
        s_vga_hw_probed = true;
    }
}

uint16_t vga_get_width(void) {
    if (!s_vga_hw_probed) {
        vga_probe_text_hardware();
    }
    return s_vga_hw_width;
}

uint16_t vga_get_height(void) {
    if (!s_vga_hw_probed) {
        vga_probe_text_hardware();
    }
    return s_vga_hw_height;
}

volatile uint16_t *vga_get_memory(void) {
    if (!s_vga_hw_probed) {
        vga_probe_text_hardware();
    }
    return (volatile uint16_t *)(uintptr_t)s_vga_hw_vram_addr;
}

uint8_t vga_get_char_height(void) {
    if (!s_vga_hw_probed) {
        vga_probe_text_hardware();
    }
    return s_vga_hw_char_height;
}

void vga_get_text_mode_info(uint16_t *out_w, uint16_t *out_h, uint32_t *out_vram_addr) {
    vga_probe_text_hardware();
    if (out_w) *out_w = s_vga_hw_width;
    if (out_h) *out_h = s_vga_hw_height;
    if (out_vram_addr) *out_vram_addr = s_vga_hw_vram_addr;
}

static uint16_t cursor_hw_offset = 0;
static bool cursor_is_visible = false;
static bool cursor_is_rendered = false;
static uint16_t cursor_rendered_offset = 0;
static uint16_t cursor_saved_vga_entry = 0;
static uint8_t cursor_saved_char = ' ';
static uint32_t cursor_last_blink_ms = 0;
static bool cursor_blink_on = true;
static int16_t last_cursor_glyph_char = -1;

static void vga_update_cursor_glyph(uint8_t src_char) {
    if (last_cursor_glyph_char == (int16_t)src_char) {
        return;
    }
    last_cursor_glyph_char = (int16_t)src_char;

    const uint8_t *src = (src_char == 0x00 || src_char == ' ' || src_char == VGA_CURSOR_GLYPH_SLOT) ? NULL : vga_font_get_cached_glyph(src_char);

    uint8_t *cursor_glyph = (uint8_t *)kmalloc(16u);
    if (cursor_glyph) {
        for (int row = 0; row < 16; row++) {
            uint8_t base = src ? src[row] : 0x00;
            /* Leftmost pixel (bit 7) forms the vertical bar cursor */
            cursor_glyph[row] = base | 0x80;
        }
        vga_load_font(VGA_CURSOR_GLYPH_SLOT, 1, cursor_glyph);
        kfree(cursor_glyph);
    } else {
        vga_load_font(VGA_CURSOR_GLYPH_SLOT, 1, s_cursor_bar_glyph);
    }
}

void vga_cursor_erase(void) {
    if (vga_is_graphics_mode()) return;
    if (!cursor_is_rendered) return;
    volatile uint16_t *vga = VGA_MEMORY;
    if (cursor_rendered_offset < VGA_WIDTH * VGA_HEIGHT) {
        if ((vga[cursor_rendered_offset] & 0xFF) == VGA_CURSOR_GLYPH_SLOT) {
            vga[cursor_rendered_offset] = cursor_saved_vga_entry;
        }
    }
    cursor_is_rendered = false;
}

static void vga_cursor_draw(void) {
    if (vga_is_graphics_mode()) return;
    if (!cursor_is_visible) return;
    if (cursor_hw_offset >= VGA_WIDTH * VGA_HEIGHT) return;

    volatile uint16_t *vga = VGA_MEMORY;
    uint16_t cur_entry = vga[cursor_hw_offset];
    uint8_t ch = cur_entry & 0xFF;
    uint8_t attr = (cur_entry >> 8) & 0xFF;

    if (ch != VGA_CURSOR_GLYPH_SLOT) {
        cursor_saved_vga_entry = cur_entry;
        cursor_saved_char = ch;
    } else if (!cursor_is_rendered || cursor_rendered_offset != cursor_hw_offset) {
        cursor_saved_char = ' ';
        cursor_saved_vga_entry = ((uint16_t)attr << 8) | ' ';
    }

    uint8_t fg = attr & 0x0F;
    uint8_t bg = (attr >> 4) & 0x0F;
    if (fg == bg) {
        fg = (bg == VGA_COLOR_WHITE) ? VGA_COLOR_BLACK : VGA_COLOR_WHITE;
        attr = (bg << 4) | fg;
    }

    vga_update_cursor_glyph(cursor_saved_char);

    vga[cursor_hw_offset] = ((uint16_t)attr << 8) | VGA_CURSOR_GLYPH_SLOT;
    cursor_rendered_offset = cursor_hw_offset;
    cursor_is_rendered = true;
}

void vga_set_cursor(uint16_t offset) {
    uint16_t max_cells = (uint16_t)(VGA_WIDTH * VGA_HEIGHT);
    if (max_cells > 0 && offset >= max_cells) {
        offset = max_cells - 1u;
    }

    if (vga_is_graphics_mode()) {
        cursor_hw_offset = offset;
        return;
    }

    uint16_t crtc_idx = vga_get_crtc_port();
    uint16_t crtc_dat = (uint16_t)(crtc_idx + 1u);
    outb(crtc_idx, 0x0E);
    outb(crtc_dat, (offset >> 8) & 0xFF);
    outb(crtc_idx, 0x0F);
    outb(crtc_dat, offset & 0xFF);

    vga_disable_hw_cursor();

    if (cursor_is_visible) {
        if (cursor_is_rendered && cursor_rendered_offset != offset) {
            vga_cursor_erase();
        }
        cursor_hw_offset = offset;
        cursor_blink_on = true;
        cursor_last_blink_ms = timer_millis();
        vga_cursor_draw();
    } else {
        cursor_hw_offset = offset;
    }
}

void vga_show_cursor(void) {
    if (vga_is_graphics_mode()) { cursor_is_visible = true; return; }
    vga_disable_hw_cursor();

    cursor_is_visible = true;
    cursor_blink_on = true;
    cursor_last_blink_ms = timer_millis();
    vga_cursor_draw();
}

void vga_hide_cursor(void) {
    if (vga_is_graphics_mode()) { cursor_is_visible = false; return; }
    vga_disable_hw_cursor();

    cursor_is_visible = false;
    vga_cursor_erase();
    last_cursor_glyph_char = -1;
}

void vga_cursor_reset(void) {
    if (vga_is_graphics_mode()) return;
    vga_cursor_erase();

    /* Sanitize stale cursor glyph slots in VGA memory */
    volatile uint16_t *vga = VGA_MEMORY;
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        if ((vga[i] & 0xFF) == VGA_CURSOR_GLYPH_SLOT) {
            uint8_t attr = (vga[i] >> 8) & 0xFF;
            vga[i] = ((uint16_t)attr << 8) | ' ';
        }
    }

    vga_load_font(VGA_CURSOR_GLYPH_SLOT, 1, s_cursor_bar_glyph);

    last_cursor_glyph_char = ' ';
    cursor_saved_char = ' ';
    cursor_saved_vga_entry = vga_entry(' ', VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    cursor_is_rendered = false;
    cursor_is_visible = false;
    cursor_blink_on = true;
    cursor_last_blink_ms = timer_millis();

    vga_disable_hw_cursor();
}

void vga_cursor_blink_tick(void) {
    /* Never touch text-mode VRAM or CRTC registers while in graphics mode */
    if (vga_is_graphics_mode()) return;
    if (!cursor_is_visible) return;

    uint32_t now = timer_millis();
    if (now - cursor_last_blink_ms < 500) {
        return;
    }
    cursor_last_blink_ms = now;
    cursor_blink_on = !cursor_blink_on;

    if (cursor_blink_on) {
        vga_cursor_draw();
    } else {
        vga_cursor_erase();
    }
}

void vga_cursor_reset_blink(void) {
    if (vga_is_graphics_mode()) return;
    if (!cursor_is_visible) return;
    cursor_blink_on = true;
    cursor_last_blink_ms = timer_millis();
    vga_cursor_draw();
}

void vga_clear(enum vga_color fg, enum vga_color bg, bool show_cursor, int cursor_position) {
    vga_hide_cursor();

    volatile uint16_t* vga = VGA_MEMORY;
    if (fg == bg) {
        fg = VGA_COLOR_LIGHT_GREY;
        bg = VGA_COLOR_BLACK;
    }
    uint16_t blank = vga_entry(' ', fg, bg);

    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga[i] = blank;
    }

    if (show_cursor) {
        vga_set_cursor(cursor_position < 0 ? 0 : (uint16_t)cursor_position);
        vga_show_cursor();
    }
}

uint16_t vga_get_cursor_position(void) {
    if (vga_is_graphics_mode()) {
        return cursor_hw_offset;
    }
    uint16_t crtc_idx = vga_get_crtc_port();
    uint16_t crtc_dat = (uint16_t)(crtc_idx + 1u);
    outb(crtc_idx, 0x0E);
    uint16_t pos = ((uint16_t)inb(crtc_dat)) << 8;
    outb(crtc_idx, 0x0F);
    pos |= (uint16_t)inb(crtc_dat);
    if (pos < VGA_WIDTH * VGA_HEIGHT) {
        cursor_hw_offset = pos;
    }
    return cursor_hw_offset;
}

bool vga_is_cursor_visible(void) {
    return cursor_is_visible;
}

uint16_t vga_increment_cursor_position(void) {
    uint16_t cursor = cursor_hw_offset;
    cursor++;
    if (cursor >= VGA_WIDTH * VGA_HEIGHT) {
        cursor = VGA_WIDTH * VGA_HEIGHT - 1;
    }
    vga_set_cursor(cursor);
    return cursor;
}

uint16_t vga_decrement_cursor_position(void) {
    uint16_t cursor = cursor_hw_offset;
    if (cursor > 0) {
        cursor--;
    }
    vga_set_cursor(cursor);
    return cursor;
}

void vga_sanitize_text_vram(void) {
    volatile uint16_t *vga = VGA_MEMORY;
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        uint16_t entry = vga[i];
        uint8_t ch = (uint8_t)(entry & 0xFF);
        uint8_t attr = (uint8_t)(entry >> 8);
        uint8_t fg = attr & 0x0F;
        uint8_t bg_attr = (attr >> 4) & 0x0F;

        if (ch == 0x00) {
            vga[i] = (attr != 0 && fg != bg_attr) ? (((uint16_t)attr << 8) | ' ') : 0x0720;
        } else if (entry == 0xFFFF || attr == 0xFF || fg == bg_attr) {
            vga[i] = 0x0720;
        } else if (ch == VGA_CURSOR_GLYPH_SLOT && (!cursor_is_rendered || i != cursor_rendered_offset)) {
            vga[i] = ((uint16_t)attr << 8) | ' ';
        }
    }
}