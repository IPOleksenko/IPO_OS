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
#include <syscall.h>
#include <memory/kmalloc.h>
#include <driver/input/keymap/dynamic_keymap.h>
#include <kernel/process.h>
#include <driver/vbe_bga.h>
#include <driver/input/mouse.h>

#define VGA_VRAM_ADDR 0xA0000

static inline bool is_foreground_process(void) {
    return process_is_foreground();
}

typedef struct {
    uint8_t *data;
    size_t count;
    size_t capacity;
} vga_dynbuf8_t;

typedef struct {
    uint16_t *data;
    size_t count;
    size_t capacity;
} vga_dynbuf16_t;

typedef struct {
    uint8_t (*data)[3];
    size_t count;
    size_t capacity;
} vga_dyndac_t;

static bool vga_dynbuf8_reserve(vga_dynbuf8_t *buf, size_t min_cap) {
    if (buf->capacity >= min_cap) {
        return true;
    }
    size_t new_cap = buf->capacity ? buf->capacity : 16;
    while (new_cap < min_cap) {
        new_cap = (new_cap * 3) / 2 + 8;
    }
    uint8_t *new_data = (uint8_t *)kmalloc(new_cap * sizeof(uint8_t));
    if (!new_data) {
        return false;
    }
    if (buf->data && buf->count > 0) {
        memcpy(new_data, buf->data, buf->count * sizeof(uint8_t));
    }
    if (new_cap > buf->count) {
        memset(new_data + buf->count, 0, (new_cap - buf->count) * sizeof(uint8_t));
    }
    if (buf->data) {
        kfree(buf->data);
    }
    buf->data = new_data;
    buf->capacity = new_cap;
    return true;
}

static void vga_dynbuf8_set(vga_dynbuf8_t *buf, size_t index, uint8_t val) {
    if (index >= buf->capacity) {
        if (!vga_dynbuf8_reserve(buf, index + 1)) {
            return;
        }
    }
    buf->data[index] = val;
    if (index + 1 > buf->count) {
        buf->count = index + 1;
    }
}

static uint8_t vga_dynbuf8_get(const vga_dynbuf8_t *buf, size_t index, uint8_t def_val) {
    if (!buf->data || index >= buf->count) {
        return def_val;
    }
    return buf->data[index];
}

static bool vga_dynbuf16_reserve(vga_dynbuf16_t *buf, size_t min_cap) {
    if (buf->capacity >= min_cap) {
        return true;
    }
    size_t new_cap = buf->capacity ? buf->capacity : 64;
    while (new_cap < min_cap) {
        new_cap = (new_cap * 3) / 2 + 16;
    }
    uint16_t *new_data = (uint16_t *)kmalloc(new_cap * sizeof(uint16_t));
    if (!new_data) {
        return false;
    }
    if (buf->data && buf->count > 0) {
        memcpy(new_data, buf->data, buf->count * sizeof(uint16_t));
    }
    if (new_cap > buf->count) {
        memset(new_data + buf->count, 0, (new_cap - buf->count) * sizeof(uint16_t));
    }
    if (buf->data) {
        kfree(buf->data);
    }
    buf->data = new_data;
    buf->capacity = new_cap;
    return true;
}

static void vga_dynbuf16_set(vga_dynbuf16_t *buf, size_t index, uint16_t val) {
    if (index >= buf->capacity) {
        if (!vga_dynbuf16_reserve(buf, index + 1)) {
            return;
        }
    }
    buf->data[index] = val;
    if (index + 1 > buf->count) {
        buf->count = index + 1;
    }
}

static uint16_t vga_dynbuf16_get(const vga_dynbuf16_t *buf, size_t index, uint16_t def_val) {
    if (!buf->data || index >= buf->count) {
        return def_val;
    }
    return buf->data[index];
}

static bool vga_dyndac_reserve(vga_dyndac_t *buf, size_t min_cap) {
    if (buf->capacity >= min_cap) {
        return true;
    }
    size_t new_cap = buf->capacity ? buf->capacity : 32;
    while (new_cap < min_cap) {
        new_cap = (new_cap * 3) / 2 + 16;
    }
    uint8_t (*new_data)[3] = (uint8_t (*)[3])kmalloc(new_cap * sizeof(uint8_t[3]));
    if (!new_data) {
        return false;
    }
    if (buf->data && buf->count > 0) {
        memcpy(new_data, buf->data, buf->count * sizeof(uint8_t[3]));
    }
    if (new_cap > buf->count) {
        memset(new_data + buf->count, 0, (new_cap - buf->count) * sizeof(uint8_t[3]));
    }
    if (buf->data) {
        kfree(buf->data);
    }
    buf->data = new_data;
    buf->capacity = new_cap;
    return true;
}

