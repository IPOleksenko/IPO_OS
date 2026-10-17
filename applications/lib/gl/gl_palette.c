#include <GL/ipo_gl.h>
#include <vga_gfx.h>
#include <ioport.h>

/* Bayer 4x4 Dithering Matrix (values 0..15 scaled to bias RGB components) */
static const int8_t bayer4x4[4][4] = {
    { -8,  0, -6,  2 },
    {  4, -4,  6, -2 },
    { -5,  3, -7,  1 },
    {  7, -1,  5, -3 }
};

static bool g_palette_initialized = false;

void gl_palette_init(void) {
    if (g_palette_initialized) return;

    /* 16..31: 16-level monochrome grayscale ramp */
    outb(0x3C8, 16);
    for (int i = 0; i < 16; i++) {
        uint8_t g = (uint8_t)(i * 63 / 15);
        outb(0x3C9, g);
        outb(0x3C9, g);
        outb(0x3C9, g);
    }

    /* 32..247: 6x6x6 RGB color cube (216 colors) */
    outb(0x3C8, 32);
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                uint8_t vr = (uint8_t)(r * 63 / 5);
                uint8_t vg = (uint8_t)(g * 63 / 5);
                uint8_t vb = (uint8_t)(b * 63 / 5);
                outb(0x3C9, vr);
                outb(0x3C9, vg);
                outb(0x3C9, vb);
            }
        }
    }

    g_palette_initialized = true;
}

void gl_quantize_argb_to_palette_strided(const uint32_t *src, int src_stride,
                                        uint8_t *dst, int dst_stride,
                                        int w, int h) {
    if (!src || !dst || w <= 0 || h <= 0 || src_stride <= 0 || dst_stride <= 0) return;

    if (!g_palette_initialized) {
        gl_palette_init();
    }

    for (int y = 0; y < h; y++) {
        const int8_t *drow = bayer4x4[y & 3];
        const uint32_t *srow = src + y * src_stride;
        uint8_t *drow_dst = dst + y * dst_stride;

        for (int x = 0; x < w; x++) {
            uint32_t c = srow[x];
            int a = (c >> 24) & 0xFF;

            /* Check transparent pixels */
            if (a < 16) {
                drow_dst[x] = 0; /* Background / black */
                continue;
            }

            int bias = (int)drow[x & 3];

            int r = ((c >> 16) & 0xFF) + bias;
            int g = ((c >> 8)  & 0xFF) + bias;
            int b = (c         & 0xFF) + bias;

            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;

            /* Fast map to 6x6x6 color cube: index = 32 + 36 * r6 + 6 * g6 + b6 */
            int r6 = (r * 5 + 128) / 255;
            int g6 = (g * 5 + 128) / 255;
            int b6 = (b * 5 + 128) / 255;

            drow_dst[x] = (uint8_t)(32 + 36 * r6 + 6 * g6 + b6);
        }
    }
}

void gl_quantize_argb_to_palette(const uint32_t *src, uint8_t *dst, int w, int h) {
    gl_quantize_argb_to_palette_strided(src, w, dst, w, w, h);
}

