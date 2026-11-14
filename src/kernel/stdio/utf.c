#include <kernel/utf.h>
#include <memory/kmalloc.h>
#include <string.h>

#define UTF_REPLACEMENT_CHAR 0xFFFDULL

static const uint8_t s_bom_utf8_bytes[]    = { 0xEFu, 0xBBu, 0xBFu };
static const uint8_t s_bom_utf16le_bytes[] = { 0xFFu, 0xFEu };
static const uint8_t s_bom_utf16be_bytes[] = { 0xFEu, 0xFFu };
static const uint8_t s_bom_utf32le_bytes[] = { 0xFFu, 0xFEu, 0x00u, 0x00u };
static const uint8_t s_bom_utf32be_bytes[] = { 0x00u, 0x00u, 0xFEu, 0xFFu };
static const uint8_t s_bom_utf64le_bytes[] = { 0xFFu, 0xFEu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u };
static const uint8_t s_bom_utf64be_bytes[] = { 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xFEu, 0xFFu };

static utf_decode_status_t decode_utf8(const uint8_t *buf, size_t len,
                                       uint64_t *out_cp, size_t *out_consumed) {
    static const uint64_t s_utf8_min_cp[8] = {
        0ULL, 0ULL, 0x80ULL, 0x800ULL, 0x10000ULL, 0x200000ULL, 0x4000000ULL, 0x80000000ULL
    };

    if (!buf || len == 0) return UTF_DECODE_INCOMPLETE;
    uint8_t b0 = buf[0];

    if (b0 < 0x80u) {
        if (out_cp) *out_cp = (uint64_t)b0;
        if (out_consumed) *out_consumed = 1;
        return UTF_DECODE_OK;
    }

    if (b0 < 0xC2u) {
        if (out_consumed) *out_consumed = 1;
        return UTF_DECODE_INVALID;
    }

    if (b0 <= 0xFEu) {
        size_t total = 0;
        uint8_t mask = 0x80u;
        while ((b0 & mask) != 0u) {
            total++;
            mask >>= 1;
        }
        if (total < 2 || total > 7) {
            if (out_consumed) *out_consumed = 1;
            return UTF_DECODE_INVALID;
        }
        if (len < total) {
            return UTF_DECODE_INCOMPLETE;
        }
        uint8_t lead_mask = (uint8_t)((1u << (7u - total)) - 1u);
        uint64_t cp = (uint64_t)(b0 & lead_mask);
        for (size_t i = 1; i < total; i++) {
            if ((buf[i] & 0xC0u) != 0x80u) {
                if (out_consumed) *out_consumed = i;
                return UTF_DECODE_INVALID;
            }
            cp = (cp << 6) | (uint64_t)(buf[i] & 0x3Fu);
        }
        if (cp < s_utf8_min_cp[total] || (cp >= 0xD800ULL && cp <= 0xDFFFULL)) {
            if (out_consumed) *out_consumed = total;
            return UTF_DECODE_INVALID;
        }
        if (out_cp) *out_cp = cp;
        if (out_consumed) *out_consumed = total;
        return UTF_DECODE_OK;
    }

    /* Extended >36-bit UTF-8 frame: 0xFF, length continuations (10xxxxxx), payload. */
    size_t idx = 1;
    size_t payload_bytes = 0;
    while (1) {
        if (idx >= len) return UTF_DECODE_INCOMPLETE;
        uint8_t lb = buf[idx++];
        if ((lb & 0xC0u) != 0x80u) {
            if (out_consumed) *out_consumed = idx - 1;
            return UTF_DECODE_INVALID;
        }
        uint8_t part = (uint8_t)(lb & 0x3Fu);
        if ((size_t)part > SIZE_MAX - payload_bytes || payload_bytes + (size_t)part > 16u) {
            if (out_consumed) *out_consumed = idx;
            return UTF_DECODE_INVALID;
        }
        payload_bytes += (size_t)part;
        if (part < 0x3Fu) break;
    }
    if (payload_bytes == 0) payload_bytes = 1;
    if (payload_bytes > SIZE_MAX - idx) {
        if (out_consumed) *out_consumed = idx;
        return UTF_DECODE_INVALID;
    }
    size_t total_len = idx + payload_bytes;
    if (len < total_len) {
        return UTF_DECODE_INCOMPLETE;
    }
    uint64_t cp = 0;
    for (size_t i = 0; i < payload_bytes; i++) {
        uint8_t pb = buf[idx + i];
        if ((pb & 0xC0u) != 0x80u) {
            if (out_consumed) *out_consumed = idx + i;
            return UTF_DECODE_INVALID;
        }
        if (cp > (UINT64_MAX >> 6)) {
            if (out_consumed) *out_consumed = idx + i + 1u;
            return UTF_DECODE_INVALID;
        }
        cp = (cp << 6) | (uint64_t)(pb & 0x3Fu);
    }
    if (out_cp) *out_cp = cp;
    if (out_consumed) *out_consumed = total_len;
    return UTF_DECODE_OK;
}

