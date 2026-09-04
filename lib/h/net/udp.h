#ifndef IPO_NET_UDP_H
#define IPO_NET_UDP_H

#include <net/net.h>

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_header_t;

typedef void (*udp_callback_t)(ip4_addr_t src_ip, uint16_t src_port, const void *data, uint16_t len);

void udp_init(void);
int  udp_send(ip4_addr_t dst_ip, uint16_t src_port, uint16_t dst_port, const void *payload, uint16_t len);
void udp_receive(ip4_addr_t src_ip, const void *data, uint16_t len);
bool udp_bind(uint16_t port, udp_callback_t callback);
void udp_unbind(uint16_t port);

#endif /* IPO_NET_UDP_H */
