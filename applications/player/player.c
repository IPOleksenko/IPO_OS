/*
 * player.c - Universal Multimedia Player for IPO_OS
 *
 * Supports:
 * 1. Image Viewer (BMP 24/32-bit, TGA 24/32-bit uncompressed/RLE)
 * 2. Audio Player (WAV PCM 8/16/24/32-bit and IEEE float, arbitrary sample rates)
 * 3. Video Player (IPOV multi-frame video streaming or procedural fallback)
 * 4. Interactive GUI File Browser (/media file selection)
 * 5. Full playback controls: Pause/Play, Rewind, Fast-Forward, Timeline seeking
 * 6. High-efficiency timer pacing (no CPU starvation, smooth OS multitasking)
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <memory/kmalloc.h>
#include <file_system/ipo_fs.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/ipo_gl.h>
#include <GL/gl_math.h>
#include <driver/audio_core.h>
#include <driver/sound.h>
#include <driver/input/mouse.h>
#include <driver/input/keyboard.h>
#include <wm.h>
#include <syscall.h>
#include <system/timer.h>
#include <system/state.h>

#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "applications/micropython/lib/uzlib/uzlib.h"
#include "applications/micropython/lib/uzlib/tinflate.c"
#include "applications/micropython/lib/uzlib/adler32.c"
#include "applications/micropython/lib/uzlib/crc32.c"

#define DEFAULT_WIN_W 240
#define DEFAULT_WIN_H 150
#define WIN_W DEFAULT_WIN_W
#define WIN_H DEFAULT_WIN_H

/* Standard scancodes */
#define SC_ESC       0x01
#define SC_BACKSPACE 0x0E
#define SC_T         0x14
#define SC_O         0x18
#define SC_ENTER     0x1C
#define SC_F         0x21
#define SC_B         0x30
#define SC_SPACE     0x39
#define SC_HOME      0x47
#define SC_UP        0x48
#define SC_PAGE_UP   0x49
#define SC_LEFT      0x4B
#define SC_RIGHT     0x4D
#define SC_END       0x4F
#define SC_DOWN      0x50
#define SC_PAGE_DOWN 0x51

/* =========================================================================
 * 1. Data Structures & Modes
 * ========================================================================= */

typedef enum {
    UI_STATE_BROWSER,
    UI_STATE_PLAYING
} ui_state_t;

typedef enum {
    MEDIA_MODE_VIDEO,
    MEDIA_MODE_IMAGE,
    MEDIA_MODE_AUDIO,
    MEDIA_MODE_DIR,
    MEDIA_MODE_FILE
} media_mode_t;

typedef struct {
    char         *path;
    char         *name;
    uint32_t     size;
    media_mode_t mode;
} media_entry_t;

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} bmp_file_header_t;

typedef struct {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} bmp_info_header_t;

typedef struct {
    uint8_t  id_length;
    uint8_t  color_map_type;
    uint8_t  image_type;
    uint16_t cm_first_entry;
    uint16_t cm_length;
    uint8_t  cm_entry_size;
    uint16_t x_origin;
    uint16_t y_origin;
    uint16_t width;
    uint16_t height;
    uint8_t  pixel_depth;
    uint8_t  image_descriptor;
} tga_header_t;

typedef struct {
    char     magic[4];       /* "IPOV" */
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint16_t num_frames;
    uint16_t channels;
    uint16_t audio_channels; /* 0 = none, 1 = mono, 2 = stereo */
    uint16_t audio_format;   /* 1 = AUDIO_FORMAT_S16 */
    uint32_t audio_rate;     /* e.g. 22050 */
    uint32_t audio_size;     /* bytes of PCM data following frames */
} vid_header_t;
#pragma pack(pop)

typedef struct {
    int      width;
    int      height;
    uint8_t *rgba;
    bool     has_alpha;
} image_t;

typedef struct {
    int          format;
    int          channels;
    uint32_t     sample_rate;
    const void  *pcm_data;
    size_t       pcm_size;
} wav_info_t;

typedef struct {
    int             width;
    int             height;
    int             fps;
    int             bpp;
    bool            is_bottom_up;
    int             total_frames;
    const uint8_t **frame_ptrs;
    size_t         *frame_sizes;
    /* Audio */
    int             audio_channels;
    uint32_t        audio_rate;
    int             audio_format;
    uint8_t        *audio_pcm;
    size_t          audio_size;
} avi_movie_t;

static avi_movie_t *current_avi = NULL;
static uint8_t     *decoded_mp3_pcm = NULL;
static audio_stream_t *current_audio_stream = NULL;

/* Procedural video buffer */
#define PROCEDURAL_W 64
#define PROCEDURAL_H 64
static uint8_t proc_frame[PROCEDURAL_W * PROCEDURAL_H * 4];

/* =========================================================================
 * 2. Decoders
 * ========================================================================= */

static void free_image(image_t *img) {
    if (!img) return;
    if (img->rgba) kfree(img->rgba);
    kfree(img);
}

static image_t *generate_test_photo(void) {
    int w = 160, h = 120;
    image_t *img = (image_t *)kmalloc(sizeof(image_t));
    if (!img) return NULL;
    img->width = w;
    img->height = h;
    img->rgba = (uint8_t *)kmalloc((size_t)w * h * 4);
    if (!img->rgba) { kfree(img); return NULL; }

    static const uint8_t bar_colors[8][3] = {
        { 255, 255, 255 }, /* White */
        { 255, 255,   0 }, /* Yellow */
        {   0, 255, 255 }, /* Cyan */
        {   0, 255,   0 }, /* Green */
        { 255,   0, 255 }, /* Magenta */
        { 255,   0,   0 }, /* Red */
        {   0,   0, 255 }, /* Blue */
        {  32,  32,  32 }  /* Dark Gray */
    };

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t off = ((size_t)y * w + x) * 4;
            if (y < 75) {
                int bar_idx = (x * 8) / w;
                img->rgba[off + 0] = bar_colors[bar_idx][0];
                img->rgba[off + 1] = bar_colors[bar_idx][1];
                img->rgba[off + 2] = bar_colors[bar_idx][2];
                img->rgba[off + 3] = 0xFF;
            } else if (y < 98) {
                int quad = (x * 4) / w;
                uint8_t grad = (uint8_t)(((x % (w / 4)) * 255) / (w / 4));
                if (quad == 0) {
                    img->rgba[off + 0] = grad; img->rgba[off + 1] = 0; img->rgba[off + 2] = 0;
                } else if (quad == 1) {
                    img->rgba[off + 0] = 0; img->rgba[off + 1] = grad; img->rgba[off + 2] = 0;
                } else if (quad == 2) {
                    img->rgba[off + 0] = 0; img->rgba[off + 1] = 0; img->rgba[off + 2] = grad;
                } else {
                    img->rgba[off + 0] = grad; img->rgba[off + 1] = grad; img->rgba[off + 2] = grad;
                }
                img->rgba[off + 3] = 0xFF;
            } else {
                bool checker = ((x / 8) + (y / 8)) & 1;
                uint8_t c = checker ? 230 : 25;
                img->rgba[off + 0] = c;
                img->rgba[off + 1] = c;
                img->rgba[off + 2] = c;
                img->rgba[off + 3] = 0xFF;
            }
        }
    }
    return img;
}

static image_t *decode_bmp(const uint8_t *data, size_t size) {
    if (size < sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t)) return NULL;
    const bmp_file_header_t *bf = (const bmp_file_header_t *)data;
    if (bf->bfType != 0x4D42) return NULL; /* "BM" */

    const bmp_info_header_t *bi = (const bmp_info_header_t *)(data + sizeof(bmp_file_header_t));
    int w = bi->biWidth;
    int h = bi->biHeight > 0 ? bi->biHeight : -bi->biHeight;
    int bpp = bi->biBitCount;
    if (w <= 0 || h <= 0 || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)) return NULL;
    if (bf->bfOffBits >= size) return NULL;

    image_t *img = (image_t *)kmalloc(sizeof(image_t));
    if (!img) return NULL;
    img->width = w;
    img->height = h;
    img->has_alpha = false;
    img->rgba = (uint8_t *)kmalloc((size_t)w * h * 4);
    if (!img->rgba) { kfree(img); return NULL; }

    const uint8_t *src = data + bf->bfOffBits;

    if (bpp == 1) {
        const uint8_t *palette = data + 14 + bi->biSize;
        int row_stride = ((w + 31) / 32) * 4;
        for (int y = 0; y < h; y++) {
            int src_y = (bi->biHeight > 0) ? (h - 1 - y) : y;
            const uint8_t *row = src + src_y * row_stride;
            for (int x = 0; x < w; x++) {
                uint8_t byte = row[x / 8];
                uint8_t idx = (byte >> (7 - (x % 8))) & 1;
                uint8_t b = palette[idx * 4 + 0];
                uint8_t g = palette[idx * 4 + 1];
                uint8_t r = palette[idx * 4 + 2];
                size_t off = ((size_t)y * w + x) * 4;
                img->rgba[off + 0] = r;
                img->rgba[off + 1] = g;
                img->rgba[off + 2] = b;
                img->rgba[off + 3] = 0xFF;
            }
        }
        return img;
    }

    if (bpp == 4) {
        const uint8_t *palette = data + 14 + bi->biSize;
        int row_stride = ((w * 4 + 31) / 32) * 4;
        for (int y = 0; y < h; y++) {
            int src_y = (bi->biHeight > 0) ? (h - 1 - y) : y;
            const uint8_t *row = src + src_y * row_stride;
            for (int x = 0; x < w; x++) {
                uint8_t byte = row[x / 2];
                uint8_t idx = (x % 2 == 0) ? ((byte >> 4) & 0x0F) : (byte & 0x0F);
                uint8_t b = palette[idx * 4 + 0];
                uint8_t g = palette[idx * 4 + 1];
                uint8_t r = palette[idx * 4 + 2];
                size_t off = ((size_t)y * w + x) * 4;
                img->rgba[off + 0] = r;
                img->rgba[off + 1] = g;
                img->rgba[off + 2] = b;
                img->rgba[off + 3] = 0xFF;
            }
        }
        return img;
    }

    if (bpp == 8) {
        const uint8_t *palette = data + 14 + bi->biSize;
        int row_stride = (w + 3) & ~3;
        for (int y = 0; y < h; y++) {
            int src_y = (bi->biHeight > 0) ? (h - 1 - y) : y;
            const uint8_t *row = src + src_y * row_stride;
            for (int x = 0; x < w; x++) {
                uint8_t idx = row[x];
                uint8_t b = palette[idx * 4 + 0];
                uint8_t g = palette[idx * 4 + 1];
                uint8_t r = palette[idx * 4 + 2];
                size_t off = ((size_t)y * w + x) * 4;
                img->rgba[off + 0] = r;
                img->rgba[off + 1] = g;
                img->rgba[off + 2] = b;
                img->rgba[off + 3] = 0xFF;
            }
        }
        return img;
    }

    if (bpp == 16) {
        int row_stride = (w * 2 + 3) & ~3;
        for (int y = 0; y < h; y++) {
            int src_y = (bi->biHeight > 0) ? (h - 1 - y) : y;
            const uint16_t *row = (const uint16_t *)(src + src_y * row_stride);
            for (int x = 0; x < w; x++) {
                uint16_t val = row[x];
                uint8_t r = (uint8_t)(((val >> 10) & 0x1F) * 255 / 31);
                uint8_t g = (uint8_t)(((val >> 5)  & 0x1F) * 255 / 31);
                uint8_t b = (uint8_t)((val         & 0x1F) * 255 / 31);
                size_t off = ((size_t)y * w + x) * 4;
                img->rgba[off + 0] = r;
                img->rgba[off + 1] = g;
                img->rgba[off + 2] = b;
                img->rgba[off + 3] = 0xFF;
            }
        }
        return img;
    }

    int bytes_per_pix = bpp / 8;
    int row_stride = (w * bytes_per_pix + 3) & ~3;

    for (int y = 0; y < h; y++) {
        int src_y = (bi->biHeight > 0) ? (h - 1 - y) : y;
        const uint8_t *row = src + src_y * row_stride;
        for (int x = 0; x < w; x++) {
            uint8_t b = row[x * bytes_per_pix + 0];
            uint8_t g = row[x * bytes_per_pix + 1];
            uint8_t r = row[x * bytes_per_pix + 2];
            uint8_t a = (bpp == 32) ? row[x * 4 + 3] : 0xFF;
            size_t off = ((size_t)y * w + x) * 4;
            img->rgba[off + 0] = r;
            img->rgba[off + 1] = g;
            img->rgba[off + 2] = b;
            img->rgba[off + 3] = a;
        }
    }

    if (bpp == 32) {
        bool all_zero = true;
        bool has_trans = false;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t a = img->rgba[i * 4 + 3];
            if (a != 0) all_zero = false;
            if (a < 255) has_trans = true;
        }
        if (all_zero) {
            for (size_t i = 0; i < (size_t)w * h; i++) {
                img->rgba[i * 4 + 3] = 0xFF;
            }
            img->has_alpha = false;
        } else {
            img->has_alpha = has_trans;
        }
    }

    return img;
}

static image_t *decode_tga(const uint8_t *data, size_t size) {
    if (size < sizeof(tga_header_t)) return NULL;
    const tga_header_t *th = (const tga_header_t *)data;
    if (th->image_type != 2 && th->image_type != 3 && th->image_type != 10 && th->image_type != 11) return NULL;
    int w = th->width;
    int h = th->height;
    int bpp = th->pixel_depth;
    if (w <= 0 || h <= 0 || (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)) return NULL;

    image_t *img = (image_t *)kmalloc(sizeof(image_t));
    if (!img) return NULL;
    img->width = w;
    img->height = h;
    img->has_alpha = false;
    img->rgba = (uint8_t *)kmalloc((size_t)w * h * 4);
    if (!img->rgba) { kfree(img); return NULL; }

    const uint8_t *src = data + sizeof(tga_header_t) + th->id_length;
    int bytes_per_pix = (bpp == 8) ? 1 : ((bpp == 16) ? 2 : (bpp / 8));

    if (th->image_type == 3) {
        /* 8-bit uncompressed grayscale */
        for (int y = 0; y < h; y++) {
            int src_y = (th->image_descriptor & 0x20) ? y : (h - 1 - y);
            for (int x = 0; x < w; x++) {
                uint8_t val = src[src_y * w + x];
                size_t off = ((size_t)y * w + x) * 4;
                img->rgba[off + 0] = val;
                img->rgba[off + 1] = val;
                img->rgba[off + 2] = val;
                img->rgba[off + 3] = 0xFF;
            }
        }
    } else if (th->image_type == 2) {
        for (int y = 0; y < h; y++) {
            int src_y = (th->image_descriptor & 0x20) ? y : (h - 1 - y);
            for (int x = 0; x < w; x++) {
                const uint8_t *p = src + (src_y * w + x) * bytes_per_pix;
                uint8_t r, g, b, a = 0xFF;
                if (bpp == 16) {
                    uint16_t val = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
                    r = (uint8_t)(((val >> 10) & 0x1F) * 255 / 31);
                    g = (uint8_t)(((val >> 5)  & 0x1F) * 255 / 31);
                    b = (uint8_t)((val         & 0x1F) * 255 / 31);
                } else {
                    b = p[0]; g = p[1]; r = p[2];
                    if (bytes_per_pix == 4) a = p[3];
                }
                size_t off = ((size_t)y * w + x) * 4;
                img->rgba[off + 0] = r;
                img->rgba[off + 1] = g;
                img->rgba[off + 2] = b;
                img->rgba[off + 3] = a;
            }
        }
    } else {
        /* RLE compressed (type 10 or 11) */
        int total = w * h;
        int count = 0;
        const uint8_t *p = src;
        while (count < total && (size_t)(p - data) < size) {
            uint8_t packet = *p++;
            int len = (packet & 0x7F) + 1;
            if (packet & 0x80) {
                uint8_t r, g, b, a = 0xFF;
                if (bpp == 8) {
                    r = g = b = *p++;
                } else if (bpp == 16) {
                    uint16_t val = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
                    p += 2;
                    r = (uint8_t)(((val >> 10) & 0x1F) * 255 / 31);
                    g = (uint8_t)(((val >> 5)  & 0x1F) * 255 / 31);
                    b = (uint8_t)((val         & 0x1F) * 255 / 31);
                } else {
                    b = p[0]; g = p[1]; r = p[2];
                    if (bytes_per_pix == 4) a = p[3];
                    p += bytes_per_pix;
                }
                for (int i = 0; i < len && count < total; i++) {
                    size_t off = (size_t)count * 4;
                    img->rgba[off + 0] = r;
                    img->rgba[off + 1] = g;
                    img->rgba[off + 2] = b;
                    img->rgba[off + 3] = a;
                    count++;
                }
            } else {
                for (int i = 0; i < len && count < total; i++) {
                    uint8_t r, g, b, a = 0xFF;
                    if (bpp == 8) {
                        r = g = b = *p++;
                    } else if (bpp == 16) {
                        uint16_t val = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
                        p += 2;
                        r = (uint8_t)(((val >> 10) & 0x1F) * 255 / 31);
                        g = (uint8_t)(((val >> 5)  & 0x1F) * 255 / 31);
                        b = (uint8_t)((val         & 0x1F) * 255 / 31);
                    } else {
                        b = p[0]; g = p[1]; r = p[2];
                        if (bytes_per_pix == 4) a = p[3];
                        p += bytes_per_pix;
                    }
                    size_t off = (size_t)count * 4;
                    img->rgba[off + 0] = r;
                    img->rgba[off + 1] = g;
                    img->rgba[off + 2] = b;
                    img->rgba[off + 3] = a;
                    count++;
                }
            }
        }
    }

    if (bpp == 32) {
        bool all_zero = true;
        bool has_trans = false;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t a = img->rgba[i * 4 + 3];
            if (a != 0) all_zero = false;
            if (a < 255) has_trans = true;
        }
        if (all_zero) {
            for (size_t i = 0; i < (size_t)w * h; i++) {
                img->rgba[i * 4 + 3] = 0xFF;
            }
            img->has_alpha = false;
        } else {
            img->has_alpha = has_trans;
        }
    }

    return img;
}