static utf_decode_status_t decode_utf16_common(const uint8_t *buf, size_t len, bool be,
                                               uint64_t *out_cp, size_t *out_consumed) {
    if (!buf || len < 2) return UTF_DECODE_INCOMPLETE;
    uint16_t u1 = be ? (uint16_t)(((uint16_t)buf[0] << 8) | buf[1])
                     : (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));

    if (u1 >= 0xD800u && u1 <= 0xDBFFu) {
        if (len < 4) return UTF_DECODE_INCOMPLETE;
        uint16_t u2 = be ? (uint16_t)(((uint16_t)buf[2] << 8) | buf[3])
                         : (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
        if (u2 < 0xDC00u || u2 > 0xDFFFu) {
            if (out_consumed) *out_consumed = 2;
            return UTF_DECODE_INVALID;
        }
        uint64_t cp = 0x10000ULL + ((((uint64_t)(u1 - 0xD800u)) << 10) | (uint64_t)(u2 - 0xDC00u));
        if (out_cp) *out_cp = cp;
        if (out_consumed) *out_consumed = 4;
        return UTF_DECODE_OK;
    }
    if (u1 >= 0xDC00u && u1 <= 0xDFFFu) {
        if (out_consumed) *out_consumed = 2;
        return UTF_DECODE_INVALID;
    }
    if (out_cp) *out_cp = (uint64_t)u1;
    if (out_consumed) *out_consumed = 2;
    return UTF_DECODE_OK;
}

static utf_decode_status_t decode_utf16le(const uint8_t *buf, size_t len, uint64_t *cp, size_t *cons) {
    return decode_utf16_common(buf, len, false, cp, cons);
}

static utf_decode_status_t decode_utf16be(const uint8_t *buf, size_t len, uint64_t *cp, size_t *cons) {
    return decode_utf16_common(buf, len, true, cp, cons);
}

static utf_decode_status_t decode_utf32le(const uint8_t *buf, size_t len, uint64_t *cp, size_t *cons) {
    if (!buf || len < 4) return UTF_DECODE_INCOMPLETE;
    uint32_t v = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                 ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    if (cp) *cp = (uint64_t)v;
    if (cons) *cons = 4;
    return UTF_DECODE_OK;
}

static utf_decode_status_t decode_utf32be(const uint8_t *buf, size_t len, uint64_t *cp, size_t *cons) {
    if (!buf || len < 4) return UTF_DECODE_INCOMPLETE;
    uint32_t v = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                 ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
    if (cp) *cp = (uint64_t)v;
    if (cons) *cons = 4;
    return UTF_DECODE_OK;
}

static utf_decode_status_t decode_utf64le(const uint8_t *buf, size_t len, uint64_t *cp, size_t *cons) {
    if (!buf || len < 8) return UTF_DECODE_INCOMPLETE;
    uint64_t v = 0;
    for (size_t i = 0; i < 8; i++) {
        v |= ((uint64_t)buf[i]) << (i * 8u);
    }
    if (cp) *cp = v;
    if (cons) *cons = 8;
    return UTF_DECODE_OK;
}

static utf_decode_status_t decode_utf64be(const uint8_t *buf, size_t len, uint64_t *cp, size_t *cons) {
    if (!buf || len < 8) return UTF_DECODE_INCOMPLETE;
    uint64_t v = 0;
    for (size_t i = 0; i < 8; i++) {
        v = (v << 8) | (uint64_t)buf[i];
    }
    if (cp) *cp = v;
    if (cons) *cons = 8;
    return UTF_DECODE_OK;
}