static void vga_dyndac_set(vga_dyndac_t *buf, size_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (index >= buf->capacity) {
        if (!vga_dyndac_reserve(buf, index + 1)) {
            return;
        }
    }
    buf->data[index][0] = r;
    buf->data[index][1] = g;
    buf->data[index][2] = b;
    if (index + 1 > buf->count) {
        buf->count = index + 1;
    }
}

static void vga_dyndac_get(const vga_dyndac_t *buf, size_t index, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (!buf->data || index >= buf->count) {
        if (r) *r = 0;
        if (g) *g = 0;
        if (b) *b = 0;
        return;
    }
    if (r) *r = buf->data[index][0];
    if (g) *g = buf->data[index][1];
    if (b) *b = buf->data[index][2];
}

static uint8_t saved_misc = 0x67;
static vga_dynbuf8_t saved_seq = {0};
static vga_dynbuf8_t saved_crtc = {0};
static vga_dynbuf8_t saved_gc = {0};
static vga_dynbuf8_t saved_ac = {0};
static vga_dyndac_t saved_dac = {0};
static vga_dynbuf8_t saved_font_plane2 = {0};
static vga_dynbuf16_t saved_text_vram = {0};
static uint16_t saved_cursor_pos = 0;
static bool saved_cursor_visible = false;
static bool has_saved_text_vram = false;
static bool text_state_saved = false;
static bool is_gfx = false;
static int requested_gfx_width = VGA_TEXT_PIXEL_WIDTH;
static int requested_gfx_height = VGA_TEXT_PIXEL_HEIGHT;

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
    /* If graphics mode is active or text VRAM was already saved, NEVER read 0xB8000!
     * In Mode 13h, 0xB8000 is unmapped by hardware and reading it yields 0x2020 bus float. */
    if (vga_is_graphics_mode() || has_saved_text_vram) {
        return;
    }

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
        vga_dynbuf16_set(&saved_text_vram, i, entry);
    }
    has_saved_text_vram = true;

    if (text_state_saved) return;

    /* Misc Output */
    saved_misc = inb(0x3CC);

    /* Sequencer */
    for (uint8_t i = 0; i < 5; i++) {
        outb(0x3C4, i);
        vga_dynbuf8_set(&saved_seq, i, inb(0x3C5));
    }

    /* CRTC */
    for (uint8_t i = 0; i < 25; i++) {
        outb(0x3D4, i);
        vga_dynbuf8_set(&saved_crtc, i, inb(0x3D5));
    }

    /* Graphics Controller */
    for (uint8_t i = 0; i < 9; i++) {
        outb(0x3CE, i);
        vga_dynbuf8_set(&saved_gc, i, inb(0x3CF));
    }

    /* Attribute Controller */
    for (uint8_t i = 0; i < 21; i++) {
        inb(0x3DA);
        outb(0x3C0, i);
        vga_dynbuf8_set(&saved_ac, i, inb(0x3C1));
    }
    inb(0x3DA);
    outb(0x3C0, 0x20);

    /* DAC Palette (all 256 colors) */
    for (int i = 0; i < 256; i++) {
        outb(0x3C7, (uint8_t)i);
        uint8_t r = inb(0x3C9);
        uint8_t g = inb(0x3C9);
        uint8_t b = inb(0x3C9);
        vga_dyndac_set(&saved_dac, i, r, g, b);
    }

    /* Save Plane 2 Font (8KB: 256 glyphs * 32 bytes) */
    uint32_t font_flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(font_flags));

    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x04); // Plane 2
    outb(0x3C4, 0x04); outb(0x3C5, 0x07); // Sequential addressing (disable odd/even)
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);

    outb(0x3CE, 0x00); outb(0x3CF, 0x00);
    outb(0x3CE, 0x01); outb(0x3CF, 0x00);
    outb(0x3CE, 0x03); outb(0x3CF, 0x00);
    outb(0x3CE, 0x04); outb(0x3CF, 0x02); // Read map = Plane 2
    outb(0x3CE, 0x05); outb(0x3CF, 0x00); // Write mode 0
    outb(0x3CE, 0x06); outb(0x3CF, 0x04); // Map 64KB memory to 0xA0000 only
    outb(0x3CE, 0x08); outb(0x3CF, 0xFF);

    volatile uint8_t *font_vram = (volatile uint8_t *)0xA0000;
    for (int i = 0; i < 8192; i++) {
        vga_dynbuf8_set(&saved_font_plane2, i, font_vram[i]);
    }

    /* Restore normal text mode operation (Plane 0 & 1, 0xB8000) */
    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x03);
    outb(0x3C4, 0x04); outb(0x3C5, 0x03);
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);

    outb(0x3CE, 0x00); outb(0x3CF, 0x00);
    outb(0x3CE, 0x01); outb(0x3CF, 0x00);
    outb(0x3CE, 0x03); outb(0x3CF, 0x00);
    outb(0x3CE, 0x04); outb(0x3CF, 0x00);
    outb(0x3CE, 0x05); outb(0x3CF, 0x10);
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E); // 0xB8000
    outb(0x3CE, 0x08); outb(0x3CF, 0xFF);

    __asm__ volatile("push %0; popf" : : "r"(font_flags));

    text_state_saved = true;
    printf("[vga] Text mode hardware state saved (cursor_pos=%u, visible=%d).\n", saved_cursor_pos, (int)saved_cursor_visible);
}

