#ifndef IPO_NET_ARP_H
#define IPO_NET_ARP_H

#include <net/net.h>

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

typedef struct {
    uint16_t   hw_type;
    uint16_t   proto_type;
    uint8_t    hw_len;
    uint8_t    proto_len;
    uint16_t   opcode;
    mac_addr_t sender_mac;
    uint32_t   sender_ip;
    mac_addr_t target_mac;
    uint32_t   target_ip;
} __attribute__((packed)) arp_packet_t;

void arp_init(void);
void arp_receive(const void *data, uint16_t len);
bool arp_resolve(ip4_addr_t target_ip, mac_addr_t *out_mac, uint32_t timeout_ms);
void arp_send_request(ip4_addr_t target_ip);

#endif /* IPO_NET_ARP_H */