/* Variable-width UTF: MSB=1 continues, MSB=0 ends (7 bits/byte). */
static utf_decode_status_t decode_utf_var(const uint8_t *buf, size_t len, uint64_t *cp, size_t *cons) {
    if (!buf || len == 0) return UTF_DECODE_INCOMPLETE;
    uint64_t val = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = buf[i];
        if (val > (UINT64_MAX >> 7)) {
            if (cons) *cons = i + 1u;
            return UTF_DECODE_INVALID;
        }
        val = (val << 7) | (uint64_t)(b & 0x7Fu);
        if ((b & 0x80u) == 0u) {
            if (cp) *cp = val;
            if (cons) *cons = i + 1;
            return UTF_DECODE_OK;
        }
    }
    return UTF_DECODE_INCOMPLETE;
}

static const utf_codec_t s_builtin_codecs[] = {
    { UTF_ENC_UTF8,    "UTF-8",    s_bom_utf8_bytes,    3, 1, decode_utf8    },
    { UTF_ENC_UTF16LE, "UTF-16LE", s_bom_utf16le_bytes, 2, 2, decode_utf16le },
    { UTF_ENC_UTF16BE, "UTF-16BE", s_bom_utf16be_bytes, 2, 2, decode_utf16be },
    { UTF_ENC_UTF32LE, "UTF-32LE", s_bom_utf32le_bytes, 4, 4, decode_utf32le },
    { UTF_ENC_UTF32BE, "UTF-32BE", s_bom_utf32be_bytes, 4, 4, decode_utf32be },
    { UTF_ENC_UTF64LE, "UTF-64LE", s_bom_utf64le_bytes, 8, 8, decode_utf64le },
    { UTF_ENC_UTF64BE, "UTF-64BE", s_bom_utf64be_bytes, 8, 8, decode_utf64be },
    { UTF_ENC_UTF_VAR, "UTF-VAR",  NULL,                0, 1, decode_utf_var }
};

#define BUILTIN_CODEC_COUNT (sizeof(s_builtin_codecs) / sizeof(s_builtin_codecs[0]))

static const uint32_t s_default_encoding_id = UTF_ENC_AUTO;

static const utf_codec_t *utf_find_codec(uint32_t encoding_id) {
    if (encoding_id == UTF_ENC_AUTO) {
        encoding_id = s_default_encoding_id;
    }
    for (size_t i = 0; i < BUILTIN_CODEC_COUNT; i++) {
        if (s_builtin_codecs[i].id == encoding_id) {
            return &s_builtin_codecs[i];
        }
    }
    return &s_builtin_codecs[0];
}

void utf_init(void) {
}

uint32_t utf_get_default_encoding(void) {
    return s_default_encoding_id;
}

static bool is_ascii_text_or_ws(uint8_t b) {
    return (b >= 0x20u && b <= 0x7Eu) ||
           b == '\n' || b == '\r' || b == '\t' || b == '\b' || b == 0x1Bu;
}

static bool is_non_ws_ctrl(uint8_t b) {
    return b > 0x00u && b < 0x20u &&
           b != '\n' && b != '\r' && b != '\t' && b != '\b' && b != 0x1Bu;
}

static void match_bom_prefix(const uint8_t *buf, size_t len,
                             const utf_codec_t **out_exact,
                             bool *out_has_longer) {
    const utf_codec_t *exact = NULL;
    bool has_longer = false;

    for (size_t i = 0; i < BUILTIN_CODEC_COUNT; i++) {
        const utf_codec_t *c = &s_builtin_codecs[i];
        if (!c->bom || c->bom_len == 0) continue;

        size_t cmp_len = (len < c->bom_len) ? len : c->bom_len;
        if (memcmp(buf, c->bom, cmp_len) != 0) continue;

        if (len < c->bom_len) {
            has_longer = true;
        } else if (!exact || c->bom_len > exact->bom_len) {
            exact = c;
        }
    }

    if (out_exact) *out_exact = exact;
    if (out_has_longer) *out_has_longer = has_longer;
}

