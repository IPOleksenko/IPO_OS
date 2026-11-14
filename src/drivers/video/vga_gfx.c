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
#include <wm.h>

#define VGA_VRAM_ADDR 0xA0000

static bool vga_proc_uses_wm_bridge(process_t *cur);

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

static bool vga_dyn_reserve(void **data,
                            size_t *cap,
                            size_t count,
                            size_t elem_size,
                            size_t min_cap,
                            size_t init_cap,
                            size_t pad) {
    if (*cap >= min_cap) {
        return true;
    }
    size_t new_cap = *cap ? *cap : init_cap;
    while (new_cap < min_cap) {
        if (new_cap > (SIZE_MAX - pad) / 2u) {
            new_cap = min_cap;
            break;
        }
        new_cap = (new_cap * 3u) / 2u + pad;
    }
    if (new_cap > SIZE_MAX / elem_size) {
        return false;
    }
    uint8_t *new_data = (uint8_t *)kmalloc(new_cap * elem_size);
    if (!new_data) {
        return false;
    }
    if (*data && count > 0) {
        memcpy(new_data, *data, count * elem_size);
    }
    if (new_cap > count) {
        memset(new_data + count * elem_size, 0, (new_cap - count) * elem_size);
    }
    if (*data) {
        kfree(*data);
    }
    *data = new_data;
    *cap = new_cap;
    return true;
}

static void vga_dynbuf8_set(vga_dynbuf8_t *buf, size_t index, uint8_t val) {
    if (index >= buf->capacity &&
        !vga_dyn_reserve((void **)&buf->data, &buf->capacity, buf->count,
                         sizeof(uint8_t), index + 1u, 16u, 8u)) {
        return;
    }
    buf->data[index] = val;
    if (index + 1u > buf->count) {
        buf->count = index + 1u;
    }
}

static uint8_t vga_dynbuf8_get(const vga_dynbuf8_t *buf, size_t index, uint8_t def_val) {
    if (!buf->data || index >= buf->count) {
        return def_val;
    }
    return buf->data[index];
}

static void vga_dynbuf16_set(vga_dynbuf16_t *buf, size_t index, uint16_t val) {
    if (index >= buf->capacity &&
        !vga_dyn_reserve((void **)&buf->data, &buf->capacity, buf->count,
                         sizeof(uint16_t), index + 1u, 64u, 16u)) {
        return;
    }
    buf->data[index] = val;
    if (index + 1u > buf->count) {
        buf->count = index + 1u;
    }
}

static uint16_t vga_dynbuf16_get(const vga_dynbuf16_t *buf, size_t index, uint16_t def_val) {
    if (!buf->data || index >= buf->count) {
        return def_val;
    }
    return buf->data[index];
}

