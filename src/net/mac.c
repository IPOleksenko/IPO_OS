#include <net/net.h>
#include <memory/kmalloc.h>
#include <string.h>

void mac_init(mac_addr_t *mac) {
    if (!mac) return;
    mac->bytes = NULL;
    mac->len = 0;
    mac->capacity = 0;
}

void mac_free(mac_addr_t *mac) {
    if (!mac) return;
    if (mac->bytes) {
        kfree(mac->bytes);
        mac->bytes = NULL;
    }
    mac->len = 0;
    mac->capacity = 0;
}

const uint8_t *mac_get_bytes(const mac_addr_t *mac) {
    if (!mac) return NULL;
    return mac->bytes;
}

size_t mac_get_len(const mac_addr_t *mac) {
    if (!mac) return 0;
    return mac->len;
}

uint8_t mac_get_byte(const mac_addr_t *mac, size_t index) {
    if (!mac || !mac->bytes || index >= mac->len) return 0;
    return mac->bytes[index];
}

bool mac_set(mac_addr_t *mac, const void *data, size_t len) {
    if (!mac || (!data && len > 0)) return false;

    if (len == 0) {
        mac_free(mac);
        return true;
    }

    if (len > mac->capacity || !mac->bytes) {
        uint8_t *new_buf = (uint8_t *)krealloc(mac->bytes, len);
        if (!new_buf) {
            return false;
        }
        mac->bytes = new_buf;
        mac->capacity = len;
    }

    memcpy(mac->bytes, data, len);
    mac->len = len;
    return true;
}

bool mac_copy(mac_addr_t *dst, const mac_addr_t *src) {
    if (!dst || !src) return false;
    return mac_set(dst, src->bytes, src->len);
}

bool mac_equal(const mac_addr_t *a, const mac_addr_t *b) {
    if (!a || !b) return false;
    if (a->len != b->len) return false;
    if (a->len == 0) return true;
    if (!a->bytes || !b->bytes) return false;
    return memcmp(a->bytes, b->bytes, a->len) == 0;
}

void mac_set_broadcast(mac_addr_t *mac) {
    const uint8_t *bcast = (const uint8_t *)"\xFF\xFF\xFF\xFF\xFF\xFF";
    mac_set(mac, bcast, 6);
}

bool mac_is_broadcast(const mac_addr_t *mac) {
    if (!mac || !mac->bytes || mac->len == 0) return false;
    for (size_t i = 0; i < mac->len; i++) {
        if (mac->bytes[i] != 0xFF) return false;
    }
    return true;
}

static inline int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool mac_set_hex(mac_addr_t *mac, const char *hex_str) {
    if (!mac || !hex_str) return false;

    /* Count bytes */
    size_t count = 0;
    const char *p = hex_str;
    while (*p) {
        while (*p == ':' || *p == '-' || *p == ' ') p++;
        if (!*p) break;
        int h1 = hex_val(*p++);
        if (h1 < 0) return false;
        if (!*p) return false;
        int h2 = hex_val(*p++);
        if (h2 < 0) return false;
        count++;
    }

    if (count == 0) return false;

    uint8_t *tmp = (uint8_t *)kmalloc(count);
    if (!tmp) return false;

    size_t idx = 0;
    p = hex_str;
    while (*p && idx < count) {
        while (*p == ':' || *p == '-' || *p == ' ') p++;
        if (!*p) break;
        int h1 = hex_val(*p++);
        int h2 = hex_val(*p++);
        tmp[idx++] = (uint8_t)((h1 << 4) | h2);
    }

    bool res = mac_set(mac, tmp, count);
    kfree(tmp);
    return res;
}

int mac_format(const mac_addr_t *mac, char *out_buf, size_t out_cap) {
    if (!mac || !out_buf || out_cap == 0) return -1;
    size_t len = mac_get_len(mac);
    const uint8_t *bytes = mac_get_bytes(mac);
    if (!bytes || len == 0) {
        out_buf[0] = '\0';
        return 0;
    }

    const char *hex = "0123456789abcdef";
    size_t written = 0;
    for (size_t i = 0; i < len; i++) {
        size_t need = (i + 1 < len) ? 3u : 2u;
        if (written + need >= out_cap) {
            break;
        }
        out_buf[written++] = hex[(bytes[i] >> 4) & 0x0F];
        out_buf[written++] = hex[bytes[i] & 0x0F];
        if (i + 1 < len) {
            out_buf[written++] = ':';
        }
    }
    out_buf[written] = '\0';
    return (int)written;
}

char *mac_format_dyn(const mac_addr_t *mac) {
    if (!mac) return NULL;
    size_t len = mac_get_len(mac);
    if (len == 0) {
        char *empty = (char *)kmalloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    size_t req_cap = (len * 3) + 1;
    char *buf = (char *)kmalloc(req_cap);
    if (!buf) return NULL;

    mac_format(mac, buf, req_cap);
    return buf;
}