static bool is_wide_codec_id(uint32_t enc_id) {
    return enc_id == UTF_ENC_UTF16LE || enc_id == UTF_ENC_UTF16BE ||
           enc_id == UTF_ENC_UTF32LE || enc_id == UTF_ENC_UTF32BE ||
           enc_id == UTF_ENC_UTF64LE || enc_id == UTF_ENC_UTF64BE;
}

/* Detect when a mixed stream using a BOM-selected wide encoding transitions
 * back to BOM-less ASCII / UTF-8 / UTF-VAR without emitting a new BOM. */
static bool auto_wide_codec_mismatch(uint32_t enc_id, const uint8_t *buf, size_t len) {
    if (!buf || len == 0) return false;

    if (enc_id == UTF_ENC_UTF64BE) {
        return buf[0] != 0x00u;
    }
    if (enc_id == UTF_ENC_UTF32BE) {
        if (buf[0] != 0x00u) return true;
        if (len >= 2u && buf[1] > 0x10u) return true;
        return false;
    }
    if (enc_id == UTF_ENC_UTF32LE || enc_id == UTF_ENC_UTF64LE) {
        if (len >= 2u && buf[0] >= 0x20u && buf[0] < 0x7Fu &&
            (buf[1] == '\n' || buf[1] == '\r' || buf[1] == '\t')) {
            return true;
        }
        if (len >= 3u) {
            if (buf[2] > 0x10u || (buf[2] > 0x03u && buf[2] < 0x0Eu)) {
                return true;
            }
        }
        size_t unit = (enc_id == UTF_ENC_UTF64LE) ? 8u : 4u;
        size_t chk = (len < unit) ? len : unit;
        for (size_t i = 3u; i < chk; i++) {
            if (buf[i] != 0x00u) return true;
        }
        return false;
    }
    if (enc_id == UTF_ENC_UTF16LE) {
        /* Guard BOM signatures or unpaired surrogates right after 1 ASCII byte */
        if (len >= 2u && buf[0] < 0x80u &&
            (buf[1] == 0xFFu || buf[1] == 0xFEu || buf[1] == 0xEFu)) {
            return true;
        }
    }
    return false;
}

static utf_decode_status_t decode_mixed_utf8_var(const uint8_t *buf, size_t len, bool eof,
                                                 uint64_t *out_cp, size_t *out_cons) {
    uint64_t cp = 0;
    size_t cons = 0;
    utf_decode_status_t st = decode_utf8(buf, len, &cp, &cons);
    if (st == UTF_DECODE_OK) {
        if (out_cp) *out_cp = cp;
        if (out_cons) *out_cons = cons;
        return UTF_DECODE_OK;
    }
    if (st == UTF_DECODE_INCOMPLETE && !eof) {
        return UTF_DECODE_INCOMPLETE;
    }

    uint64_t v_cp = 0;
    size_t v_cons = 0;
    utf_decode_status_t vst = decode_utf_var(buf, len, &v_cp, &v_cons);
    if (vst == UTF_DECODE_OK) {
        if (out_cp) *out_cp = v_cp;
        if (out_cons) *out_cons = v_cons ? v_cons : 1u;
        return UTF_DECODE_OK;
    }
    if (vst == UTF_DECODE_INCOMPLETE && !eof) {
        return UTF_DECODE_INCOMPLETE;
    }

    if (out_cp) *out_cp = UTF_REPLACEMENT_CHAR;
    if (out_cons) *out_cons = 1u;
    return UTF_DECODE_INVALID;
}

static uint64_t decode_bomless_pair_cp(uint8_t b0, uint8_t b1,
                                       bool third_is_nul, bool odd_nul_prefix) {
    bool is_le = false;
    if (b1 == 0x00u) {
        is_le = true;
    } else if (b1 == 0x04u && b0 >= 0x05u) {
        is_le = true;
    } else if (b0 == 0x04u && b1 >= 0x05u) {
        is_le = false;
    } else if (odd_nul_prefix && b1 != 0x00u) {
        is_le = true;
    } else if (b1 >= 0x01u && b1 <= 0x10u && third_is_nul) {
        is_le = true;
    } else {
        is_le = (b0 > b1);
    }
    return is_le ? (((uint64_t)b1 << 8) | (uint64_t)b0)
                 : (((uint64_t)b0 << 8) | (uint64_t)b1);
}

