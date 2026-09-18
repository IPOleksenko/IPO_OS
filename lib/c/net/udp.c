#include <net/udp.h>
#include <net/net_state.h>
#include <net/ipv4.h>
#include <string.h>

static uint8_t udp_tx_buf[1500];

void udp_init(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    memset(ctx->udp_table, 0, sizeof(ctx->udp_table));
}

bool udp_bind(uint16_t port, udp_callback_t callback) {
    net_shared_ctx_t *ctx = net_get_shared_context();

    for (int i = 0; i < UDP_SOCKETS_MAX; i++) {
        if (ctx->udp_table[i].in_use && ctx->udp_table[i].port == port) {
            ctx->udp_table[i].callback = callback;
            return true;
        }
    }

    for (int i = 0; i < UDP_SOCKETS_MAX; i++) {
        if (!ctx->udp_table[i].in_use) {
            ctx->udp_table[i].port = port;
            ctx->udp_table[i].callback = callback;
            ctx->udp_table[i].in_use = true;
            return true;
        }
    }

    return false;
}

void udp_unbind(uint16_t port) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    for (int i = 0; i < UDP_SOCKETS_MAX; i++) {
        if (ctx->udp_table[i].in_use && ctx->udp_table[i].port == port) {
            ctx->udp_table[i].in_use = false;
            ctx->udp_table[i].callback = NULL;
            ctx->udp_table[i].port = 0;
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

/* Forward declaration for central DNS response processing */
void dns_process_response_packet(const void *data, uint16_t len);

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

    /* Central DNS response capture if from port 53 */
    if (src_port == 53) {
        dns_process_response_packet(payload, payload_len);
    }

    net_shared_ctx_t *ctx = net_get_shared_context();
    for (int i = 0; i < UDP_SOCKETS_MAX; i++) {
        if (ctx->udp_table[i].in_use && ctx->udp_table[i].port == dst_port) {
            if (ctx->udp_table[i].callback) {
                ctx->udp_table[i].callback(src_ip, src_port, payload, payload_len);
            }
        }
    }
}