static image_t *decode_png(const uint8_t *data, size_t size) {
    if (!data || size < 33) return NULL;
    static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (memcmp(data, png_sig, 8) != 0) return NULL;

    size_t offset = 8;
    int width = 0, height = 0;
    int bit_depth = 0, color_type = 0;
    const uint8_t *palette = NULL;
    int palette_entries = 0;
    const uint8_t *trns_data = NULL;
    size_t trns_len = 0;

    size_t idat_total_len = 0;
    size_t scan_off = 8;
    while (scan_off + 8 <= size) {
        uint32_t chunk_len = ((uint32_t)data[scan_off] << 24) |
                             ((uint32_t)data[scan_off + 1] << 16) |
                             ((uint32_t)data[scan_off + 2] << 8) |
                             (uint32_t)data[scan_off + 3];
        const uint8_t *type = data + scan_off + 4;
        scan_off += 8;
        if (scan_off + chunk_len > size) break;
        if (memcmp(type, "IDAT", 4) == 0) {
            idat_total_len += chunk_len;
        }
        scan_off += chunk_len + 4;
    }

    if (idat_total_len == 0) return NULL;

    uint8_t *idat_buf = (uint8_t *)kmalloc(idat_total_len);
    if (!idat_buf) return NULL;

    size_t idat_copied = 0;

    while (offset + 8 <= size) {
        uint32_t chunk_len = ((uint32_t)data[offset] << 24) |
                             ((uint32_t)data[offset + 1] << 16) |
                             ((uint32_t)data[offset + 2] << 8) |
                             (uint32_t)data[offset + 3];
        const uint8_t *type = data + offset + 4;
        offset += 8;
        if (offset + chunk_len > size) break;

        if (memcmp(type, "IHDR", 4) == 0 && chunk_len >= 13) {
            width = (int)(((uint32_t)data[offset] << 24) | ((uint32_t)data[offset + 1] << 16) |
                          ((uint32_t)data[offset + 2] << 8) | (uint32_t)data[offset + 3]);
            height = (int)(((uint32_t)data[offset + 4] << 24) | ((uint32_t)data[offset + 5] << 16) |
                           ((uint32_t)data[offset + 6] << 8) | (uint32_t)data[offset + 7]);
            bit_depth = data[offset + 8];
            color_type = data[offset + 9];
        } else if (memcmp(type, "PLTE", 4) == 0) {
            palette = data + offset;
            palette_entries = chunk_len / 3;
        } else if (memcmp(type, "tRNS", 4) == 0) {
            trns_data = data + offset;
            trns_len = chunk_len;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (idat_copied + chunk_len <= idat_total_len) {
                memcpy(idat_buf + idat_copied, data + offset, chunk_len);
                idat_copied += chunk_len;
            }
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        offset += chunk_len + 4;
    }

    if (width <= 0 || height <= 0 ||
        (bit_depth != 1 && bit_depth != 2 && bit_depth != 4 && bit_depth != 8 && bit_depth != 16)) {
        kfree(idat_buf);
        return NULL;
    }

    int channels = 0;
    if (color_type == 0) channels = 1;      /* Grayscale */
    else if (color_type == 2) channels = 3; /* RGB */
    else if (color_type == 3) channels = 1; /* Indexed */
    else if (color_type == 4) channels = 2; /* Grayscale + Alpha */
    else if (color_type == 6) channels = 4; /* RGBA */
    else {
        kfree(idat_buf);
        return NULL;
    }

    size_t stride = (bit_depth < 8) ? (((size_t)width * bit_depth + 7) / 8)
                                    : ((size_t)width * channels * (bit_depth / 8));
    size_t uncomp_size = (size_t)(stride + 1) * height;

    uint8_t *uncomp = (uint8_t *)kmalloc(uncomp_size);
    if (!uncomp) {
        kfree(idat_buf);
        return NULL;
    }

    uint8_t *dict = (uint8_t *)kmalloc(32768);
    uzlib_uncomp_t d;
    memset(&d, 0, sizeof(d));
    uzlib_uncompress_init(&d, dict, dict ? 32768 : 0);

    size_t zlib_hdr = (idat_copied >= 2) ? 2 : 0;
    d.source = idat_buf + zlib_hdr;
    d.source_limit = idat_buf + idat_copied - ((idat_copied >= 6) ? 4 : 0);
    d.dest_start = uncomp;
    d.dest = uncomp;
    d.dest_limit = uncomp + uncomp_size;

    int ures = uzlib_uncompress(&d);
    if (dict) kfree(dict);
    kfree(idat_buf);

    if (ures != UZLIB_DONE && ures != UZLIB_OK) {
        kfree(uncomp);
        return NULL;
    }

    image_t *img = (image_t *)kmalloc(sizeof(image_t));
    if (!img) {
        kfree(uncomp);
        return NULL;
    }
    img->width = width;
    img->height = height;
    img->has_alpha = false;
    img->rgba = (uint8_t *)kmalloc((size_t)width * height * 4);
    if (!img->rgba) {
        kfree(img);
        kfree(uncomp);
        return NULL;
    }

    uint8_t *curr_row = (uint8_t *)kmalloc(stride);
    uint8_t *prev_row = (uint8_t *)kmalloc(stride);
    if (!curr_row || !prev_row) {
        if (curr_row) kfree(curr_row);
        if (prev_row) kfree(prev_row);
        kfree(img->rgba);
        kfree(img);
        kfree(uncomp);
        return NULL;
    }
    memset(prev_row, 0, stride);

    const uint8_t *raw_ptr = uncomp;
    int bpp_val = (bit_depth < 8) ? 1 : (channels * (bit_depth / 8));

    bool has_trns_key = false;
    uint8_t trns_r = 0, trns_g = 0, trns_b = 0, trns_gray = 0;
    if (trns_data) {
        if (color_type == 2 && trns_len >= 6) {
            has_trns_key = true;
            trns_r = (bit_depth == 16) ? trns_data[0] : trns_data[1];
            trns_g = (bit_depth == 16) ? trns_data[2] : trns_data[3];
            trns_b = (bit_depth == 16) ? trns_data[4] : trns_data[5];
        } else if (color_type == 0 && trns_len >= 2) {
            has_trns_key = true;
            trns_gray = (bit_depth == 16) ? trns_data[0] : trns_data[1];
        }
    }

    for (int y = 0; y < height; y++) {
        uint8_t filter_type = *raw_ptr++;
        memcpy(curr_row, raw_ptr, stride);
        raw_ptr += stride;

        for (size_t x = 0; x < stride; x++) {
            uint8_t a = (x >= (size_t)bpp_val) ? curr_row[x - bpp_val] : 0;
            uint8_t b = prev_row[x];
            uint8_t c = (x >= (size_t)bpp_val) ? prev_row[x - bpp_val] : 0;

            if (filter_type == 1) {
                curr_row[x] = (uint8_t)(curr_row[x] + a);
            } else if (filter_type == 2) {
                curr_row[x] = (uint8_t)(curr_row[x] + b);
            } else if (filter_type == 3) {
                curr_row[x] = (uint8_t)(curr_row[x] + ((a + b) >> 1));
            } else if (filter_type == 4) {
                int p = (int)a + (int)b - (int)c;
                int pa = p > a ? (p - a) : (a - p);
                int pb = p > b ? (p - b) : (b - p);
                int pc = p > c ? (p - c) : (c - p);
                uint8_t pr = (pa <= pb && pa <= pc) ? a : ((pb <= pc) ? b : c);
                curr_row[x] = (uint8_t)(curr_row[x] + pr);
            }
        }

        for (int x = 0; x < width; x++) {
            size_t dst_off = ((size_t)y * width + x) * 4;
            uint8_t r = 0, g = 0, b = 0, a = 0xFF;

            if (color_type == 6) { /* RGBA */
                if (bit_depth == 16) {
                    r = curr_row[x * 8 + 0];
                    g = curr_row[x * 8 + 2];
                    b = curr_row[x * 8 + 4];
                    a = curr_row[x * 8 + 6];
                } else {
                    r = curr_row[x * 4 + 0];
                    g = curr_row[x * 4 + 1];
                    b = curr_row[x * 4 + 2];
                    a = curr_row[x * 4 + 3];
                }
            } else if (color_type == 2) { /* RGB */
                if (bit_depth == 16) {
                    r = curr_row[x * 6 + 0];
                    g = curr_row[x * 6 + 2];
                    b = curr_row[x * 6 + 4];
                } else {
                    r = curr_row[x * 3 + 0];
                    g = curr_row[x * 3 + 1];
                    b = curr_row[x * 3 + 2];
                }
                if (has_trns_key && r == trns_r && g == trns_g && b == trns_b) {
                    a = 0;
                }
            } else if (color_type == 0) { /* Grayscale */
                if (bit_depth == 16) {
                    g = curr_row[x * 2 + 0];
                } else if (bit_depth == 8) {
                    g = curr_row[x];
                } else if (bit_depth == 4) {
                    g = (uint8_t)(((curr_row[x >> 1] >> ((1 - (x & 1)) * 4)) & 0x0F) * 255 / 15);
                } else if (bit_depth == 2) {
                    g = (uint8_t)(((curr_row[x >> 2] >> ((3 - (x & 3)) * 2)) & 0x03) * 255 / 3);
                } else if (bit_depth == 1) {
                    g = ((curr_row[x >> 3] >> (7 - (x & 7))) & 1) ? 255 : 0;
                }
                r = b = g;
                if (has_trns_key && g == trns_gray) {
                    a = 0;
                }
            } else if (color_type == 3) { /* Indexed */
                uint8_t idx = 0;
                if (bit_depth == 8) {
                    idx = curr_row[x];
                } else if (bit_depth == 4) {
                    idx = (uint8_t)((curr_row[x >> 1] >> ((1 - (x & 1)) * 4)) & 0x0F);
                } else if (bit_depth == 2) {
                    idx = (uint8_t)((curr_row[x >> 2] >> ((3 - (x & 3)) * 2)) & 0x03);
                } else if (bit_depth == 1) {
                    idx = (uint8_t)((curr_row[x >> 3] >> (7 - (x & 7))) & 1);
                }
                if (palette && idx < palette_entries) {
                    r = palette[idx * 3 + 0];
                    g = palette[idx * 3 + 1];
                    b = palette[idx * 3 + 2];
                }
                if (trns_data && idx < trns_len) {
                    a = trns_data[idx];
                }
            } else if (color_type == 4) { /* Grayscale + Alpha */
                if (bit_depth == 16) {
                    g = curr_row[x * 4 + 0];
                    a = curr_row[x * 4 + 2];
                } else {
                    g = curr_row[x * 2 + 0];
                    a = curr_row[x * 2 + 1];
                }
                r = b = g;
            }

            img->rgba[dst_off + 0] = r;
            img->rgba[dst_off + 1] = g;
            img->rgba[dst_off + 2] = b;
            img->rgba[dst_off + 3] = a;
            if (a < 255) {
                img->has_alpha = true;
            }
        }

        memcpy(prev_row, curr_row, stride);
    }

    kfree(curr_row);
    kfree(prev_row);
    kfree(uncomp);

    return img;
}

typedef struct {
    int       width;
    int       height;
    int       num_frames;
    uint8_t  *frames_data;   /* Contiguous: num_frames * width * height * 4 */
    uint16_t *frame_delays;  /* delay in ms for each frame */
} gif_anim_t;

static void free_gif(gif_anim_t *gif) {
    if (!gif) return;
    if (gif->frames_data) kfree(gif->frames_data);
    if (gif->frame_delays) kfree(gif->frame_delays);
    kfree(gif);
}

static gif_anim_t *decode_gif(const uint8_t *data, size_t size) {
    if (!data || size < 14) return NULL;
    if (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0) return NULL;

    int screen_w = (int)(data[6] | (data[7] << 8));
    int screen_h = (int)(data[8] | (data[9] << 8));
    if (screen_w <= 0 || screen_h <= 0 || screen_w > 1024 || screen_h > 768) return NULL;

    uint8_t lsd_packed = data[10];
    bool has_gct = (lsd_packed & 0x80) != 0;
    int gct_entries = 1 << ((lsd_packed & 7) + 1);

    size_t offset = 13;
    const uint8_t *gct = NULL;
    if (has_gct) {
        if (offset + (size_t)gct_entries * 3 > size) return NULL;
        gct = data + offset;
        offset += (size_t)gct_entries * 3;
    }

    size_t frame_bytes = (size_t)screen_w * screen_h * 4;
    uint8_t *canvas = (uint8_t *)kmalloc(frame_bytes);
    if (!canvas) return NULL;
    memset(canvas, 0, frame_bytes);

    size_t cap_frames = 16;
    uint8_t *all_frames = (uint8_t *)kmalloc(cap_frames * frame_bytes);
    uint16_t *frame_delays = (uint16_t *)kmalloc(cap_frames * sizeof(uint16_t));
    if (!all_frames || !frame_delays) {
        if (canvas) kfree(canvas);
        if (all_frames) kfree(all_frames);
        if (frame_delays) kfree(frame_delays);
        return NULL;
    }

    int frame_count = 0;
    uint8_t disposal = 0;
    bool has_transparency = false;
    uint8_t trans_idx = 0;
    uint16_t delay_ms = 66;

    static int16_t prefix[4096];
    static uint8_t suffix[4096];
    static uint8_t stack[4096];

    while (offset < size) {
        uint8_t btype = data[offset++];
        if (btype == 0x3B) { /* Trailer */
            break;
        } else if (btype == 0x21) { /* Extension block */
            if (offset >= size) break;
            uint8_t ext_label = data[offset++];
            if (ext_label == 0xF9) { /* Graphic Control Extension */
                if (offset >= size) break;
                uint8_t block_sz = data[offset++];
                if (block_sz >= 4 && offset + 4 <= size) {
                    uint8_t packed_ctrl = data[offset];
                    disposal = (packed_ctrl >> 2) & 7;
                    has_transparency = (packed_ctrl & 1) != 0;
                    uint16_t delay_cs = (uint16_t)(data[offset + 1] | (data[offset + 2] << 8));
                    trans_idx = data[offset + 3];
                    delay_ms = (delay_cs > 0) ? (delay_cs * 10) : 66;
                }
                offset += block_sz;
                while (offset < size && data[offset] != 0) {
                    offset += 1 + data[offset];
                }
                if (offset < size && data[offset] == 0) offset++;
            } else {
                while (offset < size && data[offset] != 0) {
                    offset += 1 + data[offset];
                }
                if (offset < size && data[offset] == 0) offset++;
            }
        } else if (btype == 0x2C) { /* Image Descriptor */
            if (offset + 9 > size) break;
            uint16_t img_left = (uint16_t)(data[offset] | (data[offset + 1] << 8));
            uint16_t img_top  = (uint16_t)(data[offset + 2] | (data[offset + 3] << 8));
            uint16_t img_w    = (uint16_t)(data[offset + 4] | (data[offset + 5] << 8));
            uint16_t img_h    = (uint16_t)(data[offset + 6] | (data[offset + 7] << 8));
            uint8_t img_packed = data[offset + 8];
            offset += 9;

            bool has_lct = (img_packed & 0x80) != 0;
            bool interlaced = (img_packed & 0x40) != 0;
            const uint8_t *palette = gct;

            if (has_lct) {
                int lct_entries = 1 << ((img_packed & 7) + 1);
                if (offset + (size_t)lct_entries * 3 > size) break;
                palette = data + offset;
                offset += (size_t)lct_entries * 3;
            }
            if (!palette) break;

            if (offset >= size) break;
            uint8_t min_code_sz = data[offset++];
            if (min_code_sz > 11) break;

            size_t total_lzw_bytes = 0;
            size_t probe_off = offset;
            while (probe_off < size && data[probe_off] != 0) {
                uint8_t blen = data[probe_off];
                total_lzw_bytes += blen;
                probe_off += 1 + blen;
            }
            uint8_t *lzw_buf = (uint8_t *)kmalloc(total_lzw_bytes > 0 ? total_lzw_bytes : 1);
            if (!lzw_buf) break;

            size_t lzw_written = 0;
            while (offset < size && data[offset] != 0) {
                uint8_t blen = data[offset++];
                if (offset + blen <= size && lzw_written + blen <= total_lzw_bytes) {
                    memcpy(lzw_buf + lzw_written, data + offset, blen);
                    lzw_written += blen;
                }
                offset += blen;
            }
            if (offset < size && data[offset] == 0) offset++;

            size_t total_pixels = (size_t)img_w * img_h;
            uint8_t *pixel_indices = (uint8_t *)kmalloc(total_pixels > 0 ? total_pixels : 1);
            if (!pixel_indices) {
                kfree(lzw_buf);
                break;
            }

            int clear_code = 1 << min_code_sz;
            int eoi_code = clear_code + 1;
            int code_size = min_code_sz + 1;
            int next_code = eoi_code + 1;

            for (int i = 0; i < clear_code; i++) {
                prefix[i] = -1;
                suffix[i] = (uint8_t)i;
            }

            size_t pixel_idx = 0;
            uint32_t bit_buf = 0;
            int bit_count = 0;
            size_t byte_idx = 0;
            int old_code = -1;

            while (pixel_idx < total_pixels) {
                while (bit_count < code_size) {
                    if (byte_idx >= lzw_written) break;
                    bit_buf |= ((uint32_t)lzw_buf[byte_idx++]) << bit_count;
                    bit_count += 8;
                }
                if (bit_count < code_size) break;

                int code = (int)(bit_buf & ((1u << code_size) - 1u));
                bit_buf >>= code_size;
                bit_count -= code_size;

                if (code == clear_code) {
                    code_size = min_code_sz + 1;
                    next_code = eoi_code + 1;
                    old_code = -1;
                    continue;
                } else if (code == eoi_code) {
                    break;
                }

                if (old_code == -1) {
                    pixel_indices[pixel_idx++] = suffix[code];
                    old_code = code;
                    continue;
                }

                int curr = code;
                int stack_ptr = 0;
                if (code >= next_code) {
                    curr = old_code;
                    while (curr >= 0 && curr < 4096 && stack_ptr < 4096) {
                        stack[stack_ptr++] = suffix[curr];
                        curr = prefix[curr];
                    }
                    uint8_t first_char = (stack_ptr > 0) ? stack[stack_ptr - 1] : 0;
                    for (int i = stack_ptr - 1; i >= 0; i--) {
                        if (pixel_idx < total_pixels) pixel_indices[pixel_idx++] = stack[i];
                    }
                    if (pixel_idx < total_pixels) pixel_indices[pixel_idx++] = first_char;

                    if (next_code < 4096) {
                        prefix[next_code] = (int16_t)old_code;
                        suffix[next_code] = first_char;
                        next_code++;
                        if (next_code == (1 << code_size) && code_size < 12) {
                            code_size++;
                        }
                    }
                } else {
                    while (curr >= 0 && curr < 4096 && stack_ptr < 4096) {
                        stack[stack_ptr++] = suffix[curr];
                        curr = prefix[curr];
                    }
                    uint8_t first_char = (stack_ptr > 0) ? stack[stack_ptr - 1] : 0;
                    for (int i = stack_ptr - 1; i >= 0; i--) {
                        if (pixel_idx < total_pixels) pixel_indices[pixel_idx++] = stack[i];
                    }

                    if (next_code < 4096) {
                        prefix[next_code] = (int16_t)old_code;
                        suffix[next_code] = first_char;
                        next_code++;
                        if (next_code == (1 << code_size) && code_size < 12) {
                            code_size++;
                        }
                    }
                }
                old_code = code;
            }

            kfree(lzw_buf);

            int dest_row = 0;
            int pass = 1;
            int step = 8;

            for (int iy = 0; iy < (int)img_h; iy++) {
                int row = iy;
                if (interlaced) {
                    row = dest_row;
                    dest_row += step;
                    if (dest_row >= (int)img_h) {
                        pass++;
                        if (pass == 2) { dest_row = 4; step = 8; }
                        else if (pass == 3) { dest_row = 2; step = 4; }
                        else if (pass == 4) { dest_row = 1; step = 2; }
                    }
                }

                int cy = (int)img_top + row;
                if (cy < 0 || cy >= screen_h) continue;

                for (int ix = 0; ix < (int)img_w; ix++) {
                    int cx = (int)img_left + ix;
                    if (cx < 0 || cx >= screen_w) continue;

                    size_t p_off = (size_t)iy * img_w + ix;
                    if (p_off >= total_pixels) continue;
                    uint8_t cidx = pixel_indices[p_off];

                    if (has_transparency && cidx == trans_idx) {
                        continue;
                    }

                    uint8_t r = palette[cidx * 3 + 0];
                    uint8_t g = palette[cidx * 3 + 1];
                    uint8_t b = palette[cidx * 3 + 2];

                    size_t coff = ((size_t)cy * screen_w + cx) * 4;
                    canvas[coff + 0] = r;
                    canvas[coff + 1] = g;
                    canvas[coff + 2] = b;
                    canvas[coff + 3] = 0xFF;
                }
            }

            kfree(pixel_indices);

            if ((size_t)frame_count >= cap_frames) {
                size_t new_cap = cap_frames * 2;
                uint8_t *new_frames = (uint8_t *)kmalloc(new_cap * frame_bytes);
                uint16_t *new_delays = (uint16_t *)kmalloc(new_cap * sizeof(uint16_t));
                if (new_frames && new_delays) {
                    memcpy(new_frames, all_frames, frame_count * frame_bytes);
                    memcpy(new_delays, frame_delays, frame_count * sizeof(uint16_t));
                    kfree(all_frames);
                    kfree(frame_delays);
                    all_frames = new_frames;
                    frame_delays = new_delays;
                    cap_frames = new_cap;
                } else {
                    if (new_frames) kfree(new_frames);
                    if (new_delays) kfree(new_delays);
                    break;
                }
            }

            memcpy(all_frames + (size_t)frame_count * frame_bytes, canvas, frame_bytes);
            frame_delays[frame_count] = delay_ms;
            frame_count++;

            if (disposal == 2) {
                for (int iy = 0; iy < (int)img_h; iy++) {
                    int cy = (int)img_top + iy;
                    if (cy < 0 || cy >= screen_h) continue;
                    for (int ix = 0; ix < (int)img_w; ix++) {
                        int cx = (int)img_left + ix;
                        if (cx < 0 || cx >= screen_w) continue;
                        size_t coff = ((size_t)cy * screen_w + cx) * 4;
                        canvas[coff + 0] = 0;
                        canvas[coff + 1] = 0;
                        canvas[coff + 2] = 0;
                        canvas[coff + 3] = 0;
                    }
                }
            }
        }
    }

    kfree(canvas);

    if (frame_count == 0) {
        kfree(all_frames);
        kfree(frame_delays);
        return NULL;
    }

    gif_anim_t *gif = (gif_anim_t *)kmalloc(sizeof(gif_anim_t));
    if (!gif) {
        kfree(all_frames);
        kfree(frame_delays);
        return NULL;
    }
    gif->width = screen_w;
    gif->height = screen_h;
    gif->num_frames = frame_count;
    gif->frames_data = all_frames;
    gif->frame_delays = frame_delays;
    return gif;
}

static bool parse_wav(const uint8_t *data, size_t size, wav_info_t *out) {
    if (size < 44) return false;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return false;

    size_t offset = 12;
    uint16_t wFormatTag = 0, nChannels = 0, wBitsPerSample = 0;
    uint32_t nSamplesPerSec = 0;
    const uint8_t *pcm_ptr = NULL;
    size_t pcm_len = 0;

    while (offset + 8 <= size) {
        char chunk_id[5] = {0};
        memcpy(chunk_id, data + offset, 4);
        uint32_t chunk_sz = *(const uint32_t *)(data + offset + 4);
        offset += 8;

        if (strcmp(chunk_id, "fmt ") == 0 && chunk_sz >= 16) {
            wFormatTag      = *(const uint16_t *)(data + offset + 0);
            nChannels       = *(const uint16_t *)(data + offset + 2);
            nSamplesPerSec  = *(const uint32_t *)(data + offset + 4);
            wBitsPerSample  = *(const uint16_t *)(data + offset + 14);
        } else if (strcmp(chunk_id, "data") == 0) {
            pcm_ptr = data + offset;
            pcm_len = (offset + chunk_sz <= size) ? chunk_sz : (size - offset);
        }
        offset += (chunk_sz + 1) & ~1;
    }

    if (!pcm_ptr || nChannels == 0 || nSamplesPerSec == 0) return false;

    out->channels = nChannels;
    out->sample_rate = nSamplesPerSec;
    out->pcm_data = pcm_ptr;
    out->pcm_size = pcm_len;

    if (wFormatTag == 3) {
        out->format = AUDIO_FORMAT_F32;
    } else {
        if (wBitsPerSample == 8)       out->format = AUDIO_FORMAT_U8;
        else if (wBitsPerSample == 16) out->format = AUDIO_FORMAT_S16;
        else if (wBitsPerSample == 24) out->format = AUDIO_FORMAT_S24;
        else if (wBitsPerSample == 32) out->format = AUDIO_FORMAT_S32;
        else return false;
    }
    return true;
}

static uint8_t *current_file_data = NULL;
static size_t   current_file_size = 0;
static int      current_stream_fd = -1;
static size_t   current_file_buffered = 0;

typedef struct {
    mp3dec_t mp3d;
    const uint8_t *data;
    size_t size;
    size_t offset;
    int16_t *pcm_out;
    size_t ring_samples;
    size_t pcm_total_samples;
    size_t total_expected_samples;
    int channels;
    uint32_t hz;
    int downsample;
    bool is_active;
    bool is_eof;
} mp3_stream_decoder_t;

static mp3_stream_decoder_t g_mp3_dec;

static void pump_file_stream(void) {
    if (current_stream_fd >= 0 && current_file_data && current_file_buffered < current_file_size) {
        if (current_file_buffered <= g_mp3_dec.offset || current_file_buffered - g_mp3_dec.offset < 65536) {
            size_t to_read = current_file_size - current_file_buffered;
            if (to_read > 64 * 1024) to_read = 64 * 1024;
            int rd = ipo_read(current_stream_fd, current_file_data + current_file_buffered, (uint32_t)to_read, (uint32_t)current_file_buffered);
            if (rd > 0) {
                current_file_buffered += (size_t)rd;
            } else {
                ipo_close(current_stream_fd);
                current_stream_fd = -1;
            }
            if (current_file_buffered >= current_file_size && current_stream_fd >= 0) {
                ipo_close(current_stream_fd);
                current_stream_fd = -1;
            }
        }
    }
}

static void tick_mp3_decode(void) {
    if (!g_mp3_dec.is_active || g_mp3_dec.is_eof) return;

    size_t played_samples = 0;
    if (current_audio_stream) {
        played_samples = (size_t)current_audio_stream->fractional_pos;
    }

    size_t buffered_ahead = 0;
    if (g_mp3_dec.pcm_total_samples > played_samples) {
        buffered_ahead = g_mp3_dec.pcm_total_samples - played_samples;
    }

    /* Do not overwrite unplayed samples in ring buffer */
    if (g_mp3_dec.ring_samples > 4096 && buffered_ahead >= g_mp3_dec.ring_samples - 4096) {
        return;
    }

    /* If we already have >= 4.5 seconds of decoded audio buffered ahead (~100,000 samples at 22kHz), rest! */
    if (buffered_ahead >= 100000) {
        return;
    }

    pump_file_stream();

    /* Burst decode: if low (< 2 sec), decode up to 16 frames, otherwise up to 8 frames */
    int max_frames = (buffered_ahead < 44100) ? 16 : 8;

    mp3dec_frame_info_t info;
    int16_t pcm_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];

    size_t avail = (current_file_buffered > 0 && current_file_buffered < g_mp3_dec.size) ? current_file_buffered : g_mp3_dec.size;

    for (int f = 0; f < max_frames && g_mp3_dec.offset < g_mp3_dec.size; f++) {
        if (g_mp3_dec.offset + 2048 > avail) {
            pump_file_stream();
            avail = (current_file_buffered > 0 && current_file_buffered < g_mp3_dec.size) ? current_file_buffered : g_mp3_dec.size;
            if (g_mp3_dec.offset + 2048 > avail && avail < g_mp3_dec.size) {
                break;
            }
        }
        if (g_mp3_dec.offset >= avail) {
            if (avail >= g_mp3_dec.size) {
                g_mp3_dec.is_eof = true;
            }
            break;
        }

        int samples = mp3dec_decode_frame(&g_mp3_dec.mp3d, g_mp3_dec.data + g_mp3_dec.offset,
                                          (int)(avail - g_mp3_dec.offset), pcm_buf, &info);
        if (info.frame_bytes <= 0) {
            g_mp3_dec.offset++;
            continue;
        }
        g_mp3_dec.offset += (size_t)info.frame_bytes;

        if (samples > 0) {
            int out_samples = samples / g_mp3_dec.downsample;
            for (int s = 0; s < out_samples; s++) {
                size_t s_idx = (g_mp3_dec.pcm_total_samples + s) % g_mp3_dec.ring_samples;
                if (g_mp3_dec.channels == 2) {
                    if (g_mp3_dec.downsample == 2) {
                        int32_t l = (int32_t)pcm_buf[s * 4 + 0] + (int32_t)pcm_buf[s * 4 + 2];
                        int32_t r = (int32_t)pcm_buf[s * 4 + 1] + (int32_t)pcm_buf[s * 4 + 3];
                        g_mp3_dec.pcm_out[s_idx * 2 + 0] = (int16_t)(l / 2);
                        g_mp3_dec.pcm_out[s_idx * 2 + 1] = (int16_t)(r / 2);
                    } else {
                        g_mp3_dec.pcm_out[s_idx * 2 + 0] = pcm_buf[s * 2 + 0];
                        g_mp3_dec.pcm_out[s_idx * 2 + 1] = pcm_buf[s * 2 + 1];
                    }
                } else {
                    if (g_mp3_dec.downsample == 2) {
                        int32_t m = (int32_t)pcm_buf[s * 2 + 0] + (int32_t)pcm_buf[s * 2 + 1];
                        g_mp3_dec.pcm_out[s_idx] = (int16_t)(m / 2);
                    } else {
                        g_mp3_dec.pcm_out[s_idx] = pcm_buf[s];
                    }
                }
            }
            g_mp3_dec.pcm_total_samples += out_samples;
        }
    }

    if (g_mp3_dec.offset >= g_mp3_dec.size) {
        g_mp3_dec.is_eof = true;
        printf("[player] Progressive MP3 decode complete (%u samples)\n", (uint32_t)g_mp3_dec.pcm_total_samples);
    }

    if (current_audio_stream) {
        audio_stream_update_buffered(current_audio_stream, g_mp3_dec.pcm_total_samples, g_mp3_dec.is_eof);
        if (g_mp3_dec.is_eof) {
            current_audio_stream->data_size = g_mp3_dec.pcm_total_samples * 2 * (size_t)g_mp3_dec.channels;
        }
    }
}