static utf_decode_status_t decode_auto_bomless_unit(const uint8_t *buf, size_t len, bool eof,
                                                    uint64_t *out_cp, size_t *out_cons,
                                                    bool *out_skip_only) {
    if (out_skip_only) *out_skip_only = false;
    if (!buf || len == 0) {
        if (out_cons) *out_cons = 0;
        return UTF_DECODE_INCOMPLETE;
    }

    if (buf[0] == 0x00u) {
        size_t z = 0;
        while (z < len && buf[z] == 0x00u) z++;

        if (z == len) {
            if (!eof && z < 8u) {
                return UTF_DECODE_INCOMPLETE;
            }
            if (out_skip_only) *out_skip_only = true;
            if (out_cp) *out_cp = 0;
            if (out_cons) *out_cons = (z >= 8u) ? 8u : z;
            return UTF_DECODE_OK;
        }

        /* Non-zero byte follows z NUL bytes: detect BOM-less BE vs LE NUL padding */
        const utf_codec_t *after_bom = NULL;
        bool after_longer = false;
        match_bom_prefix(buf + z, len - z, &after_bom, &after_longer);
        if (after_bom || after_longer) {
            if (out_skip_only) *out_skip_only = true;
            if (out_cp) *out_cp = 0;
            if (out_cons) *out_cons = z;
            return UTF_DECODE_OK;
        }

        uint8_t bz = buf[z];
        if (bz >= 0x80u) {
            if (z == 1u || z == 3u || z == 7u) {
                uint64_t u8_cp = 0;
                size_t u8_cons = 0;
                utf_decode_status_t u8_st = decode_utf8(buf + z, len - z, &u8_cp, &u8_cons);
                if (u8_st == UTF_DECODE_OK || (u8_st == UTF_DECODE_INCOMPLETE && !eof)) {
                    if (out_skip_only) *out_skip_only = true;
                    if (out_cp) *out_cp = 0;
                    if (out_cons) *out_cons = z;
                    return UTF_DECODE_OK;
                }
                if (bz < 0xC2u && (len == z + 1u || buf[z + 1u] == 0x00u)) {
                    if (out_cp) *out_cp = (uint64_t)bz;
                    if (out_cons) *out_cons = z + 1u;
                    return UTF_DECODE_OK;
                }
            }
            if (out_skip_only) *out_skip_only = true;
            if (out_cp) *out_cp = 0;
            if (out_cons) *out_cons = z;
            return UTF_DECODE_OK;
        }

        if (is_non_ws_ctrl(bz)) {
            if (z == 5u && bz <= 0x10u) {
                if (len < 8u) {
                    if (!eof) return UTF_DECODE_INCOMPLETE;
                } else {
                    if (out_cp) {
                        *out_cp = ((uint64_t)buf[5] << 16) |
                                  ((uint64_t)buf[6] << 8) |
                                  (uint64_t)buf[7];
                    }
                    if (out_cons) *out_cons = 8u;
                    return UTF_DECODE_OK;
                }
            }
            if (len < z + 2u) {
                if (!eof) return UTF_DECODE_INCOMPLETE;
                if (out_cp) *out_cp = (uint64_t)bz;
                if (out_cons) *out_cons = z + 1u;
                return UTF_DECODE_OK;
            }
            if (out_cp) {
                bool third_nul = (len >= z + 3u && buf[z + 2u] == 0x00u);
                bool odd_nuls = ((z & 1u) != 0u) && !(z == 1u && bz <= 0x10u && len >= 4u);
                *out_cp = decode_bomless_pair_cp(bz, buf[z + 1u], third_nul, odd_nuls);
            }
            if (out_cons) *out_cons = z + 2u;
            return UTF_DECODE_OK;
        }

        /* Printable ASCII or standard whitespace after NULs: check 1-byte lookahead for LE Cyrillic */
        if (bz == '\n' || bz == '\r' || bz == '\t' || bz == '\b' || bz == 0x1Bu) {
            if (out_cp) *out_cp = (uint64_t)bz;
            if (out_cons) *out_cons = z + 1u;
            return UTF_DECODE_OK;
        }
        if (len < z + 2u) {
            if (!eof) return UTF_DECODE_INCOMPLETE;
            if (out_cp) *out_cp = (uint64_t)bz;
            if (out_cons) *out_cons = z + 1u;
            return UTF_DECODE_OK;
        }
        if (is_non_ws_ctrl(buf[z + 1u])) {
            if (out_cp) *out_cp = ((uint64_t)buf[z + 1u] << 8) | (uint64_t)bz;
            if (out_cons) *out_cons = z + 2u;
            return UTF_DECODE_OK;
        }
        if (out_cp) *out_cp = (uint64_t)bz;
        if (out_cons) *out_cons = z + 1u;
        return UTF_DECODE_OK;
    }

    if (is_non_ws_ctrl(buf[0])) {
        if (len < 2u) {
            if (!eof) return UTF_DECODE_INCOMPLETE;
            if (out_cp) *out_cp = (uint64_t)buf[0];
            if (out_cons) *out_cons = 1u;
            return UTF_DECODE_OK;
        }
        if (out_cp) {
            bool third_nul = (len >= 3u && buf[2] == 0x00u);
            *out_cp = decode_bomless_pair_cp(buf[0], buf[1], third_nul, false);
        }
        if (out_cons) *out_cons = 2u;
        return UTF_DECODE_OK;
    }

    if (buf[0] < 0x80u) {
        if (len >= 8u && buf[2] <= 0x10u &&
            buf[3] == 0x00u && buf[4] == 0x00u && buf[5] == 0x00u &&
            buf[6] == 0x00u && buf[7] == 0x00u &&
            (buf[1] == 0x00u || is_non_ws_ctrl(buf[1]) || buf[2] > 0x00u || len == 8u)) {
            return decode_utf64le(buf, len, out_cp, out_cons);
        }
        if (len >= 4u && buf[2] <= 0x10u && buf[3] == 0x00u &&
            (buf[1] == 0x00u || is_non_ws_ctrl(buf[1]) || buf[2] > 0x00u || len == 4u)) {
            return decode_utf32le(buf, len, out_cp, out_cons);
        }
        if (len >= 2u && (buf[1] == 0x00u || (buf[1] > 0x00u && buf[1] < 0x08u))) {
            if (out_cp) *out_cp = ((uint64_t)buf[1] << 8) | (uint64_t)buf[0];
            if (out_cons) *out_cons = 2u;
            return UTF_DECODE_OK;
        }
        if (out_cp) *out_cp = (uint64_t)buf[0];
        if (out_cons) *out_cons = 1u;
        return UTF_DECODE_OK;
    }

    return decode_mixed_utf8_var(buf, len, eof, out_cp, out_cons);
}

