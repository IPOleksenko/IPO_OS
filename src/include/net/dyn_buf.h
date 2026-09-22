#ifndef IPO_NET_DYN_BUF_H
#define IPO_NET_DYN_BUF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * Universal dynamically-resizable byte buffer.
 * Automatically doubles/expands capacity as needed, with no arbitrary limits.
 */
typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   capacity;
} dyn_buf_t;

void   dyn_buf_init(dyn_buf_t *buf, size_t initial_cap);
bool   dyn_buf_reserve(dyn_buf_t *buf, size_t needed_cap);
bool   dyn_buf_append(dyn_buf_t *buf, const void *src, size_t len);
bool   dyn_buf_append_byte(dyn_buf_t *buf, uint8_t byte);
bool   dyn_buf_append_str(dyn_buf_t *buf, const char *str);
size_t dyn_buf_consume(dyn_buf_t *buf, void *dest, size_t len);
void   dyn_buf_reset(dyn_buf_t *buf);
void   dyn_buf_free(dyn_buf_t *buf);

/**
 * Universal dynamic string supporting arbitrary length (e.g. 100+ characters)
 * for IP addresses, domain names, URLs and custom protocol parameters.
 */
typedef struct {
    char   *cstr;
    size_t  len;
    size_t  capacity;
} dyn_str_t;

void   dyn_str_init(dyn_str_t *ds, const char *initial);
bool   dyn_str_set(dyn_str_t *ds, const char *val);
bool   dyn_str_append(dyn_str_t *ds, const char *suffix);
bool   dyn_str_append_char(dyn_str_t *ds, char c);
const char *dyn_str_c(const dyn_str_t *ds);
void   dyn_str_free(dyn_str_t *ds);

#endif /* IPO_NET_DYN_BUF_H */