static bool decode_mp3(const uint8_t *data, size_t size, wav_info_t *out_info) {
    if (!data || size < 4 || !out_info) return false;
    memset(out_info, 0, sizeof(wav_info_t));

    bool has_id3 = (size >= 10 && memcmp(data, "ID3", 3) == 0);
    bool has_sync = false;
    for (size_t i = 0; i + 1 < size && i < 4096; i++) {
        if (data[i] == 0xFF && (data[i + 1] & 0xE0) == 0xE0) {
            has_sync = true;
            break;
        }
    }
    if (!has_id3 && !has_sync) return false;

    size_t offset = 0;
    if (has_id3) {
        uint32_t tag_size = ((uint32_t)(data[6] & 0x7F) << 21) |
                            ((uint32_t)(data[7] & 0x7F) << 14) |
                            ((uint32_t)(data[8] & 0x7F) << 7)  |
                            ((uint32_t)(data[9] & 0x7F));
        size_t id3_len = 10 + (size_t)tag_size + ((data[5] & 0x10) ? 10 : 0);
        if (id3_len < size) {
            offset = id3_len;
        }
    }

    memset(&g_mp3_dec, 0, sizeof(g_mp3_dec));
    mp3dec_init(&g_mp3_dec.mp3d);
    g_mp3_dec.data = data;
    g_mp3_dec.size = size;
    g_mp3_dec.offset = offset;
    g_mp3_dec.ring_samples = 262144; /* 256K samples = ~11.8s of audio at 22kHz */
    /* Allocate 1MB ring buffer (262144 * 2 * sizeof(int16_t) = 1,048,576 bytes) */
    g_mp3_dec.pcm_out = (int16_t *)kmalloc(g_mp3_dec.ring_samples * 2 * sizeof(int16_t));
    if (!g_mp3_dec.pcm_out) return false;
    memset(g_mp3_dec.pcm_out, 0, g_mp3_dec.ring_samples * 2 * sizeof(int16_t));
    g_mp3_dec.pcm_total_samples = 0;
    g_mp3_dec.is_active = true;
    g_mp3_dec.is_eof = false;

    mp3dec_frame_info_t info;
    int16_t pcm_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];

    printf("[player] Progressive MP3 decoding initial chunk (size=%u)...\n", (uint32_t)size);
    int initial_frames = 0;
    size_t init_avail = (current_file_buffered > 0 && current_file_buffered < size) ? current_file_buffered : size;
    while (g_mp3_dec.offset + 4 < init_avail && g_mp3_dec.pcm_total_samples < 88200 && initial_frames < 12) {
        int samples = mp3dec_decode_frame(&g_mp3_dec.mp3d, data + g_mp3_dec.offset,
                                          (int)(init_avail - g_mp3_dec.offset), pcm_buf, &info);
        if (info.frame_bytes <= 0) {
            g_mp3_dec.offset++;
            continue;
        }
        g_mp3_dec.offset += (size_t)info.frame_bytes;
        initial_frames++;

        if (samples > 0 && info.channels > 0 && info.hz > 0) {
            if (g_mp3_dec.channels == 0) {
                g_mp3_dec.channels = info.channels;
                g_mp3_dec.hz = (info.hz >= 40000) ? ((uint32_t)info.hz / 2) : (uint32_t)info.hz;
                g_mp3_dec.downsample = (info.hz >= 40000) ? 2 : 1;
            }

            int out_samples = samples / g_mp3_dec.downsample;
            for (int s = 0; s < out_samples; s++) {
                size_t s_idx = (g_mp3_dec.pcm_total_samples + s) % g_mp3_dec.ring_samples;
                if (g_mp3_dec.channels == 2) {
                    if (g_mp3_dec.downsample == 2) {
                        int32_t l = (int32_t)pcm_buf[s * 4 + 0] + (int32_t)pcm_buf[s * 4 + 2];
                        int32_t r = (int32_t)pcm_buf[s * 4 + 1] + (int32_t)pcm_buf[s * 4 + 3];
                        g_mp3_dec.pcm_out[s_idx * 2 + 0] = (int16_t)(l / 2);
                        g_mp3_dec.pcm_out[s_idx * 2 + 1] = (int16_t)(r / 2);
                    } else {
                        g_mp3_dec.pcm_out[s_idx * 2 + 0] = pcm_buf[s * 2 + 0];
                        g_mp3_dec.pcm_out[s_idx * 2 + 1] = pcm_buf[s * 2 + 1];
                    }
                } else {
                    if (g_mp3_dec.downsample == 2) {
                        int32_t m = (int32_t)pcm_buf[s * 2 + 0] + (int32_t)pcm_buf[s * 2 + 1];
                        g_mp3_dec.pcm_out[s_idx] = (int16_t)(m / 2);
                    } else {
                        g_mp3_dec.pcm_out[s_idx] = pcm_buf[s];
                    }
                }
            }
            g_mp3_dec.pcm_total_samples += out_samples;
        }
    }

    if (g_mp3_dec.pcm_total_samples == 0 || g_mp3_dec.hz == 0 || g_mp3_dec.channels == 0) {
        kfree(g_mp3_dec.pcm_out);
        g_mp3_dec.pcm_out = NULL;
        g_mp3_dec.is_active = false;
        return false;
    }

    if (info.bitrate_kbps > 0) {
        uint32_t total_sec = (uint32_t)((uint64_t)size * 8 / (info.bitrate_kbps * 1000));
        g_mp3_dec.total_expected_samples = (size_t)total_sec * g_mp3_dec.hz;
    } else {
        g_mp3_dec.total_expected_samples = (size / 16000) * g_mp3_dec.hz;
    }
    if (g_mp3_dec.total_expected_samples < g_mp3_dec.pcm_total_samples) {
        g_mp3_dec.total_expected_samples = g_mp3_dec.pcm_total_samples;
    }

    out_info->format = AUDIO_FORMAT_S16;
    out_info->channels = g_mp3_dec.channels;
    out_info->sample_rate = g_mp3_dec.hz;
    out_info->pcm_data = g_mp3_dec.pcm_out;
    out_info->pcm_size = g_mp3_dec.total_expected_samples * 2 * (size_t)g_mp3_dec.channels;
    printf("[player] Progressive MP3 ready to play! %u samples ready, total ~%u samples, streaming in background...\n",
           (uint32_t)g_mp3_dec.pcm_total_samples, (uint32_t)g_mp3_dec.total_expected_samples);
    return true;
}