void vga_set_mode_13h_hardware(void) {
    serial_printf("[vga] 13h hardware mode set (is_gfx=%d)\n", (int)is_gfx);
    if (vbe_bga_is_available()) {
        bool output_was_active = vga_is_graphics_mode() || vbe_bga_is_enabled();
        if (vbe_bga_is_enabled() &&
            vbe_bga_get_width() == requested_gfx_width &&
            vbe_bga_get_height() == requested_gfx_height &&
            vbe_bga_get_bpp() == 8) {
            is_gfx = true;
            return;
        }
        if (vbe_bga_set_mode(requested_gfx_width, requested_gfx_height, 8)) {
            is_gfx = true;
            vga_gfx_init_default_palette();
            mouse_set_bounds_from_display();
            return;
        }
        if (vbe_bga_is_enabled()) {
            is_gfx = true;
            mouse_set_bounds_from_display();
            serial_printf("[vga] keeping device-applied mode %dx%d\n",
                          vbe_bga_get_width(), vbe_bga_get_height());
            return;
        }
        if (output_was_active) {
            serial_printf("[vga] BGA did not apply requested mode %dx%d; keeping device state\n",
                          requested_gfx_width, requested_gfx_height);
            return;
        }
        serial_printf("[vga] requested BGA mode %dx%d unavailable; using Mode 13h\n",
                      requested_gfx_width, requested_gfx_height);
    }

    if (vbe_bga_is_enabled()) {
        vbe_bga_disable();
    }
    if (vga_is_graphics_mode()) {
        return;
    }
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
    mouse_set_bounds_from_display();
}

