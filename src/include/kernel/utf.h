#ifndef KERNEL_UTF_H
#define KERNEL_UTF_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define UTF_BOM_CHAR 0xFEFFULL

typedef enum {
    UTF_ENC_AUTO    = 0,
    UTF_ENC_UTF8    = 1,
    UTF_ENC_UTF16LE = 2,
    UTF_ENC_UTF16BE = 3,
    UTF_ENC_UTF32LE = 4,
    UTF_ENC_UTF32BE = 5,
    UTF_ENC_UTF64LE = 6,
    UTF_ENC_UTF64BE = 7,
    UTF_ENC_UTF_VAR = 8
} utf_encoding_id_t;

typedef enum {
    UTF_DECODE_OK         = 0,
    UTF_DECODE_INCOMPLETE = 1,
    UTF_DECODE_INVALID    = 2
} utf_decode_status_t;

typedef utf_decode_status_t (*utf_decode_fn_t)(const uint8_t *buf, size_t len,
                                               uint64_t *out_codepoint,
                                               size_t *out_consumed);

typedef struct utf_codec {
    uint32_t        id;
    const char     *name;
    const uint8_t  *bom;
    size_t          bom_len;
    size_t          unit_bytes;
    utf_decode_fn_t decode;
} utf_codec_t;

/* Growable byte buffer for streaming multi-byte UTF sequences. */
typedef struct utf_stream {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    uint32_t encoding_id;
    uint32_t active_encoding_id;
    bool     bom_checked;
} utf_stream_t;

void utf_init(void);
uint32_t utf_get_default_encoding(void);

void utf_stream_free(utf_stream_t *stream);

/* Feed one byte into stream; returns true once a full codepoint is ready (or U+FFFD on invalid input). */
bool utf_stream_push_byte(utf_stream_t *stream, uint8_t byte, uint64_t *out_codepoint);

/* Drain any buffered codepoint remaining after an AUTO BOM mismatch. */
bool utf_stream_flush_next(utf_stream_t *stream, uint64_t *out_codepoint);

utf_decode_status_t utf_decode_next(uint32_t encoding_id,
                                    const uint8_t *buf, size_t len,
                                    uint64_t *out_codepoint,
                                    size_t *out_consumed);

/* Implemented in vga_cyrillic_font.c. */
uint8_t utf_codepoint_to_vga_glyph(uint64_t codepoint);

#endif /* KERNEL_UTF_H */