static avi_movie_t *parse_avi(const uint8_t *data, size_t size) {
    if (!data || size < 64) return NULL;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "AVI ", 4) != 0) {
        return NULL;
    }

    avi_movie_t *avi = (avi_movie_t *)kmalloc(sizeof(avi_movie_t));
    if (!avi) return NULL;
    memset(avi, 0, sizeof(*avi));
    avi->fps = 15;
    avi->bpp = 24;
    avi->is_bottom_up = true;

    size_t offset = 12;
    while (offset + 8 <= size) {
        uint32_t chunk_id = *(const uint32_t *)(data + offset);
        uint32_t chunk_size = *(const uint32_t *)(data + offset + 4);
        offset += 8;
        if (offset + chunk_size > size) chunk_size = (uint32_t)(size - offset);

        if (chunk_id == 0x5453494Cu /* "LIST" */ && chunk_size >= 4) {
            uint32_t list_type = *(const uint32_t *)(data + offset);
            size_t sub_offset = offset + 4;
            size_t sub_end = offset + chunk_size;

            if (list_type == 0x6C726468u /* "hdrl" */) {
                while (sub_offset + 8 <= sub_end) {
                    uint32_t s_id = *(const uint32_t *)(data + sub_offset);
                    uint32_t s_sz = *(const uint32_t *)(data + sub_offset + 4);
                    sub_offset += 8;
                    if (sub_offset + s_sz > sub_end) s_sz = (uint32_t)(sub_end - sub_offset);

                    if (s_id == 0x68697661u /* "avih" */ && s_sz >= 40) {
                        uint32_t us_per_frame = *(const uint32_t *)(data + sub_offset + 0);
                        avi->total_frames = (int)*(const uint32_t *)(data + sub_offset + 16);
                        avi->width = (int)*(const uint32_t *)(data + sub_offset + 32);
                        avi->height = (int)*(const uint32_t *)(data + sub_offset + 36);
                        if (us_per_frame > 0) avi->fps = (int)(1000000u / us_per_frame);
                    } else if (s_id == 0x5453494Cu /* "LIST" */ && s_sz >= 4) {
                        uint32_t l_type = *(const uint32_t *)(data + sub_offset);
                        if (l_type == 0x6C727473u /* "strl" */) {
                            size_t st_off = sub_offset + 4;
                            size_t st_end = sub_offset + s_sz;
                            uint32_t cur_stream_type = 0;
                            while (st_off + 8 <= st_end) {
                                uint32_t st_id = *(const uint32_t *)(data + st_off);
                                uint32_t st_sz = *(const uint32_t *)(data + st_off + 4);
                                st_off += 8;
                                if (st_off + st_sz > st_end) st_sz = (uint32_t)(st_end - st_off);

                                if (st_id == 0x68727473u /* "strh" */ && st_sz >= 48) {
                                    cur_stream_type = *(const uint32_t *)(data + st_off + 0);
                                    uint32_t scale = *(const uint32_t *)(data + st_off + 20);
                                    uint32_t rate  = *(const uint32_t *)(data + st_off + 24);
                                    if (cur_stream_type == 0x73646976u /* "vids" */ && scale > 0 && rate > 0) {
                                        avi->fps = (int)(rate / scale);
                                    }
                                } else if (st_id == 0x66727473u /* "strf" */) {
                                    if (cur_stream_type == 0x73646976u /* "vids" */ && st_sz >= 40) {
                                        int bw = *(const int32_t *)(data + st_off + 4);
                                        int bh = *(const int32_t *)(data + st_off + 8);
                                        int bpp = (int)*(const uint16_t *)(data + st_off + 14);
                                        avi->width = bw > 0 ? bw : -bw;
                                        avi->height = bh > 0 ? bh : -bh;
                                        avi->is_bottom_up = (bh > 0);
                                        avi->bpp = bpp ? bpp : 24;
                                    } else if (cur_stream_type == 0x73647561u /* "auds" */ && st_sz >= 14) {
                                        avi->audio_format = (int)*(const uint16_t *)(data + st_off + 0);
                                        avi->audio_channels = (int)*(const uint16_t *)(data + st_off + 2);
                                        avi->audio_rate = *(const uint32_t *)(data + st_off + 4);
                                        uint16_t bits = (st_sz >= 16) ? *(const uint16_t *)(data + st_off + 14) : 16;
                                        avi->audio_format = (bits == 8) ? AUDIO_FORMAT_U8 : AUDIO_FORMAT_S16;
                                    }
                                }
                                st_off += (st_sz + 1u) & ~1u;
                            }
                        }
                    }
                    sub_offset += (s_sz + 1u) & ~1u;
                }
            } else if (list_type == 0x69766F6Du /* "movi" */) {
                size_t m_off = sub_offset;
                size_t m_end = sub_end;

                /* Pass 1: count video frames and total audio bytes */
                int v_count = 0;
                size_t a_total_sz = 0;
                size_t scan_off = m_off;
                while (scan_off + 8 <= m_end) {
                    uint32_t cid = *(const uint32_t *)(data + scan_off);
                    uint32_t csz = *(const uint32_t *)(data + scan_off + 4);
                    scan_off += 8;
                    if (scan_off + csz > m_end) csz = (uint32_t)(m_end - scan_off);

                    /* 00dc, 00db, or any video chunk */
                    if (cid == 0x63643030u || cid == 0x62643030u) {
                        v_count++;
                    } else if (cid == 0x62773130u) { /* 01wb */
                        a_total_sz += csz;
                    }
                    scan_off += (csz + 1u) & ~1u;
                }

                if (v_count > 0) {
                    avi->total_frames = v_count;
                    avi->frame_ptrs = (const uint8_t **)kmalloc((size_t)v_count * sizeof(uint8_t *));
                    avi->frame_sizes = (size_t *)kmalloc((size_t)v_count * sizeof(size_t));
                }

                if (a_total_sz > 0 && avi->audio_channels > 0) {
                    avi->audio_pcm = (uint8_t *)kmalloc(a_total_sz);
                    avi->audio_size = a_total_sz;
                }

                /* Pass 2: store pointers */
                int v_idx = 0;
                size_t a_written = 0;
                scan_off = m_off;
                while (scan_off + 8 <= m_end) {
                    uint32_t cid = *(const uint32_t *)(data + scan_off);
                    uint32_t csz = *(const uint32_t *)(data + scan_off + 4);
                    scan_off += 8;
                    if (scan_off + csz > m_end) csz = (uint32_t)(m_end - scan_off);

                    if ((cid == 0x63643030u || cid == 0x62643030u) && avi->frame_ptrs && v_idx < v_count) {
                        avi->frame_ptrs[v_idx] = data + scan_off;
                        avi->frame_sizes[v_idx] = csz;
                        v_idx++;
                    } else if (cid == 0x62773130u && avi->audio_pcm) {
                        if (a_written + csz <= a_total_sz) {
                            memcpy(avi->audio_pcm + a_written, data + scan_off, csz);
                            a_written += csz;
                        }
                    }
                    scan_off += (csz + 1u) & ~1u;
                }
            }
        }
        offset += (chunk_size + 1u) & ~1u;
    }

    if (avi->total_frames <= 0 || avi->width <= 0 || avi->height <= 0) {
        if (avi->frame_ptrs) kfree(avi->frame_ptrs);
        if (avi->frame_sizes) kfree(avi->frame_sizes);
        if (avi->audio_pcm) kfree(avi->audio_pcm);
        kfree(avi);
        return NULL;
    }
    return avi;
}

static void free_avi(avi_movie_t *avi) {
    if (!avi) return;
    if (avi->frame_ptrs) kfree(avi->frame_ptrs);
    if (avi->frame_sizes) kfree(avi->frame_sizes);
    if (avi->audio_pcm) kfree(avi->audio_pcm);
    kfree(avi);
}


static void render_procedural_frame(int frame_no) {
    float t = (float)frame_no * 0.08f;
    for (int y = 0; y < PROCEDURAL_H; y++) {
        float ny = ((float)y / (float)PROCEDURAL_H) * 2.0f - 1.0f;
        for (int x = 0; x < PROCEDURAL_W; x++) {
            float nx = ((float)x / (float)PROCEDURAL_W) * 2.0f - 1.0f;
            float r_dist = gl_sqrtf(nx * nx + ny * ny);

            float ring = gl_sinf(r_dist * 8.0f - t * 4.0f);
            uint8_t red   = (uint8_t)((gl_sinf(nx * 3.0f + t) + 1.0f) * 127.0f);
            uint8_t green = (uint8_t)((ring + 1.0f) * 120.0f);
            uint8_t blue  = (uint8_t)((gl_cosf(ny * 3.0f + t) + 1.0f) * 127.0f);

            size_t off = ((size_t)y * PROCEDURAL_W + x) * 4;
            proc_frame[off + 0] = red;
            proc_frame[off + 1] = green;
            proc_frame[off + 2] = blue;
            proc_frame[off + 3] = 0xFF;
        }
    }
}

/* =========================================================================
 * 3. File Loader Helpers via IPO_FS
 * ========================================================================= */

static char *current_browse_dir = NULL;
static char search_filter[32] = "";
static size_t search_filter_len = 0;

static void set_browse_dir(const char *dir) {
    if (!dir || dir[0] == '\0') dir = "/";
    size_t len = strlen(dir);
    char *nd = (char *)kmalloc(len + 1);
    if (nd) {
        memcpy(nd, dir, len + 1);
        if (current_browse_dir) kfree(current_browse_dir);
        current_browse_dir = nd;
    }
}

static const char *ipo_strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (*needle == '\0') return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n) {
            char ch1 = *h;
            char ch2 = *n;
            if (ch1 >= 'A' && ch1 <= 'Z') ch1 += ('a' - 'A');
            if (ch2 >= 'A' && ch2 <= 'Z') ch2 += ('a' - 'A');
            if (ch1 != ch2) break;
            h++;
            n++;
        }
        if (*n == '\0') return haystack;
    }
    return NULL;
}

static char scancode_to_char(uint8_t sc, bool shift) {
    if (sc >= 0x02 && sc <= 0x0A) {
        const char *norm = "123456789";
        const char *sh = "!@#$%^&*(";
        return shift ? sh[sc - 0x02] : norm[sc - 0x02];
    }
    if (sc == 0x0B) return shift ? ')' : '0';
    if (sc == 0x0C) return shift ? '_' : '-';
    if (sc == 0x34) return shift ? '>' : '.';
    if (sc == 0x35) return shift ? '?' : '/';
    if (sc == 0x39) return ' ';

    static const char row1[] = "qwertyuiop";
    if (sc >= 0x10 && sc <= 0x19) {
        char c = row1[sc - 0x10];
        return (shift) ? (c - 'a' + 'A') : c;
    }
    static const char row2[] = "asdfghjkl";
    if (sc >= 0x1E && sc <= 0x26) {
        char c = row2[sc - 0x1E];
        return (shift) ? (c - 'a' + 'A') : c;
    }
    static const char row3[] = "zxcvbnm";
    if (sc >= 0x2C && sc <= 0x32) {
        char c = row3[sc - 0x2C];
        return (shift) ? (c - 'a' + 'A') : c;
    }
    return '\0';
}

static uint8_t *read_entire_file(const char *path, size_t *out_size) {
    if (!path || !out_size) return NULL;
    struct ipo_inode st;
    if (ipo_stat(path, &st) != 0 || st.size == 0) return NULL;
    printf("[player] Reading %s (%u bytes)...\n", path, (uint32_t)st.size);
    int fd = ipo_open(path);
    if (fd < 0) return NULL;

    const char *dot = strrchr(path, '.');
    bool is_mp3_path = (dot && (strcmp(dot, ".mp3") == 0 || strcmp(dot, ".MP3") == 0));

    size_t alloc_sz = (size_t)st.size + 1;
    if (is_mp3_path && alloc_sz > 16 * 1024 * 1024) {
        alloc_sz = 16 * 1024 * 1024;
    }

    uint8_t *buf = (uint8_t *)kmalloc(alloc_sz);
    if (!buf && is_mp3_path) {
        alloc_sz = 4 * 1024 * 1024;
        buf = (uint8_t *)kmalloc(alloc_sz);
    }
    if (!buf) {
        printf("[player] Failed to allocate memory for %s!\n", path);
        ipo_close(fd);
        return NULL;
    }

    if (is_mp3_path && st.size > 128 * 1024) {
        uint32_t initial_chunk = 128 * 1024;
        int rd = ipo_read(fd, buf, initial_chunk, 0);
        if (rd <= 0) {
            kfree(buf);
            ipo_close(fd);
            return NULL;
        }
        current_stream_fd = fd;
        current_file_buffered = (size_t)rd;
        current_file_size = (size_t)st.size;
        current_file_data = buf;
        buf[initial_chunk] = 0;
        *out_size = (size_t)st.size;
        printf("[player] Stream started: buffered %d/%u bytes\n", rd, (uint32_t)st.size);
        return buf;
    }

    int rd = ipo_read(fd, buf, (uint32_t)st.size, 0);
    printf("[player] ipo_read returned %d bytes\n", rd);
    ipo_close(fd);
    if (rd <= 0) {
        kfree(buf);
        return NULL;
    }
    buf[rd] = 0;
    current_stream_fd = -1;
    current_file_buffered = (size_t)rd;
    current_file_size = (size_t)rd;
    current_file_data = buf;
    *out_size = (size_t)rd;
    return buf;
}

static bool file_exists(const char *path) {
    struct ipo_inode st;
    return (ipo_stat(path, &st) == 0 && st.size > 0);
}

/* =========================================================================
 * 4. Dynamic Media File Browser List
 * ========================================================================= */

static media_entry_t *media_list = NULL;
static size_t         media_count = 0;
static size_t         media_cap = 0;
static int            selected_file_idx = 0;

static void add_media_entry(const char *path, const char *name, uint32_t size, media_mode_t mode) {
    if (media_count >= media_cap) {
        size_t new_cap = (media_cap == 0) ? 8 : media_cap * 2;
        media_entry_t *new_arr = (media_entry_t *)kmalloc(new_cap * sizeof(media_entry_t));
        if (!new_arr) return;
        if (media_list) {
            memcpy(new_arr, media_list, media_count * sizeof(media_entry_t));
            kfree(media_list);
        }
        media_list = new_arr;
        media_cap = new_cap;
    }
    size_t plen = strlen(path);
    media_list[media_count].path = (char *)kmalloc(plen + 1);
    if (media_list[media_count].path) {
        memcpy(media_list[media_count].path, path, plen + 1);
    }
    size_t nlen = strlen(name);
    media_list[media_count].name = (char *)kmalloc(nlen + 1);
    if (media_list[media_count].name) {
        memcpy(media_list[media_count].name, name, nlen + 1);
    }
    media_list[media_count].size = size;
    media_list[media_count].mode = mode;
    media_count++;
}

static void free_media_list(void) {
    if (media_list) {
        for (size_t i = 0; i < media_count; i++) {
            if (media_list[i].path) {
                kfree(media_list[i].path);
                media_list[i].path = NULL;
            }
            if (media_list[i].name) {
                kfree(media_list[i].name);
                media_list[i].name = NULL;
            }
        }
        kfree(media_list);
        media_list = NULL;
    }
    media_count = 0;
    media_cap = 0;
}

static int get_filtered_count(void) {
    if (search_filter_len == 0) return (int)media_count;
    int cnt = 0;
    for (size_t i = 0; i < media_count; i++) {
        if (media_list[i].mode == MEDIA_MODE_DIR && strcmp(media_list[i].name, "..") == 0) {
            cnt++;
        } else if (ipo_strcasestr(media_list[i].name, search_filter) != NULL) {
            cnt++;
        }
    }
    return cnt;
}

static int get_filtered_index(int filtered_idx) {
    if (search_filter_len == 0) return filtered_idx;
    int cnt = 0;
    for (size_t i = 0; i < media_count; i++) {
        if (media_list[i].mode == MEDIA_MODE_DIR && strcmp(media_list[i].name, "..") == 0) {
            if (cnt == filtered_idx) return (int)i;
            cnt++;
        } else if (ipo_strcasestr(media_list[i].name, search_filter) != NULL) {
            if (cnt == filtered_idx) return (int)i;
            cnt++;
        }
    }
    return -1;
}

