#include <vga.h>
#include <ioport.h>
#include <stdio.h>
#include <system/timer.h>

static uint16_t cursor_hw_offset = VGA_START_CURSOR_POSITION;
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

    uint8_t cursor_glyph[16];
    for (int row = 0; row < 16; row++) {
        uint8_t base = src ? src[row] : 0x00;
        /* Leftmost pixel (bit 7) is 1 for the vertical bar cursor between characters */
        cursor_glyph[row] = base | 0x80;
    }

    vga_load_font(VGA_CURSOR_GLYPH_SLOT, 1, cursor_glyph);
}

static void vga_cursor_erase(void) {
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

// Sets the cursor position
// offset = row * VGA_WIDTH + col
void vga_set_cursor(uint16_t offset) {
    if (offset >= VGA_WIDTH * VGA_HEIGHT) {
        offset = VGA_WIDTH * VGA_HEIGHT - 1;
    }

    // Write hardware cursor position registers
    outb(0x3D4, 0x0E);
    outb(0x3D5, (offset >> 8) & 0xFF);
    
    outb(0x3D4, 0x0F);
    outb(0x3D5, offset & 0xFF);

    // Disable CRTC hardware horizontal cursor
    outb(0x3D4, 0x0A);
    uint8_t crtc_val = inb(0x3D5);
    outb(0x3D5, crtc_val | 0x20);

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

// Shows the cursor
void vga_show_cursor(void) {
    outb(0x3D4, 0x0A);
    uint8_t crtc_val = inb(0x3D5);
    outb(0x3D5, crtc_val | 0x20);

    cursor_is_visible = true;
    cursor_blink_on = true;
    cursor_last_blink_ms = timer_millis();
    vga_cursor_draw();
}

// Hides the cursor
void vga_hide_cursor(void) {
    outb(0x3D4, 0x0A);
    uint8_t crtc_val = inb(0x3D5);
    outb(0x3D5, crtc_val | 0x20);

    cursor_is_visible = false;
    vga_cursor_erase();
    last_cursor_glyph_char = -1;
}

void vga_cursor_reset(void) {
    vga_cursor_erase();

    /* Sanitize any stale cursor glyph slot (0x1F) characters in VGA memory */
    volatile uint16_t *vga = VGA_MEMORY;
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        if ((vga[i] & 0xFF) == VGA_CURSOR_GLYPH_SLOT) {
            uint8_t attr = (vga[i] >> 8) & 0xFF;
            vga[i] = ((uint16_t)attr << 8) | ' ';
        }
    }

    /* Force reset Plane 2 font slot 0x1F to clean vertical bar cursor */
    uint8_t cursor_glyph[16];
    for (int row = 0; row < 16; row++) {
        cursor_glyph[row] = 0x80;
    }
    vga_load_font(VGA_CURSOR_GLYPH_SLOT, 1, cursor_glyph);

    last_cursor_glyph_char = ' ';
    cursor_saved_char = ' ';
    cursor_saved_vga_entry = vga_entry(' ', VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    cursor_is_rendered = false;
    cursor_is_visible = false;
    cursor_blink_on = true;
    cursor_last_blink_ms = timer_millis();

    /* Disable CRTC hardware horizontal cursor */
    outb(0x3D4, 0x0A);
    uint8_t crtc_val = inb(0x3D5);
    outb(0x3D5, crtc_val | 0x20);
}

void vga_cursor_blink_tick(void) {
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
    if (!cursor_is_visible) return;
    cursor_blink_on = true;
    cursor_last_blink_ms = timer_millis();
    vga_cursor_draw();
}

void vga_clear(enum vga_color fg, enum vga_color bg, bool show_cursor, int cursor_position) {
    vga_hide_cursor();

    volatile uint16_t* vga = VGA_MEMORY;
    uint16_t blank = vga_entry(0x00, fg, bg);

    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga[i] = blank;
    }

    if (show_cursor) {
        vga_set_cursor(cursor_position);
        vga_show_cursor();
    }
}

uint16_t vga_get_cursor_position(void) {
    outb(0x3D4, 0x0E);
    uint16_t pos = ((uint16_t)inb(0x3D5)) << 8;
    outb(0x3D4, 0x0F);
    pos |= (uint16_t)inb(0x3D5);
    if (pos < VGA_WIDTH * VGA_HEIGHT) {
        cursor_hw_offset = pos;
    }
    return cursor_hw_offset;
}

bool vga_is_cursor_visible(void) {
    outb(0x3D4, 0x0A);
    if (inb(0x3D5) & 0x20) {
        cursor_is_visible = false;
        return false;
    }
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