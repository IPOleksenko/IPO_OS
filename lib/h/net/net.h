#ifndef IPO_NET_NET_H
#define IPO_NET_NET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint32_t ip4_addr_t;

#define IP4_ADDR(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

#define IP4_ADDR_LOOPBACK   IP4_ADDR(127, 0, 0, 1)
#define IP4_ADDR_ANY        IP4_ADDR(0, 0, 0, 0)
#define IP4_ADDR_BROADCAST  IP4_ADDR(255, 255, 255, 255)

typedef struct {
    uint8_t mac[6];
} __attribute__((packed)) mac_addr_t;

typedef struct {
    mac_addr_t mac;
    ip4_addr_t ip;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    ip4_addr_t dns;
    bool        link_up;
} net_if_t;

/* Byte order helpers */
static inline uint16_t htons(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}
static inline uint16_t ntohs(uint16_t v) {
    return htons(v);
}
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8)  |
           ((v & 0x00FF0000u) >> 8)  |
           ((v & 0xFF000000u) >> 24);
}
static inline uint32_t ntohl(uint32_t v) {
    return htonl(v);
}

/* Checksum utility (RFC 1071) */
uint16_t net_checksum(const void *data, size_t len);

/* String conversion */
void ip_to_str(ip4_addr_t ip, char *buf, size_t size);
bool str_to_ip(const char *str, ip4_addr_t *out_ip);

/* Net initialization and polling */
bool net_init(void);
void net_poll(void);
net_if_t *net_get_interface(void);

#endif /* IPO_NET_NET_H */