static void scan_media_files(const char *dir_path) {
    free_media_list();

    if (!dir_path || dir_path[0] == '\0') {
        dir_path = "/";
    }

    /* Add ".." navigation if not in root */
    if (strcmp(dir_path, "/") != 0) {
        add_media_entry("..", "..", 0, MEDIA_MODE_DIR);
    }

    /* Dynamic self-expanding dir_buf, starting at 4KB and doubling without limits */
    size_t dir_buf_cap = 4096;
    char *dir_buf = (char *)kmalloc(dir_buf_cap);
    if (!dir_buf) return;

    while (dir_buf) {
        memset(dir_buf, 0, dir_buf_cap);
        int res = ipo_list_dir(dir_path, dir_buf, (int)dir_buf_cap - 1);
        if (res < 0) {
            kfree(dir_buf);
            return;
        }
        if ((size_t)res >= dir_buf_cap - 256) {
            size_t new_cap = dir_buf_cap * 2;
            char *new_buf = (char *)kmalloc(new_cap);
            if (!new_buf) break;
            kfree(dir_buf);
            dir_buf = new_buf;
            dir_buf_cap = new_cap;
            continue;
        }
        break;
    }

    char *line = dir_buf;
    while (*line) {
        char *next = strchr(line, '\n');
        if (next) *next = '\0';

        /* Strip carriage returns and spaces */
        size_t llen = strlen(line);
        while (llen > 0 && (line[llen - 1] == '\r' || line[llen - 1] == ' ')) {
            line[--llen] = '\0';
        }

        if (llen > 0) {
            bool is_dir = false;
            if (line[llen - 1] == '/') {
                is_dir = true;
                line[--llen] = '\0';
            }

            if (strcmp(line, ".") != 0 && strcmp(line, "..") != 0 && llen > 0) {
                size_t dlen = strlen(dir_path);
                size_t llen_alloc = strlen(line);
                char *full_path = (char *)kmalloc(dlen + llen_alloc + 2);
                if (full_path) {
                    if (strcmp(dir_path, "/") == 0) {
                        snprintf(full_path, dlen + llen_alloc + 2, "/%s", line);
                    } else {
                        snprintf(full_path, dlen + llen_alloc + 2, "%s/%s", dir_path, line);
                    }

                    if (is_dir) {
                        add_media_entry(full_path, line, 0, MEDIA_MODE_DIR);
                    } else {
                        struct ipo_inode st;
                        uint32_t fsz = 0;
                        if (ipo_stat(full_path, &st) == 0) {
                            fsz = (uint32_t)st.size;
                        }
                        media_mode_t mode = MEDIA_MODE_FILE;
                        const char *dot = strrchr(line, '.');
                        if (dot) {
                            if (strcmp(dot, ".avi") == 0 || strcmp(dot, ".AVI") == 0 ||
                                strcmp(dot, ".vid") == 0 || strcmp(dot, ".gif") == 0 || strcmp(dot, ".GIF") == 0) {
                                mode = MEDIA_MODE_VIDEO;
                            } else if (strcmp(dot, ".mp3") == 0 || strcmp(dot, ".MP3") == 0 ||
                                       strcmp(dot, ".wav") == 0 || strcmp(dot, ".WAV") == 0) {
                                mode = MEDIA_MODE_AUDIO;
                            } else if (strcmp(dot, ".bmp") == 0 || strcmp(dot, ".BMP") == 0 ||
                                       strcmp(dot, ".tga") == 0 || strcmp(dot, ".TGA") == 0 ||
                                       strcmp(dot, ".png") == 0 || strcmp(dot, ".PNG") == 0) {
                                mode = MEDIA_MODE_IMAGE;
                            }
                        }
                        add_media_entry(full_path, line, fsz, mode);
                    }
                    kfree(full_path);
                }
            }
        }

        if (!next) break;
        line = next + 1;
    }

    kfree(dir_buf);

    /* Fallback procedural items if empty in root */
    if (media_count == 0) {
        add_media_entry("test_photo", "SMPTE Color Chart (Photo)", 160 * 120 * 4, MEDIA_MODE_IMAGE);
        add_media_entry("procedural", "Procedural Video Demo", 64 * 64 * 4 * 60, MEDIA_MODE_VIDEO);
        add_media_entry("synthesizer", "Synthesizer Audio Demo", 16000, MEDIA_MODE_AUDIO);
    }
}

/* =========================================================================
 * 5. Player Global State & UI Control
 * ========================================================================= */

#define BROWSER_MAX_ROWS 7
#define BROWSER_ROW_H    13
#define BROWSER_LIST_Y   24

static volatile bool app_running = true;
static ui_state_t    ui_state = UI_STATE_BROWSER;
static media_mode_t  current_mode = MEDIA_MODE_VIDEO;
static bool          is_paused = false;

static int            browser_scroll_offset = 0;
static image_t        *current_image = NULL;
static int             image_bg_mode = 2; /* 0: Solid White, 1: Solid Black, 2: Checkerboard Grid (default) */
static uint8_t        *companion_wav_data = NULL;

static vid_header_t  current_vid_hdr;
static uint8_t      *current_vid_frames = NULL;
static bool          vid_frames_allocated = false;
static uint16_t     *vid_frame_delays = NULL;
static int           vid_w = PROCEDURAL_W;
static int           vid_h = PROCEDURAL_H;
static int           vid_total_frames = 60;
static int           vid_fps = 30;
static int           cur_frame_no = 0;

static uint32_t      last_render_time = 0;
static bool          needs_redraw = true;
static char          current_title[64] = "IPO Media Player";

/* Forward declaration */
static void load_and_play(const media_entry_t *entry, wm_window_t *win);

/* Mouse & Key Event Dispatch */
static volatile uint32_t last_click_data = 0xFFFFFFFF;
static volatile bool     has_new_click = false;
static volatile uint32_t last_key_scancode = 0;
static volatile bool     has_new_key = false;
static bool              is_dragging_scrollbar = false;
static int               drag_start_offset = 0;
static int               drag_start_y = 0;
static bool              is_dragging_timeline = false;
static float             timeline_drag_frac = 0.0f;

static void player_seek_to(float frac) {
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    if (current_mode == MEDIA_MODE_VIDEO && vid_total_frames > 0) {
        cur_frame_no = (int)(frac * (float)vid_total_frames);
        if (cur_frame_no >= vid_total_frames) cur_frame_no = vid_total_frames - 1;
        if (cur_frame_no < 0) cur_frame_no = 0;
        if (current_audio_stream) {
            audio_stream_seek(current_audio_stream, (double)frac);
        }
        needs_redraw = true;
        return;
    }

    if (current_mode == MEDIA_MODE_AUDIO) {
        if (!g_mp3_dec.is_active) {
            if (current_audio_stream) {
                audio_stream_seek(current_audio_stream, (double)frac);
            }
            needs_redraw = true;
            return;
        }

        /* Progressive MP3 seek */
        size_t target_sample = (size_t)(frac * (double)g_mp3_dec.total_expected_samples);
        size_t target_offset = (size_t)(frac * (double)g_mp3_dec.size);
        if (target_offset > g_mp3_dec.size - 2048) {
            target_offset = (g_mp3_dec.size > 2048) ? (g_mp3_dec.size - 2048) : 0;
        }

        /* Buffer data if needed from file */
        if (current_stream_fd >= 0 && current_file_data && current_file_buffered < current_file_size) {
            if (target_offset + 131072 > current_file_buffered) {
                size_t needed = target_offset + 131072;
                if (needed > current_file_size) needed = current_file_size;
                if (needed > current_file_buffered) {
                    size_t to_read = needed - current_file_buffered;
                    int rd = ipo_read(current_stream_fd, current_file_data + current_file_buffered, (uint32_t)to_read, (uint32_t)current_file_buffered);
                    if (rd > 0) {
                        current_file_buffered += (size_t)rd;
                    }
                    if (current_file_buffered >= current_file_size) {
                        ipo_close(current_stream_fd);
                        current_stream_fd = -1;
                    }
                }
            }
        }

        /* Find sync word 0xFFE0 */
        size_t sync_pos = target_offset;
        while (sync_pos + 1 < g_mp3_dec.size && sync_pos < target_offset + 4096) {
            if (g_mp3_dec.data[sync_pos] == 0xFF && (g_mp3_dec.data[sync_pos + 1] & 0xE0) == 0xE0) {
                break;
            }
            sync_pos++;
        }
        if (sync_pos + 1 < g_mp3_dec.size && (g_mp3_dec.data[sync_pos] == 0xFF && (g_mp3_dec.data[sync_pos + 1] & 0xE0) == 0xE0)) {
            g_mp3_dec.offset = sync_pos;
        } else {
            g_mp3_dec.offset = target_offset;
        }

        /* Reinitialize decoder and reset sample counter to target */
        mp3dec_init(&g_mp3_dec.mp3d);
        g_mp3_dec.pcm_total_samples = target_sample;
        g_mp3_dec.is_eof = false;

        /* Pre-decode 20 frames into ring buffer at target position */
        mp3dec_frame_info_t info;
        int16_t pcm_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];
        size_t avail = (current_file_buffered > 0 && current_file_buffered < g_mp3_dec.size) ? current_file_buffered : g_mp3_dec.size;

        for (int f = 0; f < 20 && g_mp3_dec.offset + 4 < avail; f++) {
            int samples = mp3dec_decode_frame(&g_mp3_dec.mp3d, g_mp3_dec.data + g_mp3_dec.offset,
                                              (int)(avail - g_mp3_dec.offset), pcm_buf, &info);
            if (info.frame_bytes <= 0) {
                g_mp3_dec.offset++;
                continue;
            }
            g_mp3_dec.offset += (size_t)info.frame_bytes;
            if (samples > 0) {
                int out_samples = samples / g_mp3_dec.downsample;
                for (int s = 0; s < out_samples; s++) {
                    size_t s_idx = (g_mp3_dec.pcm_total_samples + s) % g_mp3_dec.ring_samples;
                    if (g_mp3_dec.channels == 2) {
                        if (g_mp3_dec.downsample == 2) {
                            int32_t l = (int32_t)pcm_buf[s * 4 + 0] + (int32_t)pcm_buf[s * 4 + 2];
                            int32_t r = (int32_t)pcm_buf[s * 4 + 1] + (int32_t)pcm_buf[s * 4 + 3];
                            g_mp3_dec.pcm_out[s_idx * 2 + 0] = (int16_t)(l / 2);
                            g_mp3_dec.pcm_out[s_idx * 2 + 1] = (int16_t)(r / 2);
                        } else {
                            g_mp3_dec.pcm_out[s_idx * 2 + 0] = pcm_buf[s * 2 + 0];
                            g_mp3_dec.pcm_out[s_idx * 2 + 1] = pcm_buf[s * 2 + 1];
                        }
                    } else {
                        if (g_mp3_dec.downsample == 2) {
                            int32_t m = (int32_t)pcm_buf[s * 2 + 0] + (int32_t)pcm_buf[s * 2 + 1];
                            g_mp3_dec.pcm_out[s_idx] = (int16_t)(m / 2);
                        } else {
                            g_mp3_dec.pcm_out[s_idx] = pcm_buf[s];
                        }
                    }
                }
                g_mp3_dec.pcm_total_samples += out_samples;
            }
        }

        /* Update audio stream state */
        if (current_audio_stream) {
            current_audio_stream->fractional_pos = (double)target_sample;
            audio_stream_update_buffered(current_audio_stream, g_mp3_dec.pcm_total_samples, g_mp3_dec.is_eof);
            audio_stream_play(current_audio_stream);
        }
        needs_redraw = true;
    }
}

static void on_window_event(wm_window_t *w, uint32_t event, uint32_t data) {
    (void)w;
    if (event == WM_EVENT_CLOSE) {
        if (current_audio_stream) {
            audio_stream_stop(current_audio_stream);
            audio_stream_destroy(current_audio_stream);
            current_audio_stream = NULL;
        }
        app_running = false;
    } else if (event == WM_EVENT_RESIZE) {
        needs_redraw = true;
    } else if (event == WM_EVENT_CLICK) {
        last_click_data = data;
        has_new_click = true;
    } else if (event == WM_EVENT_KEY_DOWN) {
        last_key_scancode = data;
        has_new_key = true;
    }
}

/* =========================================================================
 * 6. UI Drawing Routines
 * ========================================================================= */

#define CLR_BLACK        0
#define CLR_BLUE         1
#define CLR_GREEN        2
#define CLR_CYAN         3
#define CLR_RED          4
#define CLR_MAGENTA      5
#define CLR_BROWN        6
#define CLR_LIGHT_GRAY   7
#define CLR_DARK_GRAY    8
#define CLR_LIGHT_BLUE   9
#define CLR_LIGHT_GREEN  10
#define CLR_LIGHT_CYAN   11
#define CLR_LIGHT_RED    12
#define CLR_LIGHT_MAGENTA 13
#define CLR_YELLOW       14
#define CLR_WHITE        15

static inline size_t player_bytes_per_sample(int format) {
    switch (format) {
        case AUDIO_FORMAT_U8:  return 1;
        case AUDIO_FORMAT_S16: return 2;
        case AUDIO_FORMAT_S24: return 3;
        case AUDIO_FORMAT_S32:
        case AUDIO_FORMAT_F32: return 4;
        default:               return 2;
    }
}

static uint8_t s_lut_r[256];
static uint8_t s_lut_g[256];
static uint8_t s_lut_b[256];
static bool s_lut_inited = false;

static void init_lut6(void) {
    if (s_lut_inited) return;
    for (int i = 0; i < 256; i++) {
        int v6 = (i * 5 + 128) / 255;
        s_lut_r[i] = (uint8_t)(36 * v6);
        s_lut_g[i] = (uint8_t)(6 * v6);
        s_lut_b[i] = (uint8_t)(v6);
    }
    s_lut_inited = true;
}

static inline uint8_t rgb_to_palette(uint8_t r, uint8_t g, uint8_t b) {
    return (uint8_t)(32 + s_lut_r[r] + s_lut_g[g] + s_lut_b[b]);
}

static void blit_rgba_to_window(wm_window_t *win, const uint8_t *rgba, int src_w, int src_h) {
    if (!win || !win->framebuf || !rgba || src_w <= 0 || src_h <= 0) return;
    init_lut6();
    uint8_t *fb = win->framebuf;
    int bw = win->w;
    int bh = win->h;
    int dst_w = bw;
    int dst_h = (bh > 28) ? (bh - 28) : bh;

    /* Calculate aspect-ratio preserving fit */
    int render_w = dst_w;
    int render_h = (src_h * dst_w) / src_w;
    int off_x = 0;
    int off_y = (dst_h - render_h) / 2;

    if (render_h > dst_h) {
        render_h = dst_h;
        render_w = (src_w * dst_h) / src_h;
        off_x = (dst_w - render_w) / 2;
        off_y = 0;
    }

    /* Fill letterbox bars (white in White mode, black in Black/Grid modes) */
    uint8_t letterbox_clr = (image_bg_mode == 0) ? CLR_WHITE : CLR_BLACK;
    if (off_y > 0) {
        wm_buf_fill_rect(fb, bw, bh, 0, 0, dst_w, off_y, letterbox_clr);
        wm_buf_fill_rect(fb, bw, bh, 0, off_y + render_h, dst_w, dst_h - (off_y + render_h), letterbox_clr);
    }
    if (off_x > 0) {
        wm_buf_fill_rect(fb, bw, bh, 0, off_y, off_x, render_h, letterbox_clr);
        wm_buf_fill_rect(fb, bw, bh, off_x + render_w, off_y, dst_w - (off_x + render_w), render_h, letterbox_clr);
    }

    if (render_w <= 0 || render_h <= 0) return;

    int step_x = (src_w << 16) / render_w;
    int step_y = (src_h << 16) / render_h;
    int cur_y_fp = 0;

    /* Bayer 4x4 Dithering Matrix (scaled for 6x6x6 color cube step 51) */
    static const int8_t bayer4x4[4][4] = {
        { -8,  0, -6,  2 },
        {  4, -4,  6, -2 },
        { -5,  3, -7,  1 },
        {  7, -1,  5, -3 }
    };

    /* Fast direct integer-scaled blit directly to window framebuffer */
    for (int dy = 0; dy < render_h; dy++) {
        int sy = cur_y_fp >> 16;
        cur_y_fp += step_y;
        if (sy >= src_h) sy = src_h - 1;
        const uint8_t *src_line = rgba + (size_t)sy * src_w * 4;
        uint8_t *dst_line = fb + (size_t)(off_y + dy) * bw + off_x;

        int cur_x_fp = 0;
        for (int dx = 0; dx < render_w; dx++) {
            int sx = cur_x_fp >> 16;
            cur_x_fp += step_x;
            if (sx >= src_w) sx = src_w - 1;
            const uint8_t *p = src_line + sx * 4;
            uint8_t a = p[3];

            uint8_t bg_r, bg_g, bg_b;
            if (image_bg_mode == 0) {
                /* Solid White (default clean canvas) */
                bg_r = bg_g = bg_b = 255;
            } else if (image_bg_mode == 1) {
                /* Solid Black */
                bg_r = bg_g = bg_b = 0;
            } else {
                /* Checkerboard Grid (8x8 tiles) using exact 6x6x6 cube palette levels (204 and 153) */
                int cx = (off_x + dx) >> 3;
                int cy = (off_y + dy) >> 3;
                uint8_t v = (((cx + cy) & 1) == 0) ? 204 : 153;
                bg_r = bg_g = bg_b = v;
            }

            if (a < 4) {
                /* Fully transparent or imperceptible sub-1.5% alpha noise */
                dst_line[dx] = rgb_to_palette(bg_r, bg_g, bg_b);
            } else if (a == 255) {
                /* Fully solid pixel: direct palette mapping preserves crisp lines without dither noise */
                dst_line[dx] = rgb_to_palette(p[0], p[1], p[2]);
            } else {
                uint32_t inv_a = 255 - a;
                int r = (int)(((uint32_t)p[0] * a + (uint32_t)bg_r * inv_a + 127) / 255);
                int g = (int)(((uint32_t)p[1] * a + (uint32_t)bg_g * inv_a + 127) / 255);
                int b = (int)(((uint32_t)p[2] * a + (uint32_t)bg_b * inv_a + 127) / 255);

                /* Ordered Bayer dithering for smooth color gradients and translucency */
                int bias = (int)bayer4x4[(off_y + dy) & 3][(off_x + dx) & 3] * 3;
                r += bias; if (r < 0) r = 0; else if (r > 255) r = 255;
                g += bias; if (g < 0) g = 0; else if (g > 255) g = 255;
                b += bias; if (b < 0) b = 0; else if (b > 255) b = 255;

                dst_line[dx] = rgb_to_palette((uint8_t)r, (uint8_t)g, (uint8_t)b);
            }
        }
    }
}

