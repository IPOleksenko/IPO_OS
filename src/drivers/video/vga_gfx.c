/* VGA Mode 13h and DISPI linear framebuffer graphics driver. */

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
#include <kernel/terminal.h>
#include <wm.h>

#define VGA_VRAM_ADDR 0xA0000

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

bool vga_gfx_has_saved_text(void) {
    return has_saved_text_vram && saved_text_vram.data != NULL && saved_text_vram.count > 0;
}

bool vga_gfx_get_saved_text_buffer(uint16_t **out_vram, size_t *out_cells, uint16_t **out_cursor_pos) {
    if (!vga_gfx_has_saved_text()) {
        return false;
    }
    if (out_vram) *out_vram = saved_text_vram.data;
    if (out_cells) *out_cells = saved_text_vram.count;
    if (out_cursor_pos) *out_cursor_pos = &saved_cursor_pos;
    return true;
}

void vga_gfx_scroll_saved_text(void) {
    if (!vga_gfx_has_saved_text()) {
        return;
    }
    uint16_t w = VGA_WIDTH;
    uint16_t h = VGA_HEIGHT;
    if (w == 0 || h <= 1) {
        return;
    }
    size_t needed = (size_t)w * (size_t)h;
    if (saved_text_vram.count < needed) {
        return;
    }
    uint16_t top = (uint16_t)(VGA_START_CURSOR_POSITION / w);
    if (top + 1u >= h) {
        return;
    }
    uint16_t rows = (uint16_t)(h - top);
    for (uint16_t r = 0; r + 1u < rows; r++) {
        uint16_t src = (uint16_t)((top + r + 1u) * w);
        uint16_t dst = (uint16_t)((top + r) * w);
        for (uint16_t c = 0; c < w; c++) {
            uint16_t val = saved_text_vram.data[src + c];
            if ((val & 0xFFu) == VGA_CURSOR_GLYPH_SLOT) {
                val = (uint16_t)((val & 0xFF00u) | ' ');
            }
            saved_text_vram.data[dst + c] = val;
        }
    }
    uint16_t blank = vga_entry(' ', VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    uint16_t bottom = (uint16_t)((top + rows - 1u) * w);
    for (uint16_t c = 0; c < w; c++) {
        saved_text_vram.data[bottom + c] = blank;
    }
}
static int  s_init_screen_width = 0;
static int  s_init_screen_height = 0;
static int  requested_gfx_width = 0;
static int  requested_gfx_height = 0;
static bool s_resolution_configured = false;
static bool s_gfx_initialized = false;
static int  s_vga_gfx_hw_width = 0;
static int  s_vga_gfx_hw_height = 0;
static bool s_vga_gfx_hw_probed = false;

static bool vga_probe_gfx_hardware(void) {
    if (vbe_bga_is_enabled()) {
        return false;
    }

    outb(0x3CE, 0x06);
    uint8_t gc6 = inb(0x3CF);
    if ((gc6 & 0x01u) == 0u) {
        return false;
    }

    uint16_t crtc_idx = (inb(0x3CC) & 0x01u) ? 0x3D4u : 0x3B4u;
    uint16_t crtc_dat = (uint16_t)(crtc_idx + 1u);

    outb(crtc_idx, 0x01u);
    uint32_t cols = (uint32_t)inb(crtc_dat) + 1u;

    outb(0x3C4u, 0x01u);
    uint8_t seq1 = inb(0x3C5u);
    uint32_t dot_w = (seq1 & 0x01u) ? 8u : 9u;

    inb(0x3DAu);
    outb(0x3C0u, 0x10u | 0x20u);
    uint8_t ac10 = inb(0x3C1u);
    inb(0x3DAu);
    outb(0x3C0u, 0x20u);

    uint32_t px_w = cols * dot_w;
    if (seq1 & 0x08u) px_w /= 2u;
    if (ac10 & 0x40u) px_w /= 2u;

    outb(crtc_idx, 0x12u);
    uint8_t vde_lo = inb(crtc_dat);
    outb(crtc_idx, 0x07u);
    uint8_t ovf = inb(crtc_dat);
    uint32_t total_scanlines = (uint32_t)vde_lo
                             | (((uint32_t)ovf & 0x02u) << 7)
                             | (((uint32_t)ovf & 0x40u) << 3);
    total_scanlines += 1u;

    outb(crtc_idx, 0x09u);
    uint8_t msl = inb(crtc_dat);
    uint32_t px_h = total_scanlines;
    if (msl & 0x80u) px_h /= 2u;
    uint32_t repeat = (uint32_t)(msl & 0x1Fu) + 1u;
    if (repeat > 1u) px_h /= repeat;

    if (px_w > 0u && px_h > 0u) {
        s_vga_gfx_hw_width = (int)px_w;
        s_vga_gfx_hw_height = (int)px_h;
        s_vga_gfx_hw_probed = true;
        return true;
    }
    return false;
}

void vga_gfx_init(void) {
    if (s_gfx_initialized) return;
    vga_probe_text_hardware();

    if (vbe_bga_is_available() && vbe_bga_is_enabled()) {
        s_init_screen_width = vbe_bga_get_width();
        s_init_screen_height = vbe_bga_get_height();
    } else if (vga_probe_gfx_hardware()) {
        s_init_screen_width = s_vga_gfx_hw_width;
        s_init_screen_height = s_vga_gfx_hw_height;
    } else {
        s_init_screen_width = (int)vga_get_pixel_width();
        s_init_screen_height = (int)vga_get_pixel_height();
    }

    if (!s_resolution_configured &&
        (requested_gfx_width <= 0 || requested_gfx_height <= 0)) {
        requested_gfx_width = s_init_screen_width;
        requested_gfx_height = s_init_screen_height;
    }
    s_gfx_initialized = true;
}

int vga_gfx_get_init_width(void) {
    if (!s_gfx_initialized) vga_gfx_init();
    return s_init_screen_width;
}

int vga_gfx_get_init_height(void) {
    if (!s_gfx_initialized) vga_gfx_init();
    return s_init_screen_height;
}

int vga_gfx_get_mode13h_width(void) {
    if (!s_vga_gfx_hw_probed) vga_probe_gfx_hardware();
    return s_vga_gfx_hw_width;
}

int vga_gfx_get_mode13h_height(void) {
    if (!s_vga_gfx_hw_probed) vga_probe_gfx_hardware();
    return s_vga_gfx_hw_height;
}

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
    /* Do not read 0xB8000 in graphics mode or after saving; Mode 13h unmaps it
     * so reads return floating bus data. */
    if (vga_is_graphics_mode() || has_saved_text_vram) {
        return;
    }

    saved_cursor_pos = vga_get_cursor_position();
    saved_cursor_visible = vga_is_cursor_visible();

    /* Remove software cursor glyph before snapshotting text VRAM */
    vga_hide_cursor();

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

    saved_misc = inb(0x3CC);

    for (uint8_t i = 0; i < 5; i++) {
        outb(0x3C4, i);
        vga_dynbuf8_set(&saved_seq, i, inb(0x3C5));
    }

    for (uint8_t i = 0; i < 25; i++) {
        outb(0x3D4, i);
        vga_dynbuf8_set(&saved_crtc, i, inb(0x3D5));
    }

    for (uint8_t i = 0; i < 9; i++) {
        outb(0x3CE, i);
        vga_dynbuf8_set(&saved_gc, i, inb(0x3CF));
    }

    for (uint8_t i = 0; i < 21; i++) {
        inb(0x3DA);
        outb(0x3C0, i);
        vga_dynbuf8_set(&saved_ac, i, inb(0x3C1));
    }
    inb(0x3DA);
    outb(0x3C0, 0x20);

    for (int i = 0; i < 256; i++) {
        outb(0x3C7, (uint8_t)i);
        uint8_t r = inb(0x3C9);
        uint8_t g = inb(0x3C9);
        uint8_t b = inb(0x3C9);
        vga_dyndac_set(&saved_dac, i, r, g, b);
    }

    /* Save Plane 2 font (8 KiB: 256 glyphs * 32 B) */
    uint32_t font_flags = vga_plane2_font_enter();
    volatile uint8_t *font_vram = (volatile uint8_t *)0xA0000;
    for (int i = 0; i < 8192; i++) {
        vga_dynbuf8_set(&saved_font_plane2, i, font_vram[i]);
    }
    vga_plane2_font_exit(font_flags);

    text_state_saved = true;
    serial_printf("[vga] Text mode hardware state saved (cursor_pos=%u, visible=%d).\n", saved_cursor_pos, (int)saved_cursor_visible);
}

