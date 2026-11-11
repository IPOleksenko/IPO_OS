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
    ip4_addr_t sender_ip;
    mac_addr_t target_mac;
    ip4_addr_t target_ip;
} arp_packet_t;

typedef arp_packet_t arp_dyn_packet_t;

void arp_packet_init(arp_packet_t *pkt);
void arp_packet_free(arp_packet_t *pkt);
bool arp_packet_parse(arp_packet_t *pkt, const void *data, size_t len);
size_t arp_packet_serialize(const arp_packet_t *pkt, void *buf, size_t buf_size);
size_t arp_packet_calc_size(const arp_packet_t *pkt);
size_t arp_packet_serialize_dynamic(const arp_packet_t *pkt, uint8_t **out_raw);

#define arp_dyn_init(d)    arp_packet_init(d)
#define arp_dyn_free(d)    arp_packet_free(d)
#define arp_dyn_parse(d, data, len) arp_packet_parse(d, data, len)

void arp_init(void);
void arp_receive(const void *data, uint16_t len);
bool arp_resolve(ip4_addr_t target_ip, mac_addr_t *out_mac, uint32_t timeout_ms);
bool arp_resolve_ex(ip4_addr_t target_ip, mac_addr_t *out_mac, uint32_t timeout_ms,
                    uint32_t max_retries, uint32_t retry_interval_ms);
void arp_send_request(ip4_addr_t target_ip);
void arp_send_gratuitous(void);

#endif /* IPO_NET_ARP_H */