static void blit_avi_frame_to_window(wm_window_t *win, const uint8_t *frame_data, int src_w, int src_h, int bpp, bool is_bottom_up) {
    if (!win || !win->framebuf || !frame_data || src_w <= 0 || src_h <= 0) return;
    init_lut6();
    uint8_t *fb = win->framebuf;
    int bw = win->w;
    int bh = win->h;
    int dst_w = bw;
    int dst_h = (bh > 28) ? (bh - 28) : bh;

    int render_w = dst_w;
    int render_h = (src_h * dst_w) / src_w;
    int off_x = 0;
    int off_y = (dst_h - render_h) / 2;

    if (render_h > dst_h) {
        render_h = dst_h;
        render_w = (src_w * dst_h) / src_h;
        off_x = (dst_w - render_w) / 2;
        off_y = 0;
    }

    if (off_y > 0) {
        wm_buf_fill_rect(fb, bw, bh, 0, 0, dst_w, off_y, CLR_BLACK);
        wm_buf_fill_rect(fb, bw, bh, 0, off_y + render_h, dst_w, dst_h - (off_y + render_h), CLR_BLACK);
    }
    if (off_x > 0) {
        wm_buf_fill_rect(fb, bw, bh, 0, off_y, off_x, render_h, CLR_BLACK);
        wm_buf_fill_rect(fb, bw, bh, off_x + render_w, off_y, dst_w - (off_x + render_w), render_h, CLR_BLACK);
    }

    if (render_w <= 0 || render_h <= 0) return;

    int bpp_bytes = (bpp == 32) ? 4 : 3;
    int row_stride = (bpp == 32) ? (src_w * 4) : ((src_w * 3 + 3) & ~3);

    int step_x = (src_w << 16) / render_w;
    int step_y = (src_h << 16) / render_h;
    int cur_y_fp = 0;

    for (int dy = 0; dy < render_h; dy++) {
        int sy = cur_y_fp >> 16;
        cur_y_fp += step_y;
        if (sy >= src_h) sy = src_h - 1;
        int src_y = is_bottom_up ? (src_h - 1 - sy) : sy;
        const uint8_t *src_line = frame_data + (size_t)src_y * row_stride;
        uint8_t *dst_line = fb + (size_t)(off_y + dy) * bw + off_x;

        int cur_x_fp = 0;
        for (int dx = 0; dx < render_w; dx++) {
            int sx = cur_x_fp >> 16;
            cur_x_fp += step_x;
            if (sx >= src_w) sx = src_w - 1;
            const uint8_t *p = src_line + sx * bpp_bytes;
            uint8_t b = p[0];
            uint8_t g = p[1];
            uint8_t r = p[2];
            dst_line[dx] = rgb_to_palette(r, g, b);
        }
    }
}

/* Draw 3D Button */
static void draw_button(uint8_t *fb, int bw, int bh, int x, int y, int w, int h, const char *label, bool pressed) {
    uint8_t c_bg = CLR_LIGHT_GRAY;
    uint8_t c_tl = pressed ? CLR_BLACK : CLR_WHITE;
    uint8_t c_br = pressed ? CLR_WHITE : CLR_BLACK;

    wm_buf_fill_rect(fb, bw, bh, x, y, w, h, c_bg);
    wm_buf_draw_line(fb, bw, bh, x, y, x + w - 1, y, c_tl);
    wm_buf_draw_line(fb, bw, bh, x, y, x, y + h - 1, c_tl);
    wm_buf_draw_line(fb, bw, bh, x, y + h - 1, x + w - 1, y + h - 1, c_br);
    wm_buf_draw_line(fb, bw, bh, x + w - 1, y, x + w - 1, y + h - 1, c_br);

    if (label) {
        int str_len = (int)strlen(label);
        int tx = x + (w - str_len * 8) / 2 + (pressed ? 1 : 0);
        int ty = y + (h - 8) / 2 + (pressed ? 1 : 0);
        wm_buf_draw_string(fb, bw, bh, tx, ty, label, CLR_BLACK);
    }
}

/* Render File Browser Screen */
static void render_browser_ui(wm_window_t *win) {
    if (!win || !win->framebuf) return;
    uint8_t *fb = win->framebuf;
    int bw = win->w;
    int bh = win->h;

    /* Background */
    wm_buf_fill_rect(fb, bw, bh, 0, 0, bw, bh, CLR_DARK_GRAY);

    /* Header banner */
    wm_buf_fill_rect(fb, bw, bh, 0, 0, bw, 22, CLR_BLUE);
    wm_buf_draw_line(fb, bw, bh, 0, 22, bw - 1, 22, CLR_WHITE);

    char dir_hdr[64];
    snprintf(dir_hdr, sizeof(dir_hdr), "Dir: %s", current_browse_dir);
    wm_buf_draw_string(fb, bw, bh, 6, 2, dir_hdr, CLR_WHITE);

    if (search_filter_len > 0) {
        char filter_hdr[64];
        snprintf(filter_hdr, sizeof(filter_hdr), "Filter: %s_", search_filter);
        wm_buf_draw_string(fb, bw, bh, 6, 12, filter_hdr, CLR_YELLOW);
    } else {
        wm_buf_draw_string(fb, bw, bh, 6, 12, "Type to search / Enter to open / Esc", CLR_LIGHT_CYAN);
    }

    /* Bottom Control Bar */
    int bbar_h = 24;
    int bbar_y = (bh > bbar_h) ? (bh - bbar_h) : 0;
    wm_buf_fill_rect(fb, bw, bh, 0, bbar_y, bw, bbar_h, CLR_LIGHT_GRAY);
    wm_buf_draw_line(fb, bw, bh, 0, bbar_y, bw - 1, bbar_y, CLR_WHITE);

    draw_button(fb, bw, bh, 8, bbar_y + 4, 60, 16, "Open", false);
    draw_button(fb, bw, bh, 74, bbar_y + 4, 60, 16, "Scan", false);

    int filtered_count = get_filtered_count();

    char pos_buf[32];
    snprintf(pos_buf, sizeof(pos_buf), "[%d/%d] Select", selected_file_idx + 1, filtered_count);
    wm_buf_draw_string(fb, bw, bh, 142, bbar_y + 4, pos_buf, CLR_BLACK);
    wm_buf_draw_string(fb, bw, bh, 142, bbar_y + 13, "Enter: Open", CLR_BLACK);

    /* File List Area */
    int list_y = BROWSER_LIST_Y;
    int row_h = BROWSER_ROW_H;
    int avail_h = bbar_y - list_y - 2;
    int max_rows = avail_h / row_h;
    if (max_rows < 1) max_rows = 1;
    int max_scroll = (filtered_count > max_rows) ? (filtered_count - max_rows) : 0;
    if (browser_scroll_offset > max_scroll) browser_scroll_offset = max_scroll;
    if (browser_scroll_offset < 0) browser_scroll_offset = 0;

    bool has_scrollbar = (filtered_count > max_rows);
    int sb_w = 10;
    int row_w = has_scrollbar ? (bw - sb_w - 8) : (bw - 8);

    for (int r = 0; r < max_rows; r++) {
        int fil_idx = browser_scroll_offset + r;
        if (fil_idx >= filtered_count) break;
        int i = get_filtered_index(fil_idx);
        if (i < 0 || (size_t)i >= media_count) break;

        int ry = list_y + r * row_h;
        bool is_sel = (fil_idx == selected_file_idx);

        uint8_t row_bg = is_sel ? CLR_BLUE : ((r % 2 == 0) ? CLR_BLACK : 23);
        wm_buf_fill_rect(fb, bw, bh, 4, ry, row_w, row_h, row_bg);
        if (is_sel) {
            wm_buf_draw_rect(fb, bw, bh, 4, ry, row_w, row_h, CLR_LIGHT_CYAN);
        }

        /* Mode badge */
        const char *badge = "[FILE]";
        uint8_t badge_clr = CLR_LIGHT_GRAY;
        if (media_list[i].mode == MEDIA_MODE_DIR) {
            badge = "[DIR]";
            badge_clr = CLR_LIGHT_GREEN;
        } else {
            const char *fext = strrchr(media_list[i].name, '.');
            if (fext && (strcmp(fext, ".avi") == 0 || strcmp(fext, ".AVI") == 0)) {
                badge = "[AVI]";
                badge_clr = CLR_LIGHT_MAGENTA;
            } else if (fext && (strcmp(fext, ".mp3") == 0 || strcmp(fext, ".MP3") == 0)) {
                badge = "[MP3]";
                badge_clr = CLR_YELLOW;
            } else if (fext && (strcmp(fext, ".wav") == 0 || strcmp(fext, ".WAV") == 0)) {
                badge = "[WAV]";
                badge_clr = CLR_YELLOW;
            } else if (fext && (strcmp(fext, ".png") == 0 || strcmp(fext, ".PNG") == 0)) {
                badge = "[PNG]";
                badge_clr = CLR_LIGHT_CYAN;
            } else if (fext && (strcmp(fext, ".bmp") == 0 || strcmp(fext, ".BMP") == 0)) {
                badge = "[BMP]";
                badge_clr = CLR_LIGHT_CYAN;
            } else if (fext && (strcmp(fext, ".tga") == 0 || strcmp(fext, ".TGA") == 0)) {
                badge = "[TGA]";
                badge_clr = CLR_LIGHT_CYAN;
            } else if (fext && (strcmp(fext, ".gif") == 0 || strcmp(fext, ".GIF") == 0)) {
                badge = "[GIF]";
                badge_clr = CLR_LIGHT_MAGENTA;
            } else if (media_list[i].mode == MEDIA_MODE_VIDEO) {
                badge = "[VID]";
                badge_clr = CLR_LIGHT_MAGENTA;
            } else if (media_list[i].mode == MEDIA_MODE_IMAGE) {
                badge = "[IMG]";
                badge_clr = CLR_LIGHT_CYAN;
            } else if (media_list[i].mode == MEDIA_MODE_AUDIO) {
                badge = "[AUD]";
                badge_clr = CLR_YELLOW;
            }
        }

        wm_buf_draw_string(fb, bw, bh, 8, ry + 2, badge, badge_clr);

        /* Filename */
        wm_buf_draw_string(fb, bw, bh, 48, ry + 2, media_list[i].name, is_sel ? CLR_WHITE : CLR_LIGHT_GRAY);

        /* Filesize / DIR indicator */
        char sz_buf[16];
        if (media_list[i].mode == MEDIA_MODE_DIR) {
            snprintf(sz_buf, sizeof(sz_buf), "<DIR>");
        } else if (media_list[i].size >= 1024 * 1024) {
            snprintf(sz_buf, sizeof(sz_buf), "%u MB", media_list[i].size / (1024 * 1024));
        } else if (media_list[i].size >= 1024) {
            snprintf(sz_buf, sizeof(sz_buf), "%u KB", media_list[i].size / 1024);
        } else {
            snprintf(sz_buf, sizeof(sz_buf), "%u B", media_list[i].size);
        }
        int sz_x = row_w - (int)strlen(sz_buf) * 8 - 2;
        if (sz_x > 120) {
            wm_buf_draw_string(fb, bw, bh, sz_x, ry + 2, sz_buf, CLR_LIGHT_CYAN);
        }
    }

    /* Scrollbar UI if files exceed viewport */
    if (has_scrollbar) {
        int sb_x = bw - sb_w - 3;
        int sb_y = list_y;
        int sb_h = avail_h;

        /* Track */
        wm_buf_fill_rect(fb, bw, bh, sb_x, sb_y, sb_w, sb_h, CLR_DARK_GRAY);
        wm_buf_draw_rect(fb, bw, bh, sb_x, sb_y, sb_w, sb_h, CLR_BLACK);

        /* Up arrow button */
        draw_button(fb, bw, bh, sb_x, sb_y, sb_w, 10, "^", false);

        /* Down arrow button */
        draw_button(fb, bw, bh, sb_x, sb_y + sb_h - 10, sb_w, 10, "v", false);

        /* Draggable Thumb */
        int track_h = sb_h - 20;
        int thumb_h = (max_rows * track_h) / filtered_count;
        if (thumb_h < 12) thumb_h = 12;
        int thumb_y = sb_y + 10 + (browser_scroll_offset * (track_h - thumb_h)) / (max_scroll > 0 ? max_scroll : 1);

        draw_button(fb, bw, bh, sb_x, thumb_y, sb_w, thumb_h, "", false);
        if (thumb_h >= 14) {
            int notch_y = thumb_y + thumb_h / 2;
            wm_buf_draw_line(fb, bw, bh, sb_x + 2, notch_y, sb_x + sb_w - 3, notch_y, CLR_DARK_GRAY);
        }
    }

    wm_invalidate(win);
}

/* Render Player Overlay Toolbar & Timeline */
static void render_player_toolbar(wm_window_t *win) {
    if (!win || !win->framebuf) return;
    uint8_t *fb = win->framebuf;
    int bw = win->w;
    int bh = win->h;

    /* Bottom Control Bar background */
    int bar_h = 28;
    int bar_y = (bh > bar_h) ? (bh - bar_h) : 0;
    wm_buf_fill_rect(fb, bw, bh, 0, bar_y, bw, bar_h, CLR_LIGHT_GRAY);
    wm_buf_draw_line(fb, bw, bh, 0, bar_y, bw - 1, bar_y, CLR_WHITE);

    /* Timeline Bar */
    int t_x = 6;
    int t_y = bar_y + 3;
    int t_w = (bw > 12) ? (bw - 12) : 1;
    int t_h = 6;
    wm_buf_fill_rect(fb, bw, bh, t_x, t_y, t_w, t_h, CLR_BLACK);
    wm_buf_draw_line(fb, bw, bh, t_x, t_y, t_x + t_w - 1, t_y, CLR_DARK_GRAY);

    float prog = 0.0f;
    if (is_dragging_timeline) {
        prog = timeline_drag_frac;
    } else if (current_mode == MEDIA_MODE_VIDEO) {
        if (current_audio_stream) {
            prog = (float)audio_stream_get_progress(current_audio_stream);
        } else if (vid_total_frames > 0) {
            prog = (float)cur_frame_no / (float)vid_total_frames;
        }
    } else if (current_mode == MEDIA_MODE_AUDIO) {
        if (current_audio_stream) {
            prog = (float)audio_stream_get_progress(current_audio_stream);
        }
    } else if (current_mode == MEDIA_MODE_IMAGE) {
        prog = 1.0f;
    }
    if (prog < 0.0f) prog = 0.0f;
    if (prog > 1.0f) prog = 1.0f;

    int fill_w = (int)(prog * (float)t_w);
    if (fill_w > 0) {
        wm_buf_fill_rect(fb, bw, bh, t_x, t_y, fill_w, t_h, CLR_LIGHT_CYAN);
    }

    /* Scrubber slider knob / handle */
    int thumb_x = t_x + (int)(prog * (float)(t_w - 6));
    if (thumb_x < t_x) thumb_x = t_x;
    if (thumb_x > t_x + t_w - 6) thumb_x = t_x + t_w - 6;
    wm_buf_fill_rect(fb, bw, bh, thumb_x, t_y - 2, 6, t_h + 4, CLR_WHITE);
    wm_buf_draw_rect(fb, bw, bh, thumb_x, t_y - 2, 6, t_h + 4, CLR_BLACK);

    /* Buttons: [Files] [<<] [> / ||] [>>] */
    int btn_y = bar_y + 11;
    draw_button(fb, bw, bh, 6, btn_y, 42, 14, "Files", false);
    draw_button(fb, bw, bh, 52, btn_y, 24, 14, "<<", false);
    draw_button(fb, bw, bh, 80, btn_y, 24, 14, is_paused ? ">" : "||", false);
    draw_button(fb, bw, bh, 108, btn_y, 24, 14, ">>", false);

    /* Status Label & Timeline info */
    char status_str[48];
    if (current_mode == MEDIA_MODE_VIDEO) {
        if (current_audio_stream) {
            snprintf(status_str, sizeof(status_str), "%s F:%d/%d @%dfps [A/V]",
                     is_paused ? "[PAUSE]" : "[PLAY]", cur_frame_no + 1, vid_total_frames, vid_fps);
        } else {
            snprintf(status_str, sizeof(status_str), "%s F:%d/%d @%dfps",
                     is_paused ? "[PAUSE]" : "[PLAY]", cur_frame_no + 1, vid_total_frames, vid_fps);
        }
    } else if (current_mode == MEDIA_MODE_AUDIO) {
        double cur_p = current_audio_stream ? audio_stream_get_progress(current_audio_stream) : 0.0;
        size_t bps = current_audio_stream ? (current_audio_stream->sample_rate * player_bytes_per_sample(current_audio_stream->format) * (current_audio_stream->channels > 0 ? current_audio_stream->channels : 1)) : 0;
        uint32_t total_sec = bps ? (uint32_t)(current_audio_stream->data_size / bps) : 0;
        uint32_t cur_sec = (uint32_t)(cur_p * (double)total_sec);
        snprintf(status_str, sizeof(status_str), "%s %02u:%02u/%02u:%02u",
                 is_paused ? "[PAUSE]" : "[PLAY]", cur_sec / 60, cur_sec % 60, total_sec / 60, total_sec % 60);
    } else {
        if (current_image && current_image->has_alpha) {
            static const char *bg_names[] = { "White", "Black", "Grid" };
            const char *bg_str = (image_bg_mode >= 0 && image_bg_mode <= 2) ? bg_names[image_bg_mode] : "White";
            snprintf(status_str, sizeof(status_str), "[PHOTO] %dx%d [A:%s] (T)",
                     current_image->width, current_image->height, bg_str);
        } else {
            snprintf(status_str, sizeof(status_str), "[PHOTO] %dx%d",
                     current_image ? current_image->width : 0, current_image ? current_image->height : 0);
        }
    }
    if (bw >= 240) {
        wm_buf_draw_string(fb, bw, bh, 138, btn_y + 3, status_str, CLR_BLACK);
    }

    wm_invalidate(win);
}


/* =========================================================================
 * 7. Loading and Playback Management
 * ========================================================================= */

static void unload_current_media(void) {
    if (current_stream_fd >= 0) {
        ipo_close(current_stream_fd);
        current_stream_fd = -1;
    }
    current_file_buffered = 0;
    g_mp3_dec.is_active = false;
    g_mp3_dec.is_eof = true;
    g_mp3_dec.pcm_out = NULL;
    if (current_audio_stream) {
        audio_stream_stop(current_audio_stream);
        audio_stream_destroy(current_audio_stream);
        current_audio_stream = NULL;
    }
    if (companion_wav_data) {
        kfree(companion_wav_data);
        companion_wav_data = NULL;
    }
    if (current_avi) {
        free_avi(current_avi);
        current_avi = NULL;
    }
    if (decoded_mp3_pcm) {
        kfree(decoded_mp3_pcm);
        decoded_mp3_pcm = NULL;
    }
    if (current_image) {
        free_image(current_image);
        current_image = NULL;
    }
    if (vid_frames_allocated && current_vid_frames) {
        kfree(current_vid_frames);
    }
    current_vid_frames = NULL;
    vid_frames_allocated = false;

    if (vid_frame_delays) {
        kfree(vid_frame_delays);
        vid_frame_delays = NULL;
    }

    if (current_file_data) {
        kfree(current_file_data);
        current_file_data = NULL;
    }
    current_file_size = 0;
    cur_frame_no = 0;
    is_paused = false;
}