static void vga_dyndac_set(vga_dyndac_t *buf, size_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (index >= buf->capacity &&
        !vga_dyn_reserve((void **)&buf->data, &buf->capacity, buf->count,
                         sizeof(uint8_t[3]), index + 1u, 32u, 16u)) {
        return;
    }
    buf->data[index][0] = r;
    buf->data[index][1] = g;
    buf->data[index][2] = b;
    if (index + 1u > buf->count) {
        buf->count = index + 1u;
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

    /* Save text screen buffer from hardware-probed VGA text aperture */
    vga_probe_text_hardware();
    volatile uint16_t *text_vram = VGA_MEMORY;
    int text_cells = (int)VGA_WIDTH * (int)VGA_HEIGHT;
    for (int i = 0; i < text_cells; i++) {
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
    uint32_t font_flags = vga_plane2_font_enter();
    volatile uint8_t *font_vram = (volatile uint8_t *)0xA0000;
    for (int i = 0; i < 8192; i++) {
        vga_dynbuf8_set(&saved_font_plane2, i, font_vram[i]);
    }
    vga_plane2_font_exit(font_flags);

    text_state_saved = true;
    serial_printf("[vga] Text mode hardware state saved (cursor_pos=%u, visible=%d).\n", saved_cursor_pos, (int)saved_cursor_visible);
}

void vga_set_mode_13h_hardware(void) {
    serial_printf("[vga] 13h hardware mode set (is_gfx=%d)\n", (int)is_gfx);
    /* Always capture text-mode DAC/font/VRAM before leaving text mode, also
     * on the BGA path; otherwise returning to text mode wipes the palette
     * and the Plane 2 font. */
    vga_save_text_state();
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
    static const uint8_t *text_seq = (const uint8_t *)"\x03\x00\x03\x00\x02";
    for (uint8_t i = 0; i < 5; i++) {
        seq_write(i, text_seq[i]);
    }

    /* Unlock CRTC */
    outb(0x3D4, 0x11);
    outb(0x3D5, inb(0x3D5) & ~0x80);

    uint16_t saved_cur_pos = vga_get_cursor_position();

    /* CRTC (Standard 80x25 text mode) */
    static const uint8_t *text_crtc = (const uint8_t *)
        "\x5F\x4F\x50\x82\x55\x81\xBF\x1F"
        "\x00\x4F\x0D\x0E\x00\x00\x00\x00"
        "\x9C\x8E\x8F\x28\x1F\x96\xB9\xA3\xFF";
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
    static const uint8_t *text_gc = (const uint8_t *)"\x00\x00\x00\x00\x00\x10\x0E\x00\xFF";
    for (uint8_t i = 0; i < 9; i++) {
        gc_write(i, text_gc[i]);
    }

    /* AC (Standard text mode) */
    static const uint8_t *text_ac = (const uint8_t *)
        "\x00\x01\x02\x03\x04\x05\x14\x07"
        "\x38\x39\x3A\x3B\x3C\x3D\x3E\x3F"
        "\x0C\x00\x0F\x08\x00";
    for (uint8_t i = 0; i < 21; i++) {
        ac_write(i, text_ac[i]);
    }
    inb(0x3DA);
    outb(0x3C0, 0x20);

    /* DAC (all 256 colors) */
    size_t dac_count = saved_dac.count;
    for (size_t i = 0; i < dac_count; i++) {
        uint8_t r = 0, g = 0, b = 0;
        vga_dyndac_get(&saved_dac, i, &r, &g, &b);
        outb(0x3C8, (uint8_t)i);
        outb(0x3C9, r);
        outb(0x3C9, g);
        outb(0x3C9, b);
    }

    /* Restore Plane 2 Font */
    uint32_t font_flags = vga_plane2_font_enter();
    volatile uint8_t *dest = (volatile uint8_t *)0xA0000;
    size_t font_count = saved_font_plane2.count;
    for (size_t i = 0; i < font_count; i++) {
        dest[i] = vga_dynbuf8_get(&saved_font_plane2, i, 0);
    }
    vga_plane2_font_exit(font_flags);

    /* Re-apply active custom glyphs directly from source bitmaps to eliminate any mode 13h residues */
    vga_font_reapply_active();
    dynamic_keymap_reapply_fonts();

    /* Drain keyboard controller buffer */
    for (int d = 0; d < 16 && (inb(0x64) & 0x01); d++) {
        (void)inb(0x60);
    }

    vga_probe_text_hardware();

    uint16_t active_cursor = has_saved_text_vram ? saved_cursor_pos : vga_get_cursor_position();
    bool active_visible = has_saved_text_vram ? saved_cursor_visible : vga_is_cursor_visible();

    if (has_saved_text_vram) {
        volatile uint16_t *text_vram = VGA_MEMORY;
        size_t text_count = saved_text_vram.count ? saved_text_vram.count : ((size_t)VGA_WIDTH * (size_t)VGA_HEIGHT);
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
    } else if (vbe_bga_is_available() &&
               requested_gfx_width > 0 && requested_gfx_height > 0) {
        w = requested_gfx_width;
        h = requested_gfx_height;
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
    if (width <= 0 || height <= 0) return false;

    /* Background processes and processes rendered inside a WM window must not
     * change the global display resolution used by the visible workspace. */
    process_t *cur = process_in_process_context() ? process_get_current() : NULL;
    if (process_in_process_context() && !process_is_foreground()) return true;
    if (vga_proc_uses_wm_bridge(cur)) return true;

    bool output_was_active = vga_is_graphics_mode() || vbe_bga_is_enabled();
    requested_gfx_width = width;
    requested_gfx_height = height;

    if (output_was_active && process_is_foreground()) {
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
    process_t *cur = process_in_process_context() ? process_get_current() : NULL;
    if (cur) cur->wants_graphics = true;
    if (vga_proc_uses_wm_bridge(cur)) {
        wm_ensure_raw_gfx_window(cur);
        return;
    }
    if (process_is_foreground()) {
        vga_set_mode_13h_hardware();
    }
}

void vga_set_mode_text(void) {
    process_t *cur = process_in_process_context() ? process_get_current() : NULL;
    if (cur) cur->wants_graphics = false;
    if (cur && wm_has_raw_gfx_window_for_pid(cur->pid)) {
        wm_destroy_raw_gfx_window_for_pid(cur->pid);
        return;
    }
    if (vga_proc_uses_wm_bridge(cur)) return;
    if (process_is_foreground() && vga_is_graphics_mode()) {
        vga_set_mode_text_hardware();
    }
}

static void vga_dac_write_entry(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    outb(0x3C8, idx);
    outb(0x3C9, r & 0x3F);
    outb(0x3C9, g & 0x3F);
    outb(0x3C9, b & 0x3F);
}

void vga_gfx_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    process_t *cur = process_in_process_context() ? process_get_current() : NULL;
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
            process_sync_workspace_text_state(cur);
            if (wm_has_raw_gfx_window_for_pid(cur->pid)) {
                wm_raw_gfx_set_palette(cur, idx, r, g, b);
            }
        }
    }
    if (!process_is_foreground()) return;
    /* WM owns the hardware DAC while a session is active; windowed raw apps
     * get their palette remapped by the compositor instead. */
    if (wm_session_active() && cur && (cur->is_wm_app || vga_proc_uses_wm_bridge(cur))) return;
    vga_dac_write_entry(idx, r, g, b);
}

void vga_gfx_init_default_palette(void) {
    /* 16 standard colors */
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

    /* 16..31: 16-level grayscale ramp */
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

/* Buffer and direct VRAM drawing primitives */

void vga_gfx_buf_put_pixel(uint8_t *buf, int x, int y, uint8_t color) {
    if (!buf || x < 0 || x >= VGA_GFX_WIDTH || y < 0 || y >= VGA_GFX_HEIGHT) return;
    ((volatile uint8_t *)buf)[(size_t)y * VGA_GFX_WIDTH + (size_t)x] = color;
}

void vga_gfx_buf_clear(uint8_t *buf, uint8_t color) {
    if (!buf) return;
    memset(buf, color, VGA_GFX_SIZE);
}

void vga_gfx_buf_fill_rect(uint8_t *buf, int x, int y, int w, int h, uint8_t color) {
    if (!buf || w <= 0 || h <= 0) return;
    int64_t x2_64 = (int64_t)x + (int64_t)w;
    int64_t y2_64 = (int64_t)y + (int64_t)h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    int x2 = (x2_64 > VGA_GFX_WIDTH) ? VGA_GFX_WIDTH : (int)x2_64;
    int y2 = (y2_64 > VGA_GFX_HEIGHT) ? VGA_GFX_HEIGHT : (int)y2_64;
    if (x2 <= x || y2 <= y) return;

    for (int cy = y; cy < y2; cy++) {
        memset(buf + ((size_t)cy * VGA_GFX_WIDTH + (size_t)x), color, (size_t)(x2 - x));
    }
}

void vga_gfx_buf_draw_line(uint8_t *buf, int x0, int y0, int x1, int y1, uint8_t color) {
    if (!buf) return;
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

void vga_gfx_buf_draw_rect(uint8_t *buf, int x, int y, int w, int h, uint8_t color) {
    if (!buf || w <= 0 || h <= 0) return;
    int x_end = (int)((int64_t)x + (int64_t)w - 1);
    int y_end = (int)((int64_t)y + (int64_t)h - 1);
    vga_gfx_buf_draw_line(buf, x, y, x_end, y, color);
    vga_gfx_buf_draw_line(buf, x, y_end, x_end, y_end, color);
    vga_gfx_buf_draw_line(buf, x, y, x, y_end, color);
    vga_gfx_buf_draw_line(buf, x_end, y, x_end, y_end, color);
}

void vga_gfx_buf_draw_circle(uint8_t *buf, int xc, int yc, int r, uint8_t color) {
    if (!buf || r < 0) return;
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
    if (!buf || r < 0) return;
    int64_t r2 = (int64_t)r * (int64_t)r;
    for (int dy = -r; dy <= r; dy++) {
        int64_t dy2 = (int64_t)dy * (int64_t)dy;
        for (int dx = -r; dx <= r; dx++) {
            if ((int64_t)dx * (int64_t)dx + dy2 <= r2) {
                vga_gfx_buf_put_pixel(buf, xc + dx, yc + dy, color);
            }
        }
    }
}

void vga_gfx_put_pixel(int x, int y, uint8_t color) {
    vga_gfx_buf_put_pixel((uint8_t *)(uintptr_t)VGA_VRAM_ADDR, x, y, color);
}

uint8_t vga_gfx_get_pixel(int x, int y) {
    if (x < 0 || x >= VGA_GFX_WIDTH || y < 0 || y >= VGA_GFX_HEIGHT) return 0;
    volatile uint8_t *vram = (volatile uint8_t *)(uintptr_t)VGA_VRAM_ADDR;
    return vram[(size_t)y * VGA_GFX_WIDTH + (size_t)x];
}

void vga_gfx_clear(uint8_t color) {
    vga_gfx_buf_clear((uint8_t *)(uintptr_t)VGA_VRAM_ADDR, color);
}

void vga_gfx_draw_line(int x0, int y0, int x1, int y1, uint8_t color) {
    vga_gfx_buf_draw_line((uint8_t *)(uintptr_t)VGA_VRAM_ADDR, x0, y0, x1, y1, color);
}

void vga_gfx_draw_rect(int x, int y, int w, int h, uint8_t color) {
    vga_gfx_buf_draw_rect((uint8_t *)(uintptr_t)VGA_VRAM_ADDR, x, y, w, h, color);
}

void vga_gfx_fill_rect(int x, int y, int w, int h, uint8_t color) {
    vga_gfx_buf_fill_rect((uint8_t *)(uintptr_t)VGA_VRAM_ADDR, x, y, w, h, color);
}

void vga_gfx_draw_circle(int xc, int yc, int r, uint8_t color) {
    vga_gfx_buf_draw_circle((uint8_t *)(uintptr_t)VGA_VRAM_ADDR, xc, yc, r, color);
}

void vga_gfx_fill_circle(int xc, int yc, int r, uint8_t color) {
    vga_gfx_buf_fill_circle((uint8_t *)(uintptr_t)VGA_VRAM_ADDR, xc, yc, r, color);
}

static int *s_scale_lut_x = NULL;
static int s_scale_lut_cap = 0;
static int s_scale_lut_dst_w = 0;
static int s_scale_lut_src_w = 0;
static uint8_t *s_scale_row_buf = NULL;
static int s_scale_row_cap = 0;
static volatile int s_scale_cache_lock = 0;

static void vga_gfx_blit_8bpp_target(volatile uint8_t *dst_fb,
                                     int dst_w,
                                     int dst_h,
                                     const uint8_t *buf,
                                     int source_width,
                                     int source_height,
                                     bool scale_to_display) {
    if (!dst_fb || dst_w <= 0 || dst_h <= 0) return;

    if (source_width == dst_w && source_height == dst_h) {
        memcpy((void *)dst_fb, buf, (size_t)dst_w * (size_t)dst_h);
        return;
    }

    if (scale_to_display && source_width > 0 && source_height > 0) {
        bool have_cache = false;
        if (__sync_lock_test_and_set(&s_scale_cache_lock, 1) == 0) {
            if (dst_w > s_scale_lut_cap) {
                int *new_lut = (int *)kmalloc((size_t)dst_w * sizeof(int));
                if (new_lut) {
                    if (s_scale_lut_x) kfree(s_scale_lut_x);
                    s_scale_lut_x = new_lut;
                    s_scale_lut_cap = dst_w;
                    s_scale_lut_dst_w = 0;
                }
            }
            if (dst_w > s_scale_row_cap) {
                uint8_t *new_row = (uint8_t *)kmalloc((size_t)dst_w);
                if (new_row) {
                    if (s_scale_row_buf) kfree(s_scale_row_buf);
                    s_scale_row_buf = new_row;
                    s_scale_row_cap = dst_w;
                }
            }
            if (s_scale_lut_x && s_scale_lut_cap >= dst_w &&
                s_scale_row_buf && s_scale_row_cap >= dst_w) {
                if (s_scale_lut_dst_w != dst_w || s_scale_lut_src_w != source_width) {
                    for (int x = 0; x < dst_w; x++) {
                        s_scale_lut_x[x] = (int)(((uint64_t)x * (uint32_t)source_width) / (uint32_t)dst_w);
                    }
                    s_scale_lut_dst_w = dst_w;
                    s_scale_lut_src_w = source_width;
                }
                int prev_source_y = -1;
                for (int y = 0; y < dst_h; y++) {
                    int source_y = (int)(((uint64_t)y * (uint32_t)source_height) / (uint32_t)dst_h);
                    if (source_y != prev_source_y) {
                        const uint8_t *src = buf + (size_t)source_y * (size_t)source_width;
                        int x = 0;
                        for (; x + 4 <= dst_w; x += 4) {
                            uint32_t p = (uint32_t)src[s_scale_lut_x[x]] |
                                         ((uint32_t)src[s_scale_lut_x[x + 1]] << 8) |
                                         ((uint32_t)src[s_scale_lut_x[x + 2]] << 16) |
                                         ((uint32_t)src[s_scale_lut_x[x + 3]] << 24);
                            *(uint32_t *)(s_scale_row_buf + x) = p;
                        }
                        for (; x < dst_w; x++) {
                            s_scale_row_buf[x] = src[s_scale_lut_x[x]];
                        }
                        prev_source_y = source_y;
                    }
                    memcpy((void *)(dst_fb + (size_t)y * (size_t)dst_w),
                           s_scale_row_buf, (size_t)dst_w);
                }
                have_cache = true;
            }
            __sync_lock_release(&s_scale_cache_lock);
        }
        if (!have_cache) {
            for (int y = 0; y < dst_h; y++) {
                int source_y = (int)(((uint64_t)y * (uint32_t)source_height) / (uint32_t)dst_h);
                volatile uint8_t *dst = dst_fb + (size_t)y * (size_t)dst_w;
                const uint8_t *src = buf + (size_t)source_y * (size_t)source_width;
                for (int x = 0; x < dst_w; x++) {
                    int sx = (int)(((uint64_t)x * (uint32_t)source_width) / (uint32_t)dst_w);
                    dst[x] = src[sx];
                }
            }
        }
        return;
    }

    int copy_width = dst_w < source_width ? dst_w : source_width;
    int copy_height = dst_h < source_height ? dst_h : source_height;
    int source_x = (source_width - copy_width) / 2;
    int source_y = (source_height - copy_height) / 2;
    int target_x = (dst_w - copy_width) / 2;
    int target_y = (dst_h - copy_height) / 2;

    for (int y = 0; y < target_y; y++) {
        memset((void *)(dst_fb + (size_t)y * (size_t)dst_w), 0, (size_t)dst_w);
    }
    for (int y = 0; y < copy_height; y++) {
        volatile uint8_t *dst = dst_fb + (size_t)(target_y + y) * (size_t)dst_w;
        if (target_x > 0) {
            memset((void *)dst, 0, (size_t)target_x);
        }
        memcpy((void *)(dst + target_x),
               buf + (size_t)(source_y + y) * (size_t)source_width + source_x,
               (size_t)copy_width);
        if (target_x + copy_width < dst_w) {
            memset((void *)(dst + target_x + copy_width), 0,
                   (size_t)(dst_w - target_x - copy_width));
        }
    }
    for (int y = target_y + copy_height; y < dst_h; y++) {
        memset((void *)(dst_fb + (size_t)y * (size_t)dst_w), 0, (size_t)dst_w);
    }
}

void vga_gfx_blit_hardware(const uint8_t *buf,
                           bool scale_to_display,
                           int source_width,
                           int source_height) {
    if (!buf) return;
    if (source_width <= 0 || source_height <= 0 ||
        (size_t)source_width > SIZE_MAX / (size_t)source_height) {
        return;
    }
    if (vbe_bga_is_enabled()) {
        if (vbe_bga_get_bpp() != 8) {
            return;
        }
        vga_gfx_blit_8bpp_target((volatile uint8_t *)vbe_bga_get_framebuffer(),
                                 vbe_bga_get_width(), vbe_bga_get_height(),
                                 buf, source_width, source_height, scale_to_display);
        return;
    }
    if (!vga_is_graphics_mode()) return;

    vga_gfx_blit_8bpp_target((volatile uint8_t *)(uintptr_t)VGA_VRAM_ADDR,
                             VGA_GFX_WIDTH, VGA_GFX_HEIGHT,
                             buf, source_width, source_height, scale_to_display);
}

/* True when @cur is a raw (non-WM) graphics process that shares its workspace
 * with other processes (`run a && b`). Such processes must never touch the
 * physical screen directly: the OS puts each of them into its own WM window. */
static bool vga_proc_uses_wm_bridge(process_t *cur) {
    if (!cur || !process_in_process_context()) return false;
    if (!process_is_batch_active()) return false;
    if (cur->is_wm_app) return false;
    if (wm_has_raw_gfx_window_for_pid(cur->pid)) return true;
    if (wm_session_active() && !process_get_separate_windows()) return true;
    return process_workspace_get_proc_count(cur->window_id) > 1;
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

    process_t *cur = process_in_process_context() ? process_get_current() : NULL;

    /* Shared workspace (&&): route the frame into this process's own WM window
     * so concurrent graphics apps never overwrite each other. */
    if (vga_proc_uses_wm_bridge(cur)) {
        wm_present_raw_gfx_process(cur, buf, source_width, source_height);
        process_yield();
        return;
    }

    if (cur && process_get_separate_windows() && !cur->is_wm_app) {
        if (!cur->gfx_vram_backup) {
            cur->gfx_vram_backup = (uint8_t *)kmalloc(VGA_GFX_SIZE);
        }
        if (cur->gfx_vram_backup) {
            if (source_width == VGA_GFX_WIDTH && source_height == VGA_GFX_HEIGHT) {
                memcpy(cur->gfx_vram_backup, buf, VGA_GFX_SIZE);
            } else {
                for (int y = 0; y < VGA_GFX_HEIGHT; y++) {
                    int sy = (int)(((uint32_t)y * (uint32_t)source_height) / VGA_GFX_HEIGHT);
                    const uint8_t *src = buf + (size_t)sy * (size_t)source_width;
                    uint8_t *dst = cur->gfx_vram_backup + (size_t)y * VGA_GFX_WIDTH;
                    for (int x = 0; x < VGA_GFX_WIDTH; x++) {
                        dst[x] = src[((uint32_t)x * (uint32_t)source_width) / VGA_GFX_WIDTH];
                    }
                }
            }
            process_sync_workspace_text_state(cur);
        }
    }
    if (process_in_process_context() && !process_is_foreground()) {
        process_yield();
        return;
    }
    vga_gfx_blit_hardware(buf, scale_to_display, source_width, source_height);
    process_yield();
}

void vga_gfx_flip(const uint8_t *buf) {
    vga_gfx_present(buf, true, VGA_GFX_WIDTH, VGA_GFX_HEIGHT);
}

void vga_gfx_flip_desktop(const uint8_t *buf) {
    vga_gfx_present(buf, true, VGA_GFX_WIDTH, VGA_GFX_HEIGHT);
}

void vga_gfx_flip_scaled_buffer(const uint8_t *buf,
                                int source_width,
                                int source_height) {
    vga_gfx_present(buf, true, source_width, source_height);
}
