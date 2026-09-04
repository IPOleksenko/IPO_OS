#ifndef IPO_NET_STATE_H
#define IPO_NET_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <net/net.h>
#include <net/icmp.h>
#include <driver/net/rtl8139.h>

#define NET_SHARED_MAGIC 0x49504F4Eu /* "IPON" in ASCII */
#define NET_SHARED_ADDR  0x00060000u

#define ICMP_QUEUE_SIZE 8

/* Forward declarations */
struct arp_entry;
struct udp_socket;
struct loopback_packet;

typedef struct {
    uint32_t magic;
    net_if_t default_if;
    rtl8139_dev_t rtl_dev;
    uint8_t rx_buffer[RTL8139_RX_BUFFER_SIZE] __attribute__((aligned(4)));
    uint8_t tx_buffers[4][RTL8139_TX_BUFFER_SIZE] __attribute__((aligned(4)));
    struct arp_entry *arp_cache;
    struct udp_socket *udp_sockets;
    icmp_echo_reply_t icmp_queue[ICMP_QUEUE_SIZE];
    uint8_t icmp_q_idx;
    struct loopback_packet *loop_head;
    struct loopback_packet *loop_tail;
} net_shared_ctx_t;

static inline net_shared_ctx_t *net_get_shared_context(void) {
    return (net_shared_ctx_t *)NET_SHARED_ADDR;
}

#endif /* IPO_NET_STATE_H */