static bool vga_bga_matches_request(int req_w, int req_h) {
    if (!vbe_bga_is_enabled() || vbe_bga_get_bpp() != 8 || vbe_bga_get_height() != req_h) {
        return false;
    }
    int actual_w = vbe_bga_get_width();
    return actual_w == req_w || (req_w >= 8 && actual_w == (req_w & ~7));
}

void vga_set_mode_13h_hardware(void) {
    serial_printf("[vga] 13h hardware mode set (is_gfx=%d)\n", (int)is_gfx);
    /* Snapshot text DAC, Plane 2 font, and VRAM before any mode switch;
     * DISPI mode switches otherwise wipe palette and font RAM. */
    vga_save_text_state();
    bool bga_attempted = false;
    if (vbe_bga_is_available()) {
        if (vga_bga_matches_request(requested_gfx_width, requested_gfx_height)) {
            is_gfx = true;
            return;
        }
        bga_attempted = true;
        if (vbe_bga_set_mode(requested_gfx_width, requested_gfx_height, 8)) {
            is_gfx = true;
            vga_gfx_init_default_palette();
            mouse_set_bounds_from_display();
            return;
        }
        if (vbe_bga_is_enabled()) {
            is_gfx = true;
            vga_gfx_init_default_palette();
            mouse_set_bounds_from_display();
            serial_printf("[vga] keeping device-applied mode %dx%d\n",
                          vbe_bga_get_width(), vbe_bga_get_height());
            return;
        }
        serial_printf("[vga] requested BGA mode %dx%d unavailable; using Mode 13h\n",
                      requested_gfx_width, requested_gfx_height);
        vga_save_text_state();
    }

    if (vbe_bga_is_enabled()) {
        vbe_bga_disable();
    }
    /* Do not skip Mode 13h programming after a failed DISPI attempt,
     * which leaves GC[6] and CRTC registers dirty. */
    if (!bga_attempted && is_gfx && vga_is_graphics_mode()) {
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
    gc_write(0x06, 0x05); /* 0xA0000 64 KiB */
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

    /* Unblank video */
    inb(0x3DA);
    outb(0x3C0, 0x20);

    vga_probe_gfx_hardware();
    vga_gfx_init_default_palette();
    int hw_w = vga_gfx_get_mode13h_width();
    int hw_h = vga_gfx_get_mode13h_height();
    if (hw_w > 0 && hw_h > 0 && (size_t)hw_w <= SIZE_MAX / (size_t)hw_h) {
        memset((void *)(uintptr_t)VGA_VRAM_ADDR, 0, (size_t)hw_w * (size_t)hw_h);
    }
    is_gfx = true;
    mouse_set_bounds_from_display();
}

void vga_set_mode_text_hardware(void) {
    bool was_gfx = is_gfx || vbe_bga_is_enabled();
    serial_printf("[vga] text hardware mode set (is_gfx=%d)\n", (int)is_gfx);
    vbe_bga_disable();

    /* Misc Output */
    outb(0x3C2, 0x67);

    /* Sequencer (Mode 03h) */
    static const uint8_t *text_seq = (const uint8_t *)"\x03\x00\x03\x00\x02";
    for (uint8_t i = 0; i < 5; i++) {
        seq_write(i, text_seq[i]);
    }

    /* Unlock CRTC */
    outb(0x3D4, 0x11);
    outb(0x3D5, inb(0x3D5) & ~0x80);

    uint16_t saved_cur_pos = vga_get_cursor_position();

    /* CRTC (80x25 text mode) */
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

    /* Graphics Controller */
    static const uint8_t *text_gc = (const uint8_t *)"\x00\x00\x00\x00\x00\x10\x0E\x00\xFF";
    for (uint8_t i = 0; i < 9; i++) {
        gc_write(i, text_gc[i]);
    }

    /* Attribute Controller */
    static const uint8_t *text_ac = (const uint8_t *)
        "\x00\x01\x02\x03\x04\x05\x14\x07"
        "\x38\x39\x3A\x3B\x3C\x3D\x3E\x3F"
        "\x0C\x00\x0F\x08\x00";
    for (uint8_t i = 0; i < 21; i++) {
        ac_write(i, text_ac[i]);
    }
    inb(0x3DA);
    outb(0x3C0, 0x20);

    /* DAC palette */
    size_t dac_count = saved_dac.count;
    for (size_t i = 0; i < dac_count; i++) {
        uint8_t r = 0, g = 0, b = 0;
        vga_dyndac_get(&saved_dac, i, &r, &g, &b);
        outb(0x3C8, (uint8_t)i);
        outb(0x3C9, r);
        outb(0x3C9, g);
        outb(0x3C9, b);
    }

    /* Plane 2 font */
    uint32_t font_flags = vga_plane2_font_enter();
    volatile uint8_t *dest = (volatile uint8_t *)0xA0000;
    size_t font_count = saved_font_plane2.count;
    for (size_t i = 0; i < font_count; i++) {
        dest[i] = vga_dynbuf8_get(&saved_font_plane2, i, 0);
    }
    vga_plane2_font_exit(font_flags);

    /* Re-upload custom glyphs over any Mode 13h Plane 2 corruption */
    vga_font_reapply_active();
    dynamic_keymap_reapply_fonts();

    /* Flush stale 8042 bytes */
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
    } else if (was_gfx) {
        volatile uint16_t *text_vram = VGA_MEMORY;
        size_t text_count = (size_t)VGA_WIDTH * (size_t)VGA_HEIGHT;
        for (size_t i = 0; i < text_count; i++) {
            text_vram[i] = 0x0720;
        }
        print_header_row(VGA_MEMORY, VGA_WIDTH);
        if (active_cursor < (uint16_t)(VGA_WIDTH * 2)) {
            active_cursor = (uint16_t)(VGA_WIDTH * 2);
        }
        active_visible = true;
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
    if (!s_gfx_initialized) vga_gfx_init();
    int w = s_init_screen_width;
    int h = s_init_screen_height;
    bool graphics_mode = is_gfx || vga_is_graphics_mode();

    if (vbe_bga_is_available() && vbe_bga_is_enabled()) {
        int bga_w = vbe_bga_get_width();
        int bga_h = vbe_bga_get_height();
        w = bga_w;
        h = bga_h;
    } else if (graphics_mode) {
        w = vga_gfx_get_mode13h_width();
        h = vga_gfx_get_mode13h_height();
    } else if (vbe_bga_is_available() &&
               requested_gfx_width > 0 && requested_gfx_height > 0) {
        w = requested_gfx_width;
        h = requested_gfx_height;
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
    if (width > 65535 || height > 65535) return false;
    if (!s_gfx_initialized) vga_gfx_init();

    /* Userland applications must not override the global display resolution
     * configured by the user via the `resolution` shell command. */
    if (process_in_process_context()) return true;

    bool output_was_active = vga_is_graphics_mode() || vbe_bga_is_enabled();
    requested_gfx_width = width;
    requested_gfx_height = height;
    s_resolution_configured = true;

    if (output_was_active && process_is_foreground()) {
        vga_set_mode_13h_hardware();
        if (wm_session_active()) {
            wm_on_resolution_changed();
        }
        if (vbe_bga_is_enabled())
            return vga_bga_matches_request(width, height);

        return width == vga_gfx_get_mode13h_width() &&
               height == vga_gfx_get_mode13h_height();
    }
    return true;
}

void vga_gfx_get_resolution(int *out_w, int *out_h) {
    if (!s_gfx_initialized) vga_gfx_init();
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
    if (cur && process_is_batch_active() && process_get_separate_windows()) {
        process_t *fg = process_get_foreground();
        if (!fg || (!fg->is_wm_app && !fg->wants_graphics)) {
            process_set_foreground(cur);
        }
    }
    if (process_is_foreground() && !is_gfx) {
        vga_set_mode_13h_hardware();
    }
}

void vga_set_mode_text(void) {
    process_t *cur = process_in_process_context() ? process_get_current() : NULL;
    if (cur) cur->wants_graphics = false;
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
        }
    }
    if (!process_is_foreground()) return;
    if (wm_session_active() && cur && cur->is_wm_app) return;
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

static void vga_mmio_write_row(volatile uint8_t *dst, const uint8_t *src, size_t len) {
    size_t i = 0;
    while (i < len && (((uintptr_t)(dst + i) & 3u) != 0u)) {
        dst[i] = src[i];
        i++;
    }
    size_t dwords = (len - i) >> 2;
    volatile uint32_t *dst32 = (volatile uint32_t *)(dst + i);
    const uint8_t *s = src + i;
    if (((uintptr_t)s & 3u) == 0u) {
        const uint32_t *src32 = (const uint32_t *)s;
        for (size_t d = 0; d < dwords; d++) {
            dst32[d] = src32[d];
        }
    } else {
        for (size_t d = 0; d < dwords; d++) {
            uint32_t v = (uint32_t)s[0] |
                         ((uint32_t)s[1] << 8) |
                         ((uint32_t)s[2] << 16) |
                         ((uint32_t)s[3] << 24);
            dst32[d] = v;
            s += 4;
        }
    }
    i += dwords << 2;
    while (i < len) {
        dst[i] = src[i];
        i++;
    }
}

static void vga_scale_8bpp(volatile uint8_t *dst_buf,
                           int dst_w,
                           int dst_h,
                           const uint8_t *src_buf,
                           int src_w,
                           int src_h,
                           bool dst_is_mmio) {
    if (!dst_buf || !src_buf || dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) {
        return;
    }
    if ((size_t)dst_w > SIZE_MAX / (size_t)dst_h) {
        return;
    }

    if (src_w == dst_w && src_h == dst_h) {
        if (dst_is_mmio) {
            vga_mmio_write_row(dst_buf, src_buf, (size_t)dst_w * (size_t)dst_h);
        } else {
            memcpy((void *)dst_buf, src_buf, (size_t)dst_w * (size_t)dst_h);
        }
        return;
    }

    static volatile int s_scale_lock = 0;
    static uint16_t *s_x_lut = NULL;
    static size_t s_x_lut_cap = 0;
    static int s_x_lut_dst_w = 0;
    static int s_x_lut_src_w = 0;
    static uint8_t *s_row_buf = NULL;
    static size_t s_row_cap = 0;

    if (__sync_lock_test_and_set(&s_scale_lock, 1) == 0) {
        if ((size_t)dst_w > s_x_lut_cap) {
            uint16_t *nlut = (uint16_t *)krealloc(s_x_lut, (size_t)dst_w * sizeof(uint16_t));
            if (nlut) {
                s_x_lut = nlut;
                s_x_lut_cap = (size_t)dst_w;
                s_x_lut_dst_w = 0;
            }
        }
        if ((size_t)dst_w > s_row_cap) {
            size_t alloc_w = ((size_t)dst_w + 15u) & ~(size_t)15u;
            uint8_t *nrow = (uint8_t *)krealloc(s_row_buf, alloc_w);
            if (nrow) {
                s_row_buf = nrow;
                s_row_cap = alloc_w;
            }
        }

        if (s_x_lut && s_x_lut_cap >= (size_t)dst_w && s_row_buf && s_row_cap >= (size_t)dst_w) {
            if (s_x_lut_dst_w != dst_w || s_x_lut_src_w != src_w) {
                for (int x = 0; x < dst_w; x++) {
                    s_x_lut[x] = (uint16_t)(((uint32_t)x * (uint32_t)src_w) / (uint32_t)dst_w);
                }
                s_x_lut_dst_w = dst_w;
                s_x_lut_src_w = src_w;
            }

            const uint16_t *xlut = s_x_lut;
            uint8_t *row_buf = s_row_buf;
            int prev_sy = -1;
            for (int y = 0; y < dst_h; y++) {
                int sy = (int)(((uint32_t)y * (uint32_t)src_h) / (uint32_t)dst_h);
                if (sy != prev_sy) {
                    const uint8_t *src = src_buf + (size_t)sy * (size_t)src_w;
                    int x = 0;
                    for (; x + 4 <= dst_w; x += 4) {
                        row_buf[x + 0] = src[xlut[x + 0]];
                        row_buf[x + 1] = src[xlut[x + 1]];
                        row_buf[x + 2] = src[xlut[x + 2]];
                        row_buf[x + 3] = src[xlut[x + 3]];
                    }
                    for (; x < dst_w; x++) {
                        row_buf[x] = src[xlut[x]];
                    }
                    prev_sy = sy;
                }
                volatile uint8_t *dst = dst_buf + (size_t)y * (size_t)dst_w;
                if (dst_is_mmio) {
                    vga_mmio_write_row(dst, row_buf, (size_t)dst_w);
                } else {
                    memcpy((void *)dst, row_buf, (size_t)dst_w);
                }
            }
            __sync_lock_release(&s_scale_lock);
            return;
        }
        __sync_lock_release(&s_scale_lock);
    }

    for (int y = 0; y < dst_h; y++) {
        int sy = (int)(((uint32_t)y * (uint32_t)src_h) / (uint32_t)dst_h);
        volatile uint8_t *dst = dst_buf + (size_t)y * (size_t)dst_w;
        const uint8_t *src = src_buf + (size_t)sy * (size_t)src_w;
        for (int x = 0; x < dst_w; x++) {
            dst[x] = src[((uint32_t)x * (uint32_t)src_w) / (uint32_t)dst_w];
        }
    }
}

static void vga_gfx_blit_8bpp_target(volatile uint8_t *dst_fb,
                                     int dst_w,
                                     int dst_h,
                                     const uint8_t *buf,
                                     int source_width,
                                     int source_height,
                                     bool scale_to_display) {
    if (!dst_fb || dst_w <= 0 || dst_h <= 0) return;
    if ((size_t)dst_w > SIZE_MAX / (size_t)dst_h) return;

    size_t total = (size_t)dst_w * (size_t)dst_h;
    if (source_width == dst_w && source_height == dst_h) {
        vga_mmio_write_row(dst_fb, buf, total);
        return;
    }

    if (scale_to_display && source_width > 0 && source_height > 0) {
        vga_scale_8bpp(dst_fb, dst_w, dst_h, buf, source_width, source_height, true);
        return;
    }

    int copy_width = dst_w < source_width ? dst_w : source_width;
    int copy_height = dst_h < source_height ? dst_h : source_height;
    int source_x = (source_width - copy_width) / 2;
    int source_y = (source_height - copy_height) / 2;
    int target_x = (dst_w - copy_width) / 2;
    int target_y = (dst_h - copy_height) / 2;

    memset((void *)dst_fb, 0, total);
    for (int y = 0; y < copy_height; y++) {
        vga_mmio_write_row(dst_fb + (size_t)(target_y + y) * (size_t)dst_w + target_x,
                           buf + (size_t)(source_y + y) * (size_t)source_width + source_x,
                           (size_t)copy_width);
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

    int hw_w = vga_gfx_get_mode13h_width();
    int hw_h = vga_gfx_get_mode13h_height();
    if (hw_w <= 0 || hw_h <= 0) return;

    vga_gfx_blit_8bpp_target((volatile uint8_t *)(uintptr_t)VGA_VRAM_ADDR,
                             hw_w, hw_h,
                             buf, source_width, source_height, scale_to_display);
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

    if (cur && process_get_separate_windows() && !cur->is_wm_app) {
        int bw = vbe_bga_is_enabled() ? vbe_bga_get_width() : vga_gfx_get_mode13h_width();
        int bh = vbe_bga_is_enabled() ? vbe_bga_get_height() : vga_gfx_get_mode13h_height();
        if (bw > 0 && bh > 0 && (size_t)bw <= SIZE_MAX / (size_t)bh) {
            size_t bsz = (size_t)bw * (size_t)bh;
            if (!cur->gfx_vram_backup ||
                cur->gfx_vram_backup_w != bw ||
                cur->gfx_vram_backup_h != bh) {
                uint8_t *nb = (uint8_t *)krealloc(cur->gfx_vram_backup, bsz);
                if (nb) {
                    cur->gfx_vram_backup = nb;
                    cur->gfx_vram_backup_w = bw;
                    cur->gfx_vram_backup_h = bh;
                }
            }
            if (cur->gfx_vram_backup &&
                cur->gfx_vram_backup_w == bw &&
                cur->gfx_vram_backup_h == bh) {
                vga_scale_8bpp(cur->gfx_vram_backup, bw, bh, buf, source_width, source_height, false);
                process_sync_workspace_text_state(cur);
            }
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