static void load_and_play(const media_entry_t *entry, wm_window_t *win) {
    if (!entry) return;

    if (entry->mode == MEDIA_MODE_DIR) {
        if (strcmp(entry->name, "..") == 0) {
            char *slash = current_browse_dir ? strrchr(current_browse_dir, '/') : NULL;
            if (slash && slash != current_browse_dir) {
                *slash = '\0';
                set_browse_dir(current_browse_dir);
            } else {
                set_browse_dir("/");
            }
        } else {
            const char *base = (current_browse_dir && current_browse_dir[0]) ? current_browse_dir : "/";
            size_t blen = strlen(base);
            size_t nlen = strlen(entry->name);
            char *new_dir = (char *)kmalloc(blen + nlen + 2);
            if (new_dir) {
                if (strcmp(base, "/") == 0) {
                    snprintf(new_dir, blen + nlen + 2, "/%s", entry->name);
                } else {
                    snprintf(new_dir, blen + nlen + 2, "%s/%s", base, entry->name);
                }
                set_browse_dir(new_dir);
                kfree(new_dir);
            }
        }
        search_filter[0] = '\0';
        search_filter_len = 0;
        selected_file_idx = 0;
        browser_scroll_offset = 0;
        scan_media_files(current_browse_dir);
        needs_redraw = true;
        return;
    }
    if (entry->mode == MEDIA_MODE_FILE) {
        return;
    }

    unload_current_media();

    snprintf(current_title, sizeof(current_title), "IPO Player - %s", entry->name);
    wm_set_title(win, current_title);

    current_mode = entry->mode;

    if (strcmp(entry->path, "test_photo") == 0) {
        current_mode = MEDIA_MODE_IMAGE;
        current_image = generate_test_photo();
    } else if (strcmp(entry->path, "procedural") == 0) {
        current_mode = MEDIA_MODE_VIDEO;
        vid_w = PROCEDURAL_W;
        vid_h = PROCEDURAL_H;
        vid_total_frames = 60;
        vid_fps = 30;
        current_vid_frames = NULL;
    } else if (strcmp(entry->path, "synthesizer") == 0) {
        current_mode = MEDIA_MODE_AUDIO;
        static int16_t demo_audio[8000];
        for (int i = 0; i < 8000; i++) {
            int note = (i / 1000) % 4;
            int freq_div = (note == 0) ? 20 : ((note == 1) ? 25 : ((note == 2) ? 30 : 15));
            demo_audio[i] = (int16_t)(((i % freq_div) - freq_div / 2) * 500);
        }
        current_audio_stream = audio_stream_create(AUDIO_FORMAT_S16, 1, 22050, demo_audio, sizeof(demo_audio));
        if (current_audio_stream) {
            current_audio_stream->looping = true;
            audio_stream_play(current_audio_stream);
        }
    } else {
        if (win && win->framebuf) {
            wm_buf_fill_rect(win->framebuf, win->w, win->h, 0, 0, win->w, win->h, CLR_BLACK);
            wm_buf_draw_string(win->framebuf, win->w, win->h, 20, 30, "Opening media file...", CLR_WHITE);
            wm_buf_draw_string(win->framebuf, win->w, win->h, 20, 50, entry->name, CLR_YELLOW);
            wm_invalidate(win);
        }
        current_file_data = read_entire_file(entry->path, &current_file_size);
        if (current_file_data) {
            if (win && win->framebuf) {
                wm_buf_draw_string(win->framebuf, win->w, win->h, 20, 70, "Decoding media...", CLR_LIGHT_CYAN);
                wm_invalidate(win);
            }
            wav_info_t wi;
            gif_anim_t *gif = NULL;
            avi_movie_t *avi = NULL;
            if ((avi = parse_avi(current_file_data, current_file_size)) != NULL) {
                current_mode = MEDIA_MODE_VIDEO;
                current_avi = avi;
                vid_w = avi->width;
                vid_h = avi->height;
                vid_total_frames = avi->total_frames;
                vid_fps = avi->fps;
                current_vid_frames = NULL;
                vid_frames_allocated = false;
                vid_frame_delays = NULL;

                if (avi->audio_pcm && avi->audio_size > 0 && avi->audio_channels > 0) {
                    current_audio_stream = audio_stream_create(avi->audio_format, avi->audio_channels, avi->audio_rate, avi->audio_pcm, avi->audio_size);
                    if (current_audio_stream) {
                        current_audio_stream->looping = false;
                        audio_stream_play(current_audio_stream);
                    }
                }
            } else if (decode_mp3(current_file_data, current_file_size, &wi)) {
                printf("[player] Setting up audio playback for decoded MP3...\n");
                current_mode = MEDIA_MODE_AUDIO;
                decoded_mp3_pcm = (uint8_t *)wi.pcm_data;
                current_audio_stream = audio_stream_create_ring(wi.format, wi.channels, wi.sample_rate,
                                                                wi.pcm_data, g_mp3_dec.ring_samples,
                                                                g_mp3_dec.total_expected_samples);
                if (current_audio_stream) {
                    audio_stream_update_buffered(current_audio_stream, g_mp3_dec.pcm_total_samples, g_mp3_dec.is_eof);
                    current_audio_stream->looping = false;
                    audio_stream_play(current_audio_stream);
                    printf("[player] Audio ring stream created and playing: %p\n", current_audio_stream);
                } else {
                    printf("[player] audio_stream_create_ring returned NULL!\n");
                }
            } else if (parse_wav(current_file_data, current_file_size, &wi)) {
                current_mode = MEDIA_MODE_AUDIO;
                current_audio_stream = audio_stream_create(wi.format, wi.channels, wi.sample_rate, wi.pcm_data, wi.pcm_size);
                if (current_audio_stream) {
                    current_audio_stream->looping = false;
                    audio_stream_play(current_audio_stream);
                }
            } else if ((gif = decode_gif(current_file_data, current_file_size)) != NULL) {
                if (gif->num_frames == 1) {
                    current_mode = MEDIA_MODE_IMAGE;
                    current_image = (image_t *)kmalloc(sizeof(image_t));
                    if (current_image) {
                        current_image->width = gif->width;
                        current_image->height = gif->height;
                        current_image->rgba = gif->frames_data;
                        current_image->has_alpha = false;
                        for (size_t pi = 0; pi < (size_t)gif->width * gif->height; pi++) {
                            if (current_image->rgba[pi * 4 + 3] < 255) {
                                current_image->has_alpha = true;
                                break;
                            }
                        }
                        gif->frames_data = NULL;
                    }
                    free_gif(gif);
                } else {
                    current_mode = MEDIA_MODE_VIDEO;
                    vid_w = gif->width;
                    vid_h = gif->height;
                    vid_total_frames = gif->num_frames;
                    current_vid_frames = gif->frames_data;
                    vid_frames_allocated = true;
                    vid_frame_delays = gif->frame_delays;
                    uint32_t total_d = 0;
                    for (int f = 0; f < gif->num_frames; f++) {
                        total_d += gif->frame_delays[f];
                    }
                    uint32_t avg_delay = (gif->num_frames > 0 && total_d > 0) ? (total_d / (uint32_t)gif->num_frames) : 66;
                    vid_fps = (avg_delay > 0) ? (int)(1000 / avg_delay) : 15;
                    gif->frames_data = NULL;
                    gif->frame_delays = NULL;
                    free_gif(gif);
                }
            } else if ((current_image = decode_png(current_file_data, current_file_size)) != NULL) {
                current_mode = MEDIA_MODE_IMAGE;
            } else if ((current_image = decode_bmp(current_file_data, current_file_size)) != NULL) {
                current_mode = MEDIA_MODE_IMAGE;
            } else if ((current_image = decode_tga(current_file_data, current_file_size)) != NULL) {
                current_mode = MEDIA_MODE_IMAGE;
            } else if (current_file_size >= 18 && memcmp(current_file_data, "IPOV", 4) == 0) {
                current_mode = MEDIA_MODE_VIDEO;
                memset(&current_vid_hdr, 0, sizeof(current_vid_hdr));
                if (current_file_size >= sizeof(vid_header_t)) {
                    memcpy(&current_vid_hdr, current_file_data, sizeof(vid_header_t));
                } else {
                    memcpy(&current_vid_hdr, current_file_data, 18);
                    current_vid_hdr.audio_channels = 0;
                    current_vid_hdr.audio_size = 0;
                }
                vid_w = current_vid_hdr.width;
                vid_h = current_vid_hdr.height;
                vid_total_frames = current_vid_hdr.num_frames;
                vid_fps = current_vid_hdr.fps;

                size_t hdr_size = (current_file_size >= sizeof(vid_header_t) && current_vid_hdr.audio_size > 0) ? sizeof(vid_header_t) : 18;
                current_vid_frames = current_file_data + hdr_size;

                /* 1. Check for embedded audio in video */
                if (current_vid_hdr.audio_size > 0) {
                    size_t video_bytes = (size_t)vid_total_frames * vid_w * vid_h * 4;
                    if (hdr_size + video_bytes + current_vid_hdr.audio_size <= current_file_size) {
                        uint8_t *audio_pcm = current_file_data + hdr_size + video_bytes;
                        int a_fmt = current_vid_hdr.audio_format ? current_vid_hdr.audio_format : AUDIO_FORMAT_S16;
                        int a_chan = current_vid_hdr.audio_channels ? current_vid_hdr.audio_channels : 2;
                        uint32_t a_rate = current_vid_hdr.audio_rate ? current_vid_hdr.audio_rate : 22050;
                        current_audio_stream = audio_stream_create(a_fmt, a_chan, a_rate, audio_pcm, current_vid_hdr.audio_size);
                        if (current_audio_stream) {
                            current_audio_stream->looping = true;
                            audio_stream_play(current_audio_stream);
                        }
                    }
                }

                /* 2. Fallback: Check for companion .wav if video has no embedded audio */
                if (!current_audio_stream && entry && entry->path) {
                    char wav_path[128];
                    strncpy(wav_path, entry->path, sizeof(wav_path) - 1);
                    wav_path[sizeof(wav_path) - 1] = '\0';
                    char *dot = strrchr(wav_path, '.');
                    if (dot) {
                        strcpy(dot, ".wav");
                        size_t wav_sz = 0;
                        uint8_t *wav_data = read_entire_file(wav_path, &wav_sz);
                        if (wav_data) {
                            wav_info_t wi;
                            if (parse_wav(wav_data, wav_sz, &wi)) {
                                companion_wav_data = wav_data;
                                current_audio_stream = audio_stream_create(wi.format, wi.channels, wi.sample_rate, wi.pcm_data, wi.pcm_size);
                                if (current_audio_stream) {
                                    current_audio_stream->looping = true;
                                    audio_stream_play(current_audio_stream);
                                }
                            } else {
                                kfree(wav_data);
                            }
                        }
                    }
                }
            }
        }
    }

    ui_state = UI_STATE_PLAYING;
    needs_redraw = true;
}

/* =========================================================================
 * 8. User Input Handling (Clicks & Keys)
 * ========================================================================= */

static void switch_photo_relative(wm_window_t *win, int delta) {
    if (media_count == 0) return;
    int idx = selected_file_idx;
    for (size_t step = 0; step < media_count; step++) {
        idx = (idx + delta + (int)media_count) % (int)media_count;
        if (media_list[idx].mode == MEDIA_MODE_IMAGE) {
            selected_file_idx = idx;
            load_and_play(&media_list[idx], win);
            return;
        }
    }
}