void vga_set_mode_text_hardware(void) {
    serial_printf("[vga] text hardware mode set (is_gfx=%d)\n", (int)is_gfx);
    vbe_bga_disable();
    /* Always restore hardware text mode registers, sequencer, CRTC, GC, AC, DAC and font */

    /* Misc */
    outb(0x3C2, 0x67);

    /* Sequencer (Standard Mode 03h) */
    static const uint8_t text_seq[5] = {0x03, 0x00, 0x03, 0x00, 0x02};
    for (uint8_t i = 0; i < 5; i++) {
        seq_write(i, text_seq[i]);
    }

    /* Unlock CRTC */
    outb(0x3D4, 0x11);
    outb(0x3D5, inb(0x3D5) & ~0x80);

    uint16_t saved_cur_pos = vga_get_cursor_position();

    /* CRTC (Standard 80x25 text mode) */
    static const uint8_t text_crtc[25] = {
        0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
        0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
        0xFF
    };
    for (uint8_t i = 0; i < 25; i++) {
        if (i == 0x0E) {
            crtc_write(i, (saved_cur_pos >> 8) & 0xFF);
        } else if (i == 0x0F) {
            crtc_write(i, saved_cur_pos & 0xFF);
        } else {
            crtc_write(i, text_crtc[i]);
        }
    }

    /* GC (Standard text mode) */
    static const uint8_t text_gc[9] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF};
    for (uint8_t i = 0; i < 9; i++) {
        gc_write(i, text_gc[i]);
    }

    /* AC (Standard text mode) */
    static const uint8_t text_ac[21] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
        0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
        0x0C, 0x00, 0x0F, 0x08, 0x00
    };
    for (uint8_t i = 0; i < 21; i++) {
        ac_write(i, text_ac[i]);
    }
    inb(0x3DA);
    outb(0x3C0, 0x20);

    /* DAC (all 256 colors) */
    size_t dac_count = saved_dac.count ? saved_dac.count : 256;
    for (size_t i = 0; i < dac_count; i++) {
        uint8_t r = 0, g = 0, b = 0;
        vga_dyndac_get(&saved_dac, i, &r, &g, &b);
        outb(0x3C8, (uint8_t)i);
        outb(0x3C9, r);
        outb(0x3C9, g);
        outb(0x3C9, b);
    }

    /* Restore Plane 2 Font */
    uint32_t font_flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(font_flags));

    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x04); // Write only to Plane 2
    outb(0x3C4, 0x04); outb(0x3C5, 0x07); // Sequential addressing
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);

    outb(0x3CE, 0x00); outb(0x3CF, 0x00);
    outb(0x3CE, 0x01); outb(0x3CF, 0x00);
    outb(0x3CE, 0x03); outb(0x3CF, 0x00);
    outb(0x3CE, 0x04); outb(0x3CF, 0x02); // Read map = Plane 2
    outb(0x3CE, 0x05); outb(0x3CF, 0x00); // Write mode 0
    outb(0x3CE, 0x06); outb(0x3CF, 0x04); // Map 64KB memory to 0xA0000 only
    outb(0x3CE, 0x08); outb(0x3CF, 0xFF);

    volatile uint8_t *dest = (volatile uint8_t *)0xA0000;
    size_t font_count = saved_font_plane2.count ? saved_font_plane2.count : 8192;
    for (size_t i = 0; i < font_count; i++) {
        dest[i] = vga_dynbuf8_get(&saved_font_plane2, i, 0);
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

    __asm__ volatile("push %0; popf" : : "r"(font_flags));

    /* Re-apply active custom glyphs directly from source bitmaps to eliminate any mode 13h residues */
    vga_font_reapply_active();
    dynamic_keymap_reapply_fonts();

    /* Drain keyboard controller buffer */
    for (int d = 0; d < 16 && (inb(0x64) & 0x01); d++) {
        (void)inb(0x60);
    }

    uint16_t active_cursor = has_saved_text_vram ? saved_cursor_pos : vga_get_cursor_position();
    bool active_visible = has_saved_text_vram ? saved_cursor_visible : vga_is_cursor_visible();

    if (has_saved_text_vram) {
        volatile uint16_t *text_vram = (volatile uint16_t *)0xB8000;
        size_t text_count = saved_text_vram.count ? saved_text_vram.count : (80 * 25);
        for (size_t i = 0; i < text_count; i++) {
            uint16_t entry = vga_dynbuf16_get(&saved_text_vram, i, 0x0720);
            uint8_t bg = (uint8_t)((entry >> 12) & 0x07);
            uint8_t ch = (uint8_t)(entry & 0xFF);
            if (bg == 2 || ((ch == ' ' || ch == 0x00) && bg != 0)) {
                entry = (entry & 0x0FFF) | 0x0000;
                if (ch == 0x00) entry = 0x0720;
            } else if (ch == VGA_CURSOR_GLYPH_SLOT) {
                entry = (entry & 0xFF00) | ' ';
            }
            text_vram[i] = entry;
        }
    }

    vga_set_cursor(active_cursor);
    if (active_visible) {
        vga_show_cursor();
    } else {
        vga_hide_cursor();
    }

    is_gfx = false;
    has_saved_text_vram = false;
    vga_sanitize_text_vram();
    mouse_set_bounds_from_display();
}

bool vga_is_graphics_mode(void) {
    if (vbe_bga_is_enabled()) return true;
    outb(0x3CE, 0x06);
    return (inb(0x3CF) & 0x01) != 0;
}

