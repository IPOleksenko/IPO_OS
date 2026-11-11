#ifndef IPO_NET_NET_H
#define IPO_NET_NET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint32_t ip4_addr_t;

#define IP4_ADDR(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* Special IPv4 addresses */
#define IP4_ADDR_ANY        ((ip4_addr_t)0)
#define IP4_ADDR_BROADCAST  ((ip4_addr_t)0xFFFFFFFFu)

/* Dynamic network/interface-derived IPv4 address query functions */
ip4_addr_t net_get_any_ip(void);
ip4_addr_t net_get_loopback_ip(void);
void       net_set_loopback_ip(ip4_addr_t ip);
ip4_addr_t net_get_broadcast_ip(const void *iface_ptr);

#define IP4_ADDR_LOOPBACK   (net_get_loopback_ip())

/* Dynamic unbounded MAC address structure:
 * Supports arbitrary hardware address lengths (standard 6-byte Ethernet,
 * 8-byte EUI-64, 20-byte InfiniBand, or arbitrary 999+ byte custom/cryptographic identifiers).
 */
typedef struct {
    uint8_t *bytes;       /* Dynamically allocated buffer */
    size_t   len;         /* Actual length of MAC in bytes */
    size_t   capacity;    /* Allocated capacity of bytes buffer */
} mac_addr_t;

/* Dynamic MAC address lifecycle & manipulation */
void mac_init(mac_addr_t *mac);
void mac_free(mac_addr_t *mac);
bool mac_set(mac_addr_t *mac, const void *data, size_t len);
bool mac_copy(mac_addr_t *dst, const mac_addr_t *src);
bool mac_equal(const mac_addr_t *a, const mac_addr_t *b);
const uint8_t *mac_get_bytes(const mac_addr_t *mac);
size_t mac_get_len(const mac_addr_t *mac);
uint8_t mac_get_byte(const mac_addr_t *mac, size_t index);
void mac_set_broadcast(mac_addr_t *mac);
bool mac_is_broadcast(const mac_addr_t *mac);
bool mac_set_hex(mac_addr_t *mac, const char *hex_str);
int  mac_format(const mac_addr_t *mac, char *out_buf, size_t out_cap);
char *mac_format_dyn(const mac_addr_t *mac);

typedef struct {
    mac_addr_t mac;
    ip4_addr_t ip;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    ip4_addr_t dns;
    bool        link_up;
} net_if_t;

bool net_set_mac(net_if_t *iface, const void *data, size_t len);

/* Byte order helpers */
#ifndef _BYTEORDER_HELPERS_DEFINED
#define _BYTEORDER_HELPERS_DEFINED
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
#endif

/* Checksum utility (RFC 1071) */
uint16_t net_checksum(const void *data, size_t len);

/* String conversion */
void ip_to_str(ip4_addr_t ip, char *buf, size_t size);
char *ip_to_str_dyn(ip4_addr_t ip);
bool str_to_ip(const char *str, ip4_addr_t *out_ip);

/* Net initialization and polling */
bool net_init(void);
void net_poll(void);
net_if_t *net_get_interface(void);
void net_set_ip(ip4_addr_t ip, ip4_addr_t netmask, ip4_addr_t gateway, ip4_addr_t dns);

#endif /* IPO_NET_NET_H */