static void handle_user_input(wm_window_t *win) {
    int bw = win->w;
    int bh = win->h;
    int bbar_h = 24;
    int bbar_y = (bh > bbar_h) ? (bh - bbar_h) : 0;
    int avail_h = bbar_y - BROWSER_LIST_Y - 2;
    int max_rows = avail_h / BROWSER_ROW_H;
    if (max_rows < 1) max_rows = 1;
    int max_scroll = (media_count > (size_t)max_rows) ? (int)(media_count - max_rows) : 0;

    /* Handle Mouse State (Wheel & Dragging) */
    static int32_t last_mouse_scroll = 0;
    static bool mouse_scroll_inited = false;
    mouse_state_t ms;
    mouse_get_state(&ms);

    if (!mouse_scroll_inited) {
        last_mouse_scroll = ms.scroll_pos;
        mouse_scroll_inited = true;
    } else if (ms.scroll_pos != last_mouse_scroll) {
        int32_t dscroll = ms.scroll_pos - last_mouse_scroll;
        last_mouse_scroll = ms.scroll_pos;
        if (ui_state == UI_STATE_BROWSER && (!wm_session_active() || wm_get_focused() == win)) {
            if (dscroll > 0) {
                if (browser_scroll_offset > 0) {
                    browser_scroll_offset--;
                    needs_redraw = true;
                }
            } else if (dscroll < 0) {
                if (browser_scroll_offset < max_scroll) {
                    browser_scroll_offset++;
                    needs_redraw = true;
                }
            }
        }
    }

    /* Active Mouse Dragging */
    if (is_dragging_scrollbar) {
        if (!ms.left_button) {
            is_dragging_scrollbar = false;
        } else {
            int track_h = avail_h - 20;
            if (track_h < 10) track_h = 10;
            int thumb_h = (max_rows * track_h) / (int)media_count;
            if (thumb_h < 12) thumb_h = 12;
            int cur_win_y = ms.y - win->y;
            int delta_y = cur_win_y - drag_start_y;
            int scrollable_pix = track_h - thumb_h;
            if (scrollable_pix > 0 && max_scroll > 0) {
                int new_offset = drag_start_offset + (delta_y * max_scroll) / scrollable_pix;
                if (new_offset < 0) new_offset = 0;
                if (new_offset > max_scroll) new_offset = max_scroll;
                if (new_offset != browser_scroll_offset) {
                    browser_scroll_offset = new_offset;
                    needs_redraw = true;
                }
            }
        }
    }

    if (is_dragging_timeline) {
        if (!ms.left_button) {
            is_dragging_timeline = false;
            player_seek_to(timeline_drag_frac);
        } else {
            int cur_win_x = ms.x - win->x;
            int t_w = (bw > 12) ? (bw - 12) : 1;
            float frac = (float)(cur_win_x - 6) / (float)t_w;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            timeline_drag_frac = frac;
            needs_redraw = true;
        }
    }

    /* Handle Mouse Click */
    if (has_new_click) {
        has_new_click = false;
        int cx = (int)(last_click_data >> 16);
        int cy = (int)(last_click_data & 0xFFFF);

        if (ui_state == UI_STATE_BROWSER) {
            int sb_w = 10;
            int fil_cnt = get_filtered_count();
            int row_w = (fil_cnt > max_rows) ? (bw - sb_w - 8) : (bw - 8);

            /* 1. File rows */
            if (cx >= 4 && cx <= 4 + row_w && cy >= BROWSER_LIST_Y && cy < BROWSER_LIST_Y + max_rows * BROWSER_ROW_H) {
                int clicked_row = (cy - BROWSER_LIST_Y) / BROWSER_ROW_H;
                int clicked_fil_idx = browser_scroll_offset + clicked_row;
                if (clicked_fil_idx >= 0 && clicked_fil_idx < fil_cnt) {
                    int actual_idx = get_filtered_index(clicked_fil_idx);
                    if (actual_idx >= 0 && (size_t)actual_idx < media_count) {
                        if (selected_file_idx == clicked_fil_idx) {
                            load_and_play(&media_list[actual_idx], win);
                            return;
                        } else {
                            selected_file_idx = clicked_fil_idx;
                            needs_redraw = true;
                        }
                    }
                }
            }

            /* 2. Scrollbar clicks */
            if (fil_cnt > max_rows && cx >= bw - sb_w - 3 && cx <= bw - 3) {
                int sb_y = BROWSER_LIST_Y;
                int sb_h = avail_h;
                if (cy >= sb_y && cy < sb_y + 10) {
                    /* Up arrow */
                    if (browser_scroll_offset > 0) browser_scroll_offset--;
                    needs_redraw = true;
                    return;
                } else if (cy >= sb_y + sb_h - 10 && cy <= sb_y + sb_h) {
                    /* Down arrow */
                    if (browser_scroll_offset < max_scroll) browser_scroll_offset++;
                    needs_redraw = true;
                    return;
                } else if (cy >= sb_y + 10 && cy < sb_y + sb_h - 10) {
                    int track_h = sb_h - 20;
                    if (track_h < 10) track_h = 10;
                    int thumb_h = (max_rows * track_h) / fil_cnt;
                    if (thumb_h < 12) thumb_h = 12;
                    int thumb_y = sb_y + 10 + (browser_scroll_offset * (track_h - thumb_h)) / (max_scroll > 0 ? max_scroll : 1);
                    if (cy < thumb_y) {
                        browser_scroll_offset -= max_rows;
                        if (browser_scroll_offset < 0) browser_scroll_offset = 0;
                    } else if (cy > thumb_y + thumb_h) {
                        browser_scroll_offset += max_rows;
                        if (browser_scroll_offset > max_scroll) browser_scroll_offset = max_scroll;
                    } else {
                        is_dragging_scrollbar = true;
                        drag_start_offset = browser_scroll_offset;
                        drag_start_y = cy;
                    }
                    needs_redraw = true;
                    return;
                }
            }

            /* Open button */
            if (cx >= 8 && cx <= 68 && cy >= bbar_y + 4 && cy <= bbar_y + 20) {
                if (selected_file_idx >= 0 && selected_file_idx < fil_cnt) {
                    int actual_idx = get_filtered_index(selected_file_idx);
                    if (actual_idx >= 0 && (size_t)actual_idx < media_count) {
                        load_and_play(&media_list[actual_idx], win);
                        return;
                    }
                }
            }
            /* Scan button */
            if (cx >= 74 && cx <= 134 && cy >= bbar_y + 4 && cy <= bbar_y + 20) {
                scan_media_files(current_browse_dir);
                selected_file_idx = 0;
                browser_scroll_offset = 0;
                needs_redraw = true;
                return;
            }
        } else if (ui_state == UI_STATE_PLAYING) {
            int p_bar_h = 28;
            int p_bar_y = (bh > p_bar_h) ? (bh - p_bar_h) : 0;
            int t_x = 6;
            int t_y = p_bar_y + 3;
            int t_w = (bw > 12) ? (bw - 12) : 1;
            int t_h = 6;

            /* Timeline Click */
            if (cy >= t_y - 2 && cy <= t_y + t_h + 2 && cx >= t_x && cx <= t_x + t_w) {
                float frac = (float)(cx - t_x) / (float)t_w;
                if (frac < 0.0f) frac = 0.0f;
                if (frac > 1.0f) frac = 1.0f;
                is_dragging_timeline = true;
                timeline_drag_frac = frac;
                player_seek_to(frac);
                needs_redraw = true;
                return;
            }

            /* Buttons row at p_bar_y + 11 */
            int btn_y = p_bar_y + 11;
            if (cy >= btn_y && cy <= btn_y + 14) {
                /* [Files] button */
                if (cx >= 6 && cx <= 48) {
                    ui_state = UI_STATE_BROWSER;
                    wm_set_title(win, "IPO Media Player - File Browser");
                    needs_redraw = true;
                    return;
                }

                /* [<<] Rewind button */
                if (cx >= 52 && cx <= 76) {
                    if (current_mode == MEDIA_MODE_IMAGE) {
                        switch_photo_relative(win, -1);
                        return;
                    } else if (current_mode == MEDIA_MODE_VIDEO) {
                        int step = (vid_fps > 0) ? vid_fps : 15;
                        cur_frame_no -= step;
                        if (cur_frame_no < 0) cur_frame_no = 0;
                        if (vid_total_frames > 0) {
                            player_seek_to((float)cur_frame_no / (float)vid_total_frames);
                        }
                    } else if (current_mode == MEDIA_MODE_AUDIO && current_audio_stream) {
                        double cur_p = audio_stream_get_progress(current_audio_stream);
                        player_seek_to((float)(cur_p - 0.05));
                    }
                    needs_redraw = true;
                    return;
                }

                /* [> / ||] Pause / Play toggle */
                if (cx >= 80 && cx <= 104) {
                    is_paused = !is_paused;
                    if (current_audio_stream) {
                        if (is_paused) audio_stream_pause(current_audio_stream);
                        else audio_stream_play(current_audio_stream);
                    }
                    needs_redraw = true;
                    return;
                }

                /* [>>] Fast-Forward button */
                if (cx >= 108 && cx <= 132) {
                    if (current_mode == MEDIA_MODE_IMAGE) {
                        switch_photo_relative(win, 1);
                        return;
                    } else if (current_mode == MEDIA_MODE_VIDEO) {
                        int step = (vid_fps > 0) ? vid_fps : 15;
                        cur_frame_no += step;
                        if (cur_frame_no >= vid_total_frames) cur_frame_no = vid_total_frames - 1;
                        if (vid_total_frames > 0) {
                            player_seek_to((float)cur_frame_no / (float)vid_total_frames);
                        }
                    } else if (current_mode == MEDIA_MODE_AUDIO && current_audio_stream) {
                        double cur_p = audio_stream_get_progress(current_audio_stream);
                        player_seek_to((float)(cur_p + 0.05));
                    }
                    needs_redraw = true;
                    return;
                }

                /* Status text area click: toggle alpha background mode for images */
                if (cx >= 138) {
                    if (current_mode == MEDIA_MODE_IMAGE) {
                        image_bg_mode = (image_bg_mode + 1) % 3;
                        needs_redraw = true;
                        return;
                    }
                }
            }
        }
    }

    if (!wm_session_active()) {
        /* Poll keyboard queue (only in standalone non-GUI mode) */
        uint8_t polled_sc;
        while ((polled_sc = keyboard_get_scancode()) != 0) {
            if (polled_sc == 0xE0) {
                continue; /* Skip extended prefix */
            }
            if (!(polled_sc & 0x80)) {
                last_key_scancode = polled_sc;
                has_new_key = true;
                break;
            }
        }
    }

    /* Handle Key Down */
    if (has_new_key) {
        has_new_key = false;
        uint8_t sc = (uint8_t)(last_key_scancode & 0x7F);

        if (ui_state == UI_STATE_BROWSER) {
            int fil_cnt = get_filtered_count();
            if (sc == SC_UP) {
                if (selected_file_idx > 0) {
                    selected_file_idx--;
                    if (selected_file_idx < browser_scroll_offset) {
                        browser_scroll_offset = selected_file_idx;
                    }
                    needs_redraw = true;
                }
            } else if (sc == SC_DOWN) {
                if (fil_cnt > 0 && selected_file_idx + 1 < fil_cnt) {
                    selected_file_idx++;
                    if (selected_file_idx >= browser_scroll_offset + max_rows) {
                        browser_scroll_offset = selected_file_idx - max_rows + 1;
                    }
                    needs_redraw = true;
                }
            } else if (sc == SC_PAGE_UP) {
                selected_file_idx -= max_rows;
                if (selected_file_idx < 0) selected_file_idx = 0;
                if (selected_file_idx < browser_scroll_offset) {
                    browser_scroll_offset = selected_file_idx;
                }
                needs_redraw = true;
            } else if (sc == SC_PAGE_DOWN) {
                if (fil_cnt > 0) {
                    selected_file_idx += max_rows;
                    if (selected_file_idx >= fil_cnt) selected_file_idx = fil_cnt - 1;
                    if (selected_file_idx >= browser_scroll_offset + max_rows) {
                        browser_scroll_offset = selected_file_idx - max_rows + 1;
                    }
                    needs_redraw = true;
                }
            } else if (sc == SC_HOME) {
                selected_file_idx = 0;
                browser_scroll_offset = 0;
                needs_redraw = true;
            } else if (sc == SC_END) {
                if (fil_cnt > 0) {
                    selected_file_idx = fil_cnt - 1;
                    browser_scroll_offset = max_scroll;
                    needs_redraw = true;
                }
            } else if (sc == SC_ENTER || sc == SC_SPACE) {
                if (selected_file_idx >= 0 && selected_file_idx < fil_cnt) {
                    int actual_idx = get_filtered_index(selected_file_idx);
                    if (actual_idx >= 0 && (size_t)actual_idx < media_count) {
                        load_and_play(&media_list[actual_idx], win);
                    }
                }
            } else if (sc == SC_BACKSPACE) {
                if (search_filter_len > 0) {
                    search_filter[--search_filter_len] = '\0';
                    selected_file_idx = 0;
                    browser_scroll_offset = 0;
                    needs_redraw = true;
                } else if (current_browse_dir && strcmp(current_browse_dir, "/") != 0) {
                    char *slash = strrchr(current_browse_dir, '/');
                    if (slash && slash != current_browse_dir) {
                        *slash = '\0';
                        set_browse_dir(current_browse_dir);
                    } else {
                        set_browse_dir("/");
                    }
                    search_filter[0] = '\0';
                    search_filter_len = 0;
                    selected_file_idx = 0;
                    browser_scroll_offset = 0;
                    scan_media_files(current_browse_dir);
                    needs_redraw = true;
                }
            } else if (sc == SC_ESC) {
                if (search_filter_len > 0) {
                    search_filter[0] = '\0';
                    search_filter_len = 0;
                    selected_file_idx = 0;
                    browser_scroll_offset = 0;
                    needs_redraw = true;
                } else {
                    app_running = false;
                }
            } else {
                char ch = scancode_to_char(sc, false);
                if (ch != '\0' && search_filter_len + 1 < sizeof(search_filter)) {
                    search_filter[search_filter_len++] = ch;
                    search_filter[search_filter_len] = '\0';
                    selected_file_idx = 0;
                    browser_scroll_offset = 0;
                    needs_redraw = true;
                }
            }
        } else if (ui_state == UI_STATE_PLAYING) {
            if (sc == SC_SPACE) {
                is_paused = !is_paused;
                if (current_audio_stream) {
                    if (is_paused) audio_stream_pause(current_audio_stream);
                    else audio_stream_play(current_audio_stream);
                }
                needs_redraw = true;
            } else if (sc == SC_LEFT) {
                if (current_mode == MEDIA_MODE_IMAGE) {
                    switch_photo_relative(win, -1);
                    return;
                } else if (current_mode == MEDIA_MODE_VIDEO) {
                    int step = (vid_fps > 0) ? vid_fps : 15;
                    cur_frame_no -= step;
                    if (cur_frame_no < 0) cur_frame_no = 0;
                    if (vid_total_frames > 0) {
                        player_seek_to((float)cur_frame_no / (float)vid_total_frames);
                    }
                } else if (current_mode == MEDIA_MODE_AUDIO && current_audio_stream) {
                    double cur_p = audio_stream_get_progress(current_audio_stream);
                    player_seek_to((float)(cur_p - 0.05));
                }
                needs_redraw = true;
            } else if (sc == SC_RIGHT) {
                if (current_mode == MEDIA_MODE_IMAGE) {
                    switch_photo_relative(win, 1);
                    return;
                } else if (current_mode == MEDIA_MODE_VIDEO) {
                    int step = (vid_fps > 0) ? vid_fps : 15;
                    cur_frame_no += step;
                    if (cur_frame_no >= vid_total_frames) cur_frame_no = vid_total_frames - 1;
                    if (vid_total_frames > 0) {
                        player_seek_to((float)cur_frame_no / (float)vid_total_frames);
                    }
                } else if (current_mode == MEDIA_MODE_AUDIO && current_audio_stream) {
                    double cur_p = audio_stream_get_progress(current_audio_stream);
                    player_seek_to((float)(cur_p + 0.05));
                }
                needs_redraw = true;
            } else if (sc == SC_T || sc == SC_B) {
                if (current_mode == MEDIA_MODE_IMAGE) {
                    image_bg_mode = (image_bg_mode + 1) % 3;
                    needs_redraw = true;
                }
            } else if (sc == SC_O || sc == SC_F || sc == SC_BACKSPACE) {
                ui_state = UI_STATE_BROWSER;
                wm_set_title(win, "IPO Media Player - File Browser");
                needs_redraw = true;
            } else if (sc == SC_ESC) {
                app_running = false;
            }
        }
    }
}

/* =========================================================================
 * 9. Main Application Entrypoint & Optimized Loop
 * ========================================================================= */

int main(int argc, char **argv) {
    printf("[Media Player] Initializing Universal Media Player...\n");
    keyboard_set_app_input_mode(true);
    audio_init();

    /* Scan media folder */
    set_browse_dir("/");
    scan_media_files(current_browse_dir);

    /* Check if target file passed on command line */
    const char *target_file = (argc >= 2 && argv && argv[1]) ? argv[1] : NULL;

    /* Window options */
    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = current_title;
    uint32_t pid = (uint32_t)ipo_syscall(IPO_SYSCALL_GETPID, 0, NULL);
    if (pid == 0) pid = 1;
    opt.x = 20 + ((pid * 24) % 120);
    opt.y = 14 + ((pid * 18) % 60);
    opt.w = WIN_W;
    opt.h = WIN_H;
    opt.event_cb = on_window_event;

    wm_window_t *win = wm_create_window(&opt);
    if (!win) {
        free_media_list();
        if (current_browse_dir) { kfree(current_browse_dir); current_browse_dir = NULL; }
        return 1;
    }

    gl_palette_init();

    /* If CLI argument given, load it immediately */
    if (target_file) {
        media_entry_t cli_entry;
        cli_entry.path = (char *)target_file;
        const char *slash = strrchr(target_file, '/');
        cli_entry.name = (char *)(slash ? slash + 1 : target_file);
        cli_entry.size = 0;
        cli_entry.mode = MEDIA_MODE_VIDEO;

        const char *dot = strrchr(target_file, '.');
        if (dot) {
            if (strcmp(dot, ".bmp") == 0 || strcmp(dot, ".BMP") == 0 ||
                strcmp(dot, ".tga") == 0 || strcmp(dot, ".TGA") == 0 ||
                strcmp(dot, ".png") == 0 || strcmp(dot, ".PNG") == 0) cli_entry.mode = MEDIA_MODE_IMAGE;
            else if (strcmp(dot, ".wav") == 0 || strcmp(dot, ".WAV") == 0 || strcmp(dot, ".mp3") == 0 || strcmp(dot, ".MP3") == 0) cli_entry.mode = MEDIA_MODE_AUDIO;
            else if (strcmp(dot, ".avi") == 0 || strcmp(dot, ".AVI") == 0 || strcmp(dot, ".vid") == 0 || strcmp(dot, ".gif") == 0 || strcmp(dot, ".GIF") == 0) cli_entry.mode = MEDIA_MODE_VIDEO;
        }
        load_and_play(&cli_entry, win);
    } else {
        ui_state = UI_STATE_BROWSER;
        wm_set_title(win, "IPO Media Player - File Browser");
        needs_redraw = true;
    }

    uint32_t last_tick = timer_millis();

    while (app_running && wm_session_active() && !system_is_interrupted() && wm_is_window_valid(win)) {
        /* Progressive MP3 background decode & audio engine update */
        tick_mp3_decode();
        pump_file_stream();
        audio_tick();

        /* Process mouse & keyboard interactions */
        handle_user_input(win);

        uint32_t now = timer_millis();
        uint32_t elapsed = now - last_tick;

        if (ui_state == UI_STATE_BROWSER) {
            if (needs_redraw) {
                render_browser_ui(win);
                needs_redraw = false;
            }
            /* High efficiency idle: sleep so CPU stays cool and free */
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
            continue;
        }

        /* UI_STATE_PLAYING */
        if (current_mode == MEDIA_MODE_IMAGE) {
            /* Photo mode: render once and idle */
            if (needs_redraw) {
                int bw = win->w;
                int bh = win->h;
                int disp_h = (bh > 28) ? (bh - 28) : bh;
                if (current_image && current_image->rgba) {
                    blit_rgba_to_window(win, current_image->rgba, current_image->width, current_image->height);
                } else {
                    wm_buf_fill_rect(win->framebuf, bw, bh, 0, 0, bw, disp_h, CLR_BLACK);
                }
                render_player_toolbar(win);
                wm_invalidate(win);
                needs_redraw = false;
            }
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
            continue;
        }

        /* Video or Audio mode: timing pacing according to media FPS */
        uint32_t target_interval = 0;
        if (current_mode == MEDIA_MODE_VIDEO) {
            if (vid_frame_delays && cur_frame_no < vid_total_frames) {
                target_interval = vid_frame_delays[cur_frame_no];
            } else if (vid_fps > 0) {
                target_interval = 1000 / vid_fps;
            } else {
                target_interval = 0; /* Uncapped/unlimited FPS as requested */
            }
        } else if (current_mode == MEDIA_MODE_AUDIO) {
            target_interval = 40; /* 25 FPS UI animation */
        }

        if (target_interval > 0 && elapsed < target_interval && !needs_redraw) {
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
            continue;
        }
        last_tick = now;

        if (current_mode == MEDIA_MODE_VIDEO) {
            if (!is_paused) {
                if (current_audio_stream && vid_total_frames > 0) {
                    double p = audio_stream_get_progress(current_audio_stream);
                    cur_frame_no = (int)(p * (double)vid_total_frames);
                    if (cur_frame_no >= vid_total_frames) cur_frame_no = vid_total_frames - 1;
                    if (cur_frame_no < 0) cur_frame_no = 0;
                } else {
                    cur_frame_no++;
                    if (vid_total_frames > 0 && cur_frame_no >= vid_total_frames) {
                        cur_frame_no = 0; /* Loop video */
                    }
                }
            }

            if (current_avi && current_avi->frame_ptrs && cur_frame_no < current_avi->total_frames) {
                blit_avi_frame_to_window(win, current_avi->frame_ptrs[cur_frame_no],
                                         current_avi->width, current_avi->height,
                                         current_avi->bpp, current_avi->is_bottom_up);
            } else if (current_vid_frames && vid_total_frames > 0) {
                const uint8_t *frame_src = current_vid_frames + (size_t)cur_frame_no * vid_w * vid_h * 4;
                blit_rgba_to_window(win, frame_src, vid_w, vid_h);
            } else {
                render_procedural_frame(cur_frame_no);
                blit_rgba_to_window(win, proc_frame, PROCEDURAL_W, PROCEDURAL_H);
            }
        } else if (current_mode == MEDIA_MODE_AUDIO) {
            int bw = win->w;
            int bh = win->h;
            int disp_h = (bh > 28) ? (bh - 28) : bh;

            /* Clear audio display canvas to black */
            wm_buf_fill_rect(win->framebuf, bw, bh, 0, 0, bw, disp_h, CLR_BLACK);

            /* Audio title & track metadata */
            wm_buf_draw_string(win->framebuf, bw, bh, 10, 8, "Now Playing Audio:", CLR_LIGHT_GRAY);
            wm_buf_draw_string(win->framebuf, bw, bh, 10, 20, current_title, CLR_YELLOW);

            /* Animated Frequency Equalizer Bars */
            int bar_w = 7;
            int spacing = 2;
            int start_x = 12;
            int max_w = bw - 24;
            int num_bars = (max_w > 0) ? (max_w / (bar_w + spacing)) : 10;
            if (num_bars > 64) num_bars = 64;
            if (num_bars < 4) num_bars = 4;
            int max_h = (disp_h > 45) ? (disp_h - 40) : 10;
            int base_y = disp_h - 6;
            float t = (float)cur_frame_no * 0.15f;
            if (!is_paused) cur_frame_no++;

            for (int b = 0; b < num_bars; b++) {
                float phase = (float)b * 0.4f;
                float s = (gl_sinf(t + phase) * 0.5f + 0.5f);
                float s2 = (gl_cosf(t * 0.7f + phase * 1.3f) * 0.5f + 0.5f);
                float h_val = is_paused ? 0.15f : (0.10f + 0.90f * ((s + s2) * 0.5f));
                int bh_val = (int)(h_val * (float)max_h);
                if (bh_val < 3) bh_val = 3;
                if (bh_val > max_h) bh_val = max_h;

                int bx = start_x + b * (bar_w + spacing);
                int by = base_y - bh_val;

                uint8_t c = (bh_val > (max_h * 3 / 4)) ? CLR_LIGHT_RED : ((bh_val > (max_h / 2)) ? CLR_YELLOW : CLR_LIGHT_GREEN);
                wm_buf_fill_rect(win->framebuf, bw, bh, bx, by, bar_w, bh_val, c);
                wm_buf_fill_rect(win->framebuf, bw, bh, bx, by, bar_w, 2, CLR_WHITE);
            }
        }

        render_player_toolbar(win);
        wm_invalidate(win);
        needs_redraw = false;

        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
    }

    /* Cleanup */
    unload_current_media();
    free_media_list();
    if (current_browse_dir) {
        kfree(current_browse_dir);
        current_browse_dir = NULL;
    }
    if (wm_is_window_valid(win)) {
        wm_destroy_window(win);
    }
    keyboard_set_app_input_mode(false);

    return 0;
}