void vga_gfx_get_screen_bounds(int *out_w, int *out_h) {
    int w = VGA_TEXT_PIXEL_WIDTH;
    int h = VGA_TEXT_PIXEL_HEIGHT;
    bool graphics_mode = is_gfx || vga_is_graphics_mode();

    if (vbe_bga_is_available() && vbe_bga_is_enabled()) {
        int bga_w = vbe_bga_get_width();
        int bga_h = vbe_bga_get_height();
        w = bga_w;
        h = bga_h;
    } else if (graphics_mode) {
        w = VGA_GFX_WIDTH;
        h = VGA_GFX_HEIGHT;
    } else {
        w = VGA_TEXT_PIXEL_WIDTH;
        h = VGA_TEXT_PIXEL_HEIGHT;
    }

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

int vga_gfx_get_width(void) {
    int w = 0, h = 0;
    vga_gfx_get_screen_bounds(&w, &h);
    return w;
}

int vga_gfx_get_height(void) {
    int w = 0, h = 0;
    vga_gfx_get_screen_bounds(&w, &h);
    return h;
}

bool vga_gfx_set_resolution(int width, int height) {
    if (width < 0 || height < 0) return false;

    bool output_was_active = vga_is_graphics_mode() || vbe_bga_is_enabled();
    requested_gfx_width = width;
    requested_gfx_height = height;

    if (output_was_active) {
        vga_set_mode_13h_hardware();
        if (vbe_bga_is_enabled())
            return vbe_bga_get_width() == width &&
                   vbe_bga_get_height() == height &&
                   vbe_bga_get_bpp() == 8;

        return width == VGA_GFX_WIDTH && height == VGA_GFX_HEIGHT;
    }
    return true;
}

void vga_gfx_get_resolution(int *out_w, int *out_h) {
    if (vbe_bga_is_enabled()) {
        if (out_w) *out_w = vbe_bga_get_width();
        if (out_h) *out_h = vbe_bga_get_height();
        return;
    }

    if (vga_is_graphics_mode()) {
        vga_gfx_get_screen_bounds(out_w, out_h);
        return;
    }

    if (out_w) *out_w = requested_gfx_width;
    if (out_h) *out_h = requested_gfx_height;
}

void vga_set_mode_13h(void) {
    process_t *cur = process_get_current();
    process_t *fg = process_get_foreground();
    if (cur) cur->wants_graphics = true;
    if (!fg || cur == fg) {
        vga_set_mode_13h_hardware();
    }
}

void vga_set_mode_text(void) {
    process_t *cur = process_get_current();
    process_t *fg = process_get_foreground();
    if (cur) cur->wants_graphics = false;
    if (!fg || cur == fg) {
        if (vga_is_graphics_mode()) {
            vga_set_mode_text_hardware();
        }
    }
}

void vga_gfx_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    process_t *cur = process_get_current();
    if (cur) {
        if (!cur->gfx_palette_backup) {
            cur->gfx_palette_backup = (uint8_t *)kmalloc(768);
            if (cur->gfx_palette_backup) {
                memset(cur->gfx_palette_backup, 0, 768);
            }
        }
        if (cur->gfx_palette_backup) {
            cur->gfx_palette_backup[idx * 3 + 0] = r & 0x3F;
            cur->gfx_palette_backup[idx * 3 + 1] = g & 0x3F;
            cur->gfx_palette_backup[idx * 3 + 2] = b & 0x3F;
        }
    }
    if (!is_foreground_process()) return;
    outb(0x3C8, idx);
    outb(0x3C9, r & 0x3F);
    outb(0x3C9, g & 0x3F);
    outb(0x3C9, b & 0x3F);
}

static void vga_dac_write_entry(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    process_t *cur = process_get_current();
    if (cur) {
        if (!cur->gfx_palette_backup) {
            cur->gfx_palette_backup = (uint8_t *)kmalloc(768);
            if (cur->gfx_palette_backup) {
                memset(cur->gfx_palette_backup, 0, 768);
            }
        }
        if (cur->gfx_palette_backup) {
            cur->gfx_palette_backup[idx * 3 + 0] = r & 0x3F;
            cur->gfx_palette_backup[idx * 3 + 1] = g & 0x3F;
            cur->gfx_palette_backup[idx * 3 + 2] = b & 0x3F;
        }
    }
    outb(0x3C8, idx);
    outb(0x3C9, r & 0x3F);
    outb(0x3C9, g & 0x3F);
    outb(0x3C9, b & 0x3F);
}

