#include <net/icmp.h>
#include <net/net_state.h>
#include <net/ipv4.h>
#include <system/timer.h>
#include <string.h>
#include <ioport.h>
#include <stdio.h>

static uint8_t icmp_tx_buf[1500];

void icmp_init(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    memset(ctx->icmp_queue, 0, sizeof(ctx->icmp_queue));
    ctx->icmp_q_idx = 0;
}

int icmp_send_echo(ip4_addr_t dst_ip, uint16_t id, uint16_t seq, uint8_t ttl, const void *payload, uint16_t len) {
    if (len > (1500 - sizeof(icmp_header_t))) {
        return -1;
    }

    icmp_header_t *hdr = (icmp_header_t *)icmp_tx_buf;
    hdr->type = ICMP_TYPE_ECHO_REQUEST;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->id = htons(id);
    hdr->sequence = htons(seq);

    if (payload && len > 0) {
        memcpy(icmp_tx_buf + sizeof(icmp_header_t), payload, len);
    }

    uint16_t total_len = (uint16_t)(sizeof(icmp_header_t) + len);
    hdr->checksum = net_checksum(icmp_tx_buf, total_len);

    return ip4_send(dst_ip, IPPROTO_ICMP, ttl, icmp_tx_buf, total_len);
}

void icmp_receive(ip4_addr_t src_ip, uint8_t ttl, const void *data, uint16_t len) {
    if (!data || len < sizeof(icmp_header_t)) {
        return;
    }

    if (net_checksum(data, len) != 0) {
        return;
    }

    const icmp_header_t *hdr = (const icmp_header_t *)data;
    uint16_t id = ntohs(hdr->id);
    uint16_t seq = ntohs(hdr->sequence);

    if (hdr->type == ICMP_TYPE_ECHO_REQUEST) {
        /* Echo request -> reply back */
        memcpy(icmp_tx_buf, data, len);
        icmp_header_t *rep_hdr = (icmp_header_t *)icmp_tx_buf;
        rep_hdr->type = ICMP_TYPE_ECHO_REPLY;
        rep_hdr->code = 0;
        rep_hdr->checksum = 0;
        rep_hdr->checksum = net_checksum(icmp_tx_buf, len);

        ip4_send(src_ip, IPPROTO_ICMP, 64, icmp_tx_buf, len);
        return;
    }

    if (hdr->type == ICMP_TYPE_ECHO_REPLY ||
        hdr->type == ICMP_TYPE_DEST_UNREACH ||
        hdr->type == ICMP_TYPE_TIME_EXCEED) {
        
        /* Store reply in circular queue */
        net_shared_ctx_t *ctx = net_get_shared_context();
        uint8_t q_idx = ctx->icmp_q_idx;
        icmp_echo_reply_t *slot = &ctx->icmp_queue[q_idx];
        slot->src_ip = src_ip;
        slot->ttl = ttl;
        slot->type = hdr->type;
        slot->code = hdr->code;
        slot->id = id;
        slot->seq = seq;
        slot->recv_tsc = read_tsc();

        uint16_t payload_len = len - (uint16_t)sizeof(icmp_header_t);
        if (payload_len > sizeof(slot->data)) {
            payload_len = sizeof(slot->data);
        }
        slot->data_len = payload_len;
        if (payload_len > 0) {
            memcpy(slot->data, (const uint8_t *)data + sizeof(icmp_header_t), payload_len);
        }

        ctx->icmp_q_idx = (q_idx + 1) % ICMP_QUEUE_SIZE;
    }
}

bool icmp_poll_reply(uint16_t id, uint16_t seq, icmp_echo_reply_t *out_reply, uint32_t timeout_ms) {
    uint32_t start = timer_millis();

    while (timer_millis() - start < timeout_ms) {
        net_poll();

        net_shared_ctx_t *ctx = net_get_shared_context();
        for (int i = 0; i < ICMP_QUEUE_SIZE; i++) {
            if (ctx->icmp_queue[i].id == id && ctx->icmp_queue[i].seq == seq) {
                if (out_reply) {
                    memcpy(out_reply, &ctx->icmp_queue[i], sizeof(icmp_echo_reply_t));
                }
                /* Consume entry */
                ctx->icmp_queue[i].id = 0xFFFF;
                ctx->icmp_queue[i].seq = 0xFFFF;
                return true;
            }
        }
        io_wait();
    }

    return false;
}