void utf_stream_free(utf_stream_t *stream) {
    if (!stream) return;
    if (stream->buf) {
        kfree(stream->buf);
        stream->buf = NULL;
    }
    stream->len = 0;
    stream->cap = 0;
    stream->active_encoding_id = UTF_ENC_AUTO;
    stream->bom_checked = false;
}

static bool stream_append_byte(utf_stream_t *stream, uint8_t byte) {
    if (stream->len >= stream->cap) {
        size_t new_cap = (stream->cap == 0) ? 16 : (stream->cap * 2);
        uint8_t *new_buf = (uint8_t *)krealloc(stream->buf, new_cap);
        if (!new_buf) return false;
        stream->buf = new_buf;
        stream->cap = new_cap;
    }
    stream->buf[stream->len++] = byte;
    return true;
}

static void stream_consume_prefix(utf_stream_t *stream, size_t n) {
    if (n == 0) return;
    if (n >= stream->len) {
        stream->len = 0;
        return;
    }
    memmove(stream->buf, stream->buf + n, stream->len - n);
    stream->len -= n;
}

static bool stream_decode_step(utf_stream_t *stream, bool eof, uint64_t *out_codepoint) {
    if (!stream || stream->len == 0) return false;

    uint32_t cfg_enc = (stream->encoding_id != UTF_ENC_AUTO)
                           ? stream->encoding_id
                           : s_default_encoding_id;

    if (cfg_enc != UTF_ENC_AUTO) {
        const utf_codec_t *codec = utf_find_codec(cfg_enc);
        if (!codec || !codec->decode) codec = &s_builtin_codecs[0];

        const uint8_t *bom_ptr = codec->bom;
        if (!stream->bom_checked && bom_ptr && codec->bom_len > 0) {
            if (stream->len <= codec->bom_len &&
                memcmp(stream->buf, bom_ptr, stream->len) == 0) {
                if (stream->len == codec->bom_len) {
                    stream->bom_checked = true;
                    stream_consume_prefix(stream, codec->bom_len);
                    return false;
                }
                if (!eof) return false;
            }
            stream->bom_checked = true;
        }

        uint64_t cp = 0;
        size_t consumed = 0;
        utf_decode_status_t st = codec->decode(stream->buf, stream->len, &cp, &consumed);
        if (st == UTF_DECODE_INCOMPLETE) {
            if (!eof) return false;
            stream_consume_prefix(stream, 1u);
            if (out_codepoint) *out_codepoint = UTF_REPLACEMENT_CHAR;
            return true;
        }
        if (consumed == 0) consumed = 1u;
        stream_consume_prefix(stream, consumed);
        if (out_codepoint) *out_codepoint = (st == UTF_DECODE_OK) ? cp : UTF_REPLACEMENT_CHAR;
        return true;
    }

    /* Mixed / Automatic UTF mode */
    while (stream->len > 0) {
        const utf_codec_t *exact_bom = NULL;
        bool has_longer_bom = false;
        match_bom_prefix(stream->buf, stream->len, &exact_bom, &has_longer_bom);
        if (has_longer_bom && !eof) {
            return false;
        }
        if (exact_bom) {
            stream->active_encoding_id = exact_bom->id;
            stream->bom_checked = true;
            stream_consume_prefix(stream, exact_bom->bom_len);
            continue;
        }
        stream->bom_checked = true;

        uint32_t active = stream->active_encoding_id;
        if (active != UTF_ENC_AUTO &&
            is_wide_codec_id(active) &&
            auto_wide_codec_mismatch(active, stream->buf, stream->len)) {
            stream->active_encoding_id = UTF_ENC_AUTO;
            active = UTF_ENC_AUTO;
        }

        if (active != UTF_ENC_AUTO && active != UTF_ENC_UTF8) {
            const utf_codec_t *codec = utf_find_codec(active);
            if (codec && codec->decode) {
                uint64_t cp = 0;
                size_t consumed = 0;
                utf_decode_status_t st = codec->decode(stream->buf, stream->len, &cp, &consumed);
                if (st == UTF_DECODE_OK) {
                    if (consumed == 0) consumed = 1u;
                    stream_consume_prefix(stream, consumed);
                    if (out_codepoint) *out_codepoint = cp;
                    return true;
                }
                if (st == UTF_DECODE_INCOMPLETE && !eof) {
                    return false;
                }
            }
            /* Fallback out of locked active encoding on mismatch or EOF remainder */
            stream->active_encoding_id = UTF_ENC_AUTO;
        }

        uint64_t cp = 0;
        size_t consumed = 0;
        bool skip_only = false;
        utf_decode_status_t st = decode_auto_bomless_unit(
            stream->buf, stream->len, eof, &cp, &consumed, &skip_only);
        if (st == UTF_DECODE_INCOMPLETE && !eof) {
            return false;
        }
        if (consumed == 0) consumed = 1u;
        stream_consume_prefix(stream, consumed);
        if (skip_only) {
            continue;
        }
        if (out_codepoint) *out_codepoint = (st == UTF_DECODE_OK) ? cp : UTF_REPLACEMENT_CHAR;
        return true;
    }

    return false;
}

