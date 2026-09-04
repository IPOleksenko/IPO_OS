#ifndef IPO_NET_IPV4_H
#define IPO_NET_IPV4_H

#include <net/net.h>

#define IPPROTO_ICMP 1
#define IPPROTO_UDP  17

typedef struct {
    uint8_t    ihl_version; // version: 4, ihl: 4
    uint8_t    tos;
    uint16_t   total_len;
    uint16_t   id;
    uint16_t   flags_frag;
    uint8_t    ttl;
    uint8_t    protocol;
    uint16_t   checksum;
    uint32_t   src_ip;
    uint32_t   dst_ip;
} __attribute__((packed)) ip4_header_t;

int  ip4_send(ip4_addr_t dst_ip, uint8_t protocol, uint8_t ttl, const void *payload, uint16_t len);
void ip4_receive(const void *packet, uint16_t len);

#endif /* IPO_NET_IPV4_H */
