#include <net/udp.h>
#include <net/net_state.h>
#include <net/ipv4.h>
#include <memory/kmalloc.h>
#include <string.h>

typedef struct udp_socket {
    uint16_t port;
    udp_callback_t callback;
    struct udp_socket *next;
} udp_socket_t;

static inline udp_socket_t **get_udp_sockets(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return &ctx->udp_sockets;
}

static uint8_t udp_tx_buf[1500];

void udp_init(void) {
    udp_socket_t *cur = *get_udp_sockets();
    while (cur) {
        udp_socket_t *next = cur->next;
        kfree(cur);
        cur = next;
    }
    *get_udp_sockets() = NULL;
}

bool udp_bind(uint16_t port, udp_callback_t callback) {
    udp_socket_t *cur = *get_udp_sockets();
    while (cur) {
        if (cur->port == port) {
            cur->callback = callback;
            return true;
        }
        cur = cur->next;
    }

    udp_socket_t *sock = (udp_socket_t *)kmalloc(sizeof(udp_socket_t));
    if (!sock) {
        return false;
    }
    sock->port = port;
    sock->callback = callback;
    sock->next = *get_udp_sockets();
    *get_udp_sockets() = sock;
    return true;
}

void udp_unbind(uint16_t port) {
    udp_socket_t **curr = get_udp_sockets();
    while (*curr) {
        udp_socket_t *entry = *curr;
        if (entry->port == port) {
            *curr = entry->next;
            kfree(entry);
        } else {
            curr = &entry->next;
        }
    }
}

int udp_send(ip4_addr_t dst_ip, uint16_t src_port, uint16_t dst_port, const void *payload, uint16_t len) {
    if (len > (1500 - sizeof(udp_header_t))) {
        return -1;
    }

    udp_header_t *hdr = (udp_header_t *)udp_tx_buf;
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length = htons((uint16_t)(sizeof(udp_header_t) + len));
    hdr->checksum = 0; // Checksum optional in IPv4 UDP

    if (payload && len > 0) {
        memcpy(udp_tx_buf + sizeof(udp_header_t), payload, len);
    }

    uint16_t total_len = (uint16_t)(sizeof(udp_header_t) + len);
    return ip4_send(dst_ip, IPPROTO_UDP, 64, udp_tx_buf, total_len);
}

void udp_receive(ip4_addr_t src_ip, const void *data, uint16_t len) {
    if (!data || len < sizeof(udp_header_t)) {
        return;
    }

    const udp_header_t *hdr = (const udp_header_t *)data;
    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint16_t ulen = ntohs(hdr->length);

    if (ulen < sizeof(udp_header_t) || ulen > len) {
        return;
    }

    const uint8_t *payload = (const uint8_t *)data + sizeof(udp_header_t);
    uint16_t payload_len = ulen - (uint16_t)sizeof(udp_header_t);

    udp_socket_t *cur = *get_udp_sockets();
    while (cur) {
        if (cur->port == dst_port) {
            if (cur->callback) {
                cur->callback(src_ip, src_port, payload, payload_len);
            }
        }
        cur = cur->next;
    }
}