bool utf_stream_flush_next(utf_stream_t *stream, uint64_t *out_codepoint) {
    return stream_decode_step(stream, false, out_codepoint);
}

bool utf_stream_push_byte(utf_stream_t *stream, uint8_t byte, uint64_t *out_codepoint) {
    if (!stream) return false;

    uint32_t cfg_enc = (stream->encoding_id != UTF_ENC_AUTO)
                           ? stream->encoding_id
                           : s_default_encoding_id;
    uint32_t eff_enc = (cfg_enc != UTF_ENC_AUTO) ? cfg_enc : stream->active_encoding_id;

    /* Fast path: plain 7-bit ASCII text/whitespace when idle in UTF-8/UTF-VAR/AUTO
     * (never touches kmalloc, safe during earliest pre-heap boot prints). */
    if (stream->len == 0 && is_ascii_text_or_ws(byte) &&
        (eff_enc == UTF_ENC_AUTO || eff_enc == UTF_ENC_UTF8 || eff_enc == UTF_ENC_UTF_VAR)) {
        if (cfg_enc == UTF_ENC_AUTO) {
            stream->active_encoding_id = UTF_ENC_AUTO;
        }
        stream->bom_checked = true;
        if (out_codepoint) *out_codepoint = (uint64_t)byte;
        return true;
    }

    if (!stream_append_byte(stream, byte)) {
        stream->len = 0;
        if (out_codepoint) *out_codepoint = (uint64_t)byte;
        return true;
    }

    return stream_decode_step(stream, false, out_codepoint);
}