void vga_gfx_init_default_palette(void) {
    /* 16 Standard Colors */
    vga_dac_write_entry(0,  0,  0,  0);
    vga_dac_write_entry(1,  0,  0, 42);
    vga_dac_write_entry(2,  0, 42,  0);
    vga_dac_write_entry(3,  0, 42, 42);
    vga_dac_write_entry(4, 42,  0,  0);
    vga_dac_write_entry(5, 42,  0, 42);
    vga_dac_write_entry(6, 42, 21,  0);
    vga_dac_write_entry(7, 42, 42, 42);
    vga_dac_write_entry(8, 21, 21, 21);
    vga_dac_write_entry(9, 21, 21, 63);
    vga_dac_write_entry(10, 21, 63, 21);
    vga_dac_write_entry(11, 21, 63, 63);
    vga_dac_write_entry(12, 63, 21, 21);
    vga_dac_write_entry(13, 63, 21, 63);
    vga_dac_write_entry(14, 63, 63, 21);
    vga_dac_write_entry(15, 63, 63, 63);

    /* 16..31: 16-level monochrome grayscale ramp */
    for (int i = 0; i < 16; i++) {
        uint8_t v = (uint8_t)(i * 63 / 15);
        vga_dac_write_entry((uint8_t)(16 + i), v, v, v);
    }

    /* 32..247: 6x6x6 RGB color cube (216 colors) */
    int idx = 32;
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                vga_dac_write_entry((uint8_t)idx++,
                                    (uint8_t)(r * 63 / 5),
                                    (uint8_t)(g * 63 / 5),
                                    (uint8_t)(b * 63 / 5));
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
    if (x2 <= x || y2 <= y) return;

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

static void vga_gfx_present(const uint8_t *buf,
                            bool scale_to_display,
                            int source_width,
                            int source_height) {
    if (!buf) {
        process_yield();
        return;
    }
    if (source_width <= 0 || source_height <= 0 ||
        (size_t)source_width > SIZE_MAX / (size_t)source_height) {
        process_yield();
        return;
    }

    process_t *cur = process_get_current();
    if (cur && process_get_separate_windows()) {
        if (!cur->gfx_vram_backup) {
            cur->gfx_vram_backup = (uint8_t *)kmalloc(VGA_GFX_SIZE);
        }
        if (cur->gfx_vram_backup) {
            memcpy(cur->gfx_vram_backup, buf, VGA_GFX_SIZE);
        }
    }
    if (!is_foreground_process()) {
        process_yield();
        return;
    }
    if (vbe_bga_is_enabled()) {
        if (vbe_bga_get_bpp() != 8) {
            process_yield();
            return;
        }
        int width = vbe_bga_get_width();
        int height = vbe_bga_get_height();
        uint8_t *framebuffer = (uint8_t *)vbe_bga_get_framebuffer();
        if (!framebuffer || width <= 0 || height <= 0) {
            process_yield();
            return;
        }

        if (scale_to_display) {
            if (source_width == width && source_height == height) {
                memcpy(framebuffer, buf, (size_t)width * height);
            } else {
                int *lut_x = (int *)kmalloc(sizeof(int) * width);
                if (lut_x) {
                    for (int x = 0; x < width; x++) {
                        lut_x[x] = (int)(((uint64_t)x * (uint32_t)source_width) / (uint32_t)width);
                    }
                    for (int y = 0; y < height; y++) {
                        int source_y = (int)(((uint64_t)y * (uint32_t)source_height) / (uint32_t)height);
                        uint8_t *dst = framebuffer + (size_t)y * (size_t)width;
                        const uint8_t *src = buf + (size_t)source_y * (size_t)source_width;
                        for (int x = 0; x < width; x++) {
                            dst[x] = src[lut_x[x]];
                        }
                    }
                    kfree(lut_x);
                }
            }
        } else {
            if (source_width == width && source_height == height) {
                memcpy(framebuffer, buf, (size_t)width * height);
            } else {
                int copy_width = width < source_width ? width : source_width;
                int copy_height = height < source_height ? height : source_height;
                int source_x = (source_width - copy_width) / 2;
                int source_y = (source_height - copy_height) / 2;
                int target_x = (width - copy_width) / 2;
                int target_y = (height - copy_height) / 2;
                for (int y = 0; y < target_y; y++) {
                    memset(framebuffer + (size_t)y * (size_t)width, 0, (size_t)width);
                }
                for (int y = 0; y < copy_height; y++) {
                    uint8_t *dst = framebuffer + (size_t)(target_y + y) * (size_t)width;
                    if (target_x > 0) {
                        memset(dst, 0, (size_t)target_x);
                    }
                    memcpy(dst + target_x,
                           buf + (size_t)(source_y + y) * (size_t)source_width + source_x,
                           (size_t)copy_width);
                    if (target_x + copy_width < width) {
                        memset(dst + target_x + copy_width, 0,
                               (size_t)(width - target_x - copy_width));
                    }
                }
                for (int y = target_y + copy_height; y < height; y++) {
                    memset(framebuffer + (size_t)y * (size_t)width, 0, (size_t)width);
                }
            }
        }
        process_yield();
        return;
    }
    if (!vga_is_graphics_mode()) return;

    volatile uint8_t *vram = (volatile uint8_t *)VGA_VRAM_ADDR;
    if (scale_to_display) {
        if (source_width == VGA_GFX_WIDTH && source_height == VGA_GFX_HEIGHT) {
            memcpy((void *)vram, buf, VGA_GFX_SIZE);
        } else {
            int lut_x[VGA_GFX_WIDTH];
            for (int x = 0; x < VGA_GFX_WIDTH; x++) {
                lut_x[x] = (int)(((uint64_t)x * (uint32_t)source_width) / VGA_GFX_WIDTH);
            }
            for (int y = 0; y < VGA_GFX_HEIGHT; y++) {
                int source_y = (int)(((uint64_t)y * (uint32_t)source_height) / VGA_GFX_HEIGHT);
                volatile uint8_t *dst = vram + y * VGA_GFX_WIDTH;
                const uint8_t *src = buf + (size_t)source_y * (size_t)source_width;
                for (int x = 0; x < VGA_GFX_WIDTH; x++) {
                    dst[x] = src[lut_x[x]];
                }
            }
        }
    } else {
        if (source_width == VGA_GFX_WIDTH && source_height == VGA_GFX_HEIGHT) {
            memcpy((void *)vram, buf, VGA_GFX_SIZE);
        } else {
            int copy_width = source_width < VGA_GFX_WIDTH ? source_width : VGA_GFX_WIDTH;
            int copy_height = source_height < VGA_GFX_HEIGHT ? source_height : VGA_GFX_HEIGHT;
            int source_x = (source_width - copy_width) / 2;
            int source_y = (source_height - copy_height) / 2;
            int target_x = (VGA_GFX_WIDTH - copy_width) / 2;
            int target_y = (VGA_GFX_HEIGHT - copy_height) / 2;
            for (int y = 0; y < target_y; y++) {
                memset((void *)(vram + (size_t)y * VGA_GFX_WIDTH), 0, VGA_GFX_WIDTH);
            }
            for (int y = 0; y < copy_height; y++) {
                memset((void *)(vram + (size_t)(target_y + y) * VGA_GFX_WIDTH),
                       0, VGA_GFX_WIDTH);
                memcpy((void *)(vram + (size_t)(target_y + y) * VGA_GFX_WIDTH + target_x),
                       buf + (size_t)(source_y + y) * (size_t)source_width + source_x,
                       (size_t)copy_width);
            }
            for (int y = target_y + copy_height; y < VGA_GFX_HEIGHT; y++) {
                memset((void *)(vram + (size_t)y * VGA_GFX_WIDTH), 0, VGA_GFX_WIDTH);
            }
        }
    }
    process_yield();
}

void vga_gfx_flip(const uint8_t *buf) {
    vga_gfx_present(buf, false, VGA_GFX_WIDTH, VGA_GFX_HEIGHT);
}

void vga_gfx_flip_desktop(const uint8_t *buf) {
    vga_gfx_present(buf, true, VGA_GFX_WIDTH, VGA_GFX_HEIGHT);
}

void vga_gfx_flip_scaled_buffer(const uint8_t *buf,
                                int source_width,
                                int source_height) {
    vga_gfx_present(buf, true, source_width, source_height);
}
