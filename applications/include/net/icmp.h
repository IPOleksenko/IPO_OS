#ifndef IPO_NET_ICMP_H
#define IPO_NET_ICMP_H

#include <net/net.h>

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_DEST_UNREACH 3
#define ICMP_TYPE_ECHO_REQUEST 8
#define ICMP_TYPE_TIME_EXCEED  11

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} __attribute__((packed)) icmp_header_t;

typedef struct {
    ip4_addr_t src_ip;
    uint8_t    ttl;
    uint8_t    type;
    uint8_t    code;
    uint16_t   id;
    uint16_t   seq;
    uint16_t   data_len;
    uint8_t    data[1500];
    uint64_t   recv_tsc;
} icmp_echo_reply_t;

void icmp_init(void);
int  icmp_send_echo(ip4_addr_t dst_ip, uint16_t id, uint16_t seq, uint8_t ttl, const void *payload, uint16_t len);
void icmp_receive(ip4_addr_t src_ip, uint8_t ttl, const void *data, uint16_t len);
bool icmp_poll_reply(uint16_t id, uint16_t seq, icmp_echo_reply_t *out_reply, uint32_t timeout_ms);

#endif /* IPO_NET_ICMP_H */