utf_decode_status_t utf_decode_next(uint32_t encoding_id,
                                    const uint8_t *buf, size_t len,
                                    uint64_t *out_codepoint,
                                    size_t *out_consumed) {
    if (!buf || len == 0) {
        if (out_consumed) *out_consumed = 0;
        return UTF_DECODE_INCOMPLETE;
    }

    uint32_t eff_enc = (encoding_id != UTF_ENC_AUTO) ? encoding_id : s_default_encoding_id;
    if (eff_enc != UTF_ENC_AUTO) {
        const utf_codec_t *codec = utf_find_codec(eff_enc);
        if (!codec || !codec->decode) codec = &s_builtin_codecs[0];
        return codec->decode(buf, len, out_codepoint, out_consumed);
    }

    size_t pos = 0;
    const utf_codec_t *bom_codec = NULL;
    while (pos < len) {
        const utf_codec_t *exact_bom = NULL;
        bool has_longer = false;
        match_bom_prefix(buf + pos, len - pos, &exact_bom, &has_longer);
        if (!exact_bom) break;
        bom_codec = exact_bom;
        pos += exact_bom->bom_len;
    }

    if (bom_codec) {
        if (pos >= len) {
            if (out_codepoint) *out_codepoint = UTF_BOM_CHAR;
            if (out_consumed) *out_consumed = pos;
            return UTF_DECODE_OK;
        }
        uint64_t cp = 0;
        size_t sub_cons = 0;
        utf_decode_status_t st = bom_codec->decode
            ? bom_codec->decode(buf + pos, len - pos, &cp, &sub_cons)
            : UTF_DECODE_INVALID;
        if (st == UTF_DECODE_OK) {
            if (out_codepoint) *out_codepoint = cp;
            if (out_consumed) *out_consumed = pos + (sub_cons ? sub_cons : 1u);
            return UTF_DECODE_OK;
        }
    }

    size_t local_pos = pos;
    while (local_pos < len) {
        uint64_t cp = 0;
        size_t cons = 0;
        bool skip_only = false;
        utf_decode_status_t st = decode_auto_bomless_unit(
            buf + local_pos, len - local_pos, true, &cp, &cons, &skip_only);
        if (cons == 0) cons = 1u;
        local_pos += cons;
        if (skip_only) {
            if (local_pos >= len) {
                if (out_codepoint) *out_codepoint = 0;
                if (out_consumed) *out_consumed = local_pos;
                return UTF_DECODE_OK;
            }
            const utf_codec_t *next_bom = NULL;
            bool next_longer = false;
            match_bom_prefix(buf + local_pos, len - local_pos, &next_bom, &next_longer);
            if (next_bom) {
                uint64_t rem_cp = 0;
                size_t rem_cons = 0;
                utf_decode_status_t rem_st = utf_decode_next(
                    UTF_ENC_AUTO, buf + local_pos, len - local_pos, &rem_cp, &rem_cons);
                if (out_codepoint) *out_codepoint = rem_cp;
                if (out_consumed) *out_consumed = local_pos + rem_cons;
                return rem_st;
            }
            continue;
        }
        if (out_codepoint) *out_codepoint = cp;
        if (out_consumed) *out_consumed = local_pos;
        return st;
    }

    if (out_consumed) *out_consumed = pos ? pos : 1u;
    return UTF_DECODE_INVALID;
}
