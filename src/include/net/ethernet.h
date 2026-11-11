#ifndef IPO_NET_ETHERNET_H
#define IPO_NET_ETHERNET_H

#include <net/net.h>
#include <net/dyn_buf.h>

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

/* Dynamic self-expanding Ethernet frame/header abstraction:
 * Automatically supports arbitrary hardware address lengths via kmalloc
 */
typedef struct {
    mac_addr_t dest;
    mac_addr_t src;
    uint16_t   type;
    dyn_buf_t  payload;
} eth_header_t;

typedef eth_header_t eth_frame_t;
typedef eth_header_t eth_dyn_frame_t;

void eth_header_init(eth_header_t *frame);
void eth_header_free(eth_header_t *frame);

#define eth_dyn_frame_init(f) eth_header_init(f)
#define eth_dyn_frame_free(f) eth_header_free(f)
#define eth_frame_init(f) eth_header_init(f)
#define eth_frame_free(f) eth_header_free(f)

size_t net_get_hw_addr_len(void);
int    eth_send(const mac_addr_t *dest, uint16_t ethertype, const void *payload, uint16_t len);
void   eth_receive(const void *frame, uint16_t len);

#endif /* IPO_NET_ETHERNET_H */
