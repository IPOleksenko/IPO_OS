#include <net/dyn_buf.h>
#include <memory/kmalloc.h>
#include <string.h>

#define DYN_BUF_DEFAULT_CAP 64

void dyn_buf_init(dyn_buf_t *buf, size_t initial_cap) {
    if (!buf) return;
    buf->size = 0;
    buf->capacity = initial_cap > 0 ? initial_cap : DYN_BUF_DEFAULT_CAP;
    buf->data = (uint8_t *)kmalloc(buf->capacity);
    if (buf->data) {
        memset(buf->data, 0, buf->capacity);
    } else {
        buf->capacity = 0;
    }
}

bool dyn_buf_reserve(dyn_buf_t *buf, size_t needed_cap) {
    if (!buf) return false;
    if (buf->capacity >= needed_cap) return true;

    size_t new_cap = buf->capacity ? buf->capacity : DYN_BUF_DEFAULT_CAP;
    while (new_cap < needed_cap) {
        /* Geometric growth: double size to guarantee amortized O(1) appending */
        size_t next = new_cap * 2;
        if (next < new_cap) {
            new_cap = needed_cap; /* overflow guard */
            break;
        }
        new_cap = next;
    }

    uint8_t *new_data = (uint8_t *)krealloc(buf->data, new_cap);
    if (!new_data) {
        return false;
    }

    /* Zero-fill freshly allocated margin */
    if (new_cap > buf->capacity) {
        memset(new_data + buf->capacity, 0, new_cap - buf->capacity);
    }

    buf->data = new_data;
    buf->capacity = new_cap;
    return true;
}

bool dyn_buf_append(dyn_buf_t *buf, const void *src, size_t len) {
    if (!buf || (!src && len > 0)) return false;
    if (len == 0) return true;
    if (len > SIZE_MAX - buf->size) return false;
    if (!dyn_buf_reserve(buf, buf->size + len)) {
        return false;
    }
    memcpy(buf->data + buf->size, src, len);
    buf->size += len;
    return true;
}

bool dyn_buf_append_byte(dyn_buf_t *buf, uint8_t byte) {
    return dyn_buf_append(buf, &byte, 1);
}

bool dyn_buf_append_str(dyn_buf_t *buf, const char *str) {
    if (!str) return true;
    return dyn_buf_append(buf, str, strlen(str));
}

size_t dyn_buf_consume(dyn_buf_t *buf, void *dest, size_t len) {
    if (!buf || !buf->data || buf->size == 0 || len == 0) return 0;
    size_t to_read = len < buf->size ? len : buf->size;

    if (dest) {
        memcpy(dest, buf->data, to_read);
    }

    size_t remaining = buf->size - to_read;
    if (remaining > 0) {
        memmove(buf->data, buf->data + to_read, remaining);
    }
    buf->size = remaining;
    return to_read;
}

void dyn_buf_reset(dyn_buf_t *buf) {
    if (!buf) return;
    buf->size = 0;
}

void dyn_buf_free(dyn_buf_t *buf) {
    if (!buf) return;
    if (buf->data) {
        kfree(buf->data);
        buf->data = NULL;
    }
    buf->size = 0;
    buf->capacity = 0;
}

/* Dynamic strings */

static bool dyn_str_grow(dyn_str_t *ds, size_t needed) {
    if (needed <= ds->capacity && ds->cstr) {
        return true;
    }
    size_t new_cap = ds->capacity ? ds->capacity : 32;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }
    char *new_cstr = (char *)krealloc(ds->cstr, new_cap);
    if (!new_cstr) return false;
    ds->cstr = new_cstr;
    ds->capacity = new_cap;
    return true;
}

void dyn_str_init(dyn_str_t *ds, const char *initial) {
    if (!ds) return;
    ds->len = 0;
    ds->capacity = 0;
    ds->cstr = NULL;

    size_t slen = initial ? strlen(initial) : 0;
    if (slen > SIZE_MAX - 16) return;
    size_t cap = slen + 16;
    char *buf = (char *)kmalloc(cap);
    if (!buf) return;

    if (initial && slen > 0) {
        memcpy(buf, initial, slen);
    }
    buf[slen] = '\0';
    ds->cstr = buf;
    ds->len = slen;
    ds->capacity = cap;
}

bool dyn_str_set(dyn_str_t *ds, const char *val) {
    if (!ds) return false;
    if (!val) {
        if (ds->cstr) ds->cstr[0] = '\0';
        ds->len = 0;
        return true;
    }
    size_t slen = strlen(val);
    if (slen >= SIZE_MAX - 16) return false;
    if (!dyn_str_grow(ds, slen + 1)) return false;
    memcpy(ds->cstr, val, slen);
    ds->cstr[slen] = '\0';
    ds->len = slen;
    return true;
}

bool dyn_str_append(dyn_str_t *ds, const char *suffix) {
    if (!ds) return false;
    if (!suffix) return true;
    size_t slen = strlen(suffix);
    if (slen == 0) return true;
    if (slen >= SIZE_MAX - ds->len - 1) return false;
    if (!dyn_str_grow(ds, ds->len + slen + 1)) return false;

    memcpy(ds->cstr + ds->len, suffix, slen);
    ds->len += slen;
    ds->cstr[ds->len] = '\0';
    return true;
}

bool dyn_str_append_char(dyn_str_t *ds, char c) {
    if (!ds) return false;
    if (ds->len > SIZE_MAX - 2) return false;
    if (!dyn_str_grow(ds, ds->len + 2)) return false;
    ds->cstr[ds->len++] = c;
    ds->cstr[ds->len] = '\0';
    return true;
}

const char *dyn_str_c(const dyn_str_t *ds) {
    return (ds && ds->cstr) ? ds->cstr : "";
}

void dyn_str_free(dyn_str_t *ds) {
    if (!ds) return;
    if (ds->cstr) {
        kfree(ds->cstr);
        ds->cstr = NULL;
    }
    ds->len = 0;
    ds->capacity = 0;
}

