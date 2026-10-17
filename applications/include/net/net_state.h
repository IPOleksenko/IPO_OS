#ifndef IPO_NET_STATE_H
#define IPO_NET_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <net/net.h>
#include <net/icmp.h>
#include <driver/net/rtl8139.h>

#define NET_SHARED_MAGIC 0x49504F4Eu /* "IPON" in ASCII */

/* Forward declarations */
struct arp_entry;
struct udp_socket;
struct loopback_packet;
typedef struct {
    ip4_addr_t ip;
    mac_addr_t mac;
    uint32_t   last_seen_ms;
    bool       in_use;
} arp_entry_t;

typedef void (*udp_callback_t)(ip4_addr_t src_ip, uint16_t src_port, const void *data, uint16_t len);

typedef struct {
    uint16_t port;
    udp_callback_t callback;
    bool in_use;
} udp_socket_t;

typedef struct {
    uint16_t id;
    ip4_addr_t ip;
    bool valid;
} dns_reply_entry_t;

typedef struct {
    uint8_t data[1536];
    uint16_t len;
    bool valid;
} loopback_slot_t;

typedef struct {
    uint32_t magic;
    net_if_t default_if;
    rtl8139_dev_t rtl_dev;
    uint8_t rx_buffer[RTL8139_RX_BUFFER_SIZE] __attribute__((aligned(4)));
    uint8_t tx_buffers[4][RTL8139_TX_BUFFER_SIZE] __attribute__((aligned(4)));
    struct arp_entry *arp_cache;
    struct udp_socket *udp_sockets;
    arp_entry_t *arp_table;
    size_t arp_table_capacity;
    udp_socket_t *udp_table;
    size_t udp_table_capacity;
    dns_reply_entry_t *dns_queue;
    size_t dns_queue_capacity;
    size_t dns_q_idx;
    icmp_echo_reply_t *icmp_queue;
    size_t icmp_queue_capacity;
    size_t icmp_q_idx;
    struct loopback_packet *loop_head;
    struct loopback_packet *loop_tail;
    loopback_slot_t *loop_slots;
    size_t loopback_queue_capacity;
    size_t loop_head_idx;
    size_t loop_tail_idx;
} net_shared_ctx_t;

extern net_shared_ctx_t g_net_shared_ctx;

static inline net_shared_ctx_t *net_get_shared_context(void) {
    return &g_net_shared_ctx;
}

#endif /* IPO_NET_STATE_H */
