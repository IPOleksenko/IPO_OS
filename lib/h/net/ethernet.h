#ifndef IPO_NET_ETHERNET_H
#define IPO_NET_ETHERNET_H

#include <net/net.h>

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

typedef struct {
    mac_addr_t dest;
    mac_addr_t src;
    uint16_t   type;
} __attribute__((packed)) eth_header_t;

int  eth_send(const mac_addr_t *dest, uint16_t ethertype, const void *payload, uint16_t len);
void eth_receive(const void *frame, uint16_t len);

#endif /* IPO_NET_ETHERNET_H */
