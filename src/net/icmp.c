#include <net/icmp.h>
#include <net/net_state.h>
#include <net/ipv4.h>
#include <system/timer.h>
#include <string.h>
#include <ioport.h>
#include <syscall.h>
#include <memory/kmalloc.h>

void icmp_echo_reply_init(icmp_echo_reply_t *reply) {
    if (!reply) return;
    memset(reply, 0, sizeof(*reply));
    reply->data = NULL;
    reply->data_cap = 0;
    reply->data_len = 0;
}

void icmp_echo_reply_free(icmp_echo_reply_t *reply) {
    if (!reply) return;
    if (reply->data) {
        kfree(reply->data);
        reply->data = NULL;
    }
    reply->data_cap = 0;
    reply->data_len = 0;
}

void icmp_init(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->icmp_queue_capacity > 0 && ctx->icmp_queue) {
        for (size_t i = 0; i < ctx->icmp_queue_capacity; i++) {
            icmp_echo_reply_free(&ctx->icmp_queue[i]);
            icmp_echo_reply_init(&ctx->icmp_queue[i]);
            ctx->icmp_queue[i].id = 0xFFFF;
            ctx->icmp_queue[i].seq = 0xFFFF;
        }
    }
    ctx->icmp_q_idx = 0;
}

int icmp_send_echo(ip4_addr_t dst_ip, uint16_t id, uint16_t seq, uint8_t ttl, const void *payload, uint16_t len) {
    if (!payload && len > 0) {
        return -1;
    }
    if ((size_t)len > 65535u - sizeof(icmp_header_t)) {
        return -1;
    }

    uint16_t total_len = (uint16_t)(sizeof(icmp_header_t) + len);
    uint8_t *tx_buf = (uint8_t *)kmalloc(total_len);
    if (!tx_buf) {
        return -1;
    }

    icmp_header_t *hdr = (icmp_header_t *)tx_buf;
    hdr->type = ICMP_TYPE_ECHO_REQUEST;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->id = htons(id);
    hdr->sequence = htons(seq);

    if (payload && len > 0) {
        memcpy(tx_buf + sizeof(icmp_header_t), payload, len);
    }

    hdr->checksum = net_checksum(tx_buf, total_len);

    int res = ip4_send(dst_ip, IPPROTO_ICMP, ttl, tx_buf, total_len);
    kfree(tx_buf);
    return res;
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
        uint8_t *rep_buf = (uint8_t *)kmalloc(len);
        if (!rep_buf) return;
        memcpy(rep_buf, data, len);
        icmp_header_t *rep_hdr = (icmp_header_t *)rep_buf;
        rep_hdr->type = ICMP_TYPE_ECHO_REPLY;
        rep_hdr->code = 0;
        rep_hdr->checksum = 0;
        rep_hdr->checksum = net_checksum(rep_buf, len);

        ip4_send(src_ip, IPPROTO_ICMP, 64, rep_buf, len);
        kfree(rep_buf);
        return;
    }

    if (hdr->type == ICMP_TYPE_ECHO_REPLY ||
        hdr->type == ICMP_TYPE_DEST_UNREACH ||
        hdr->type == ICMP_TYPE_TIME_EXCEED) {
        if (hdr->type != ICMP_TYPE_ECHO_REPLY && len >= sizeof(icmp_header_t) + sizeof(ip4_header_t)) {
            const uint8_t *inner = (const uint8_t *)data + sizeof(icmp_header_t);
            uint16_t inner_len = len - (uint16_t)sizeof(icmp_header_t);
            const ip4_header_t *orig_ip = (const ip4_header_t *)inner;
            uint8_t orig_ihl = (orig_ip->ihl_version & 0x0F) * 4;
            if (orig_ihl >= sizeof(ip4_header_t) && orig_ip->protocol == IPPROTO_ICMP &&
                inner_len >= (uint16_t)(orig_ihl + sizeof(icmp_header_t))) {
                const icmp_header_t *orig_icmp = (const icmp_header_t *)(inner + orig_ihl);
                id = ntohs(orig_icmp->id);
                seq = ntohs(orig_icmp->sequence);
            }
        }

        net_shared_ctx_t *ctx = net_get_shared_context();
        if (ctx->icmp_queue_capacity == 0 || !ctx->icmp_queue) {
            size_t init_cap = 16;
            icmp_echo_reply_t *init_q = (icmp_echo_reply_t *)kmalloc(init_cap * sizeof(icmp_echo_reply_t));
            if (!init_q) return;
            for (size_t i = 0; i < init_cap; i++) {
                icmp_echo_reply_init(&init_q[i]);
                init_q[i].id = 0xFFFF;
                init_q[i].seq = 0xFFFF;
            }
            ctx->icmp_queue = init_q;
            ctx->icmp_queue_capacity = init_cap;
            ctx->icmp_q_idx = 0;
        }
        size_t free_slot = ctx->icmp_queue_capacity;
        for (size_t i = 0; i < ctx->icmp_queue_capacity; i++) {
            if (ctx->icmp_queue[i].id == 0xFFFF && ctx->icmp_queue[i].seq == 0xFFFF) {
                free_slot = i;
                break;
            }
        }
        if (free_slot == ctx->icmp_queue_capacity) {
            size_t old_cap = ctx->icmp_queue_capacity;
            size_t new_cap = (old_cap == 0) ? 8 : (old_cap * 2);
            if (new_cap > old_cap && new_cap <= (SIZE_MAX / sizeof(icmp_echo_reply_t))) {
                icmp_echo_reply_t *new_q = (icmp_echo_reply_t *)krealloc(ctx->icmp_queue, new_cap * sizeof(icmp_echo_reply_t));
                if (new_q) {
                    for (size_t i = old_cap; i < new_cap; i++) {
                        icmp_echo_reply_init(&new_q[i]);
                        new_q[i].id = 0xFFFF;
                        new_q[i].seq = 0xFFFF;
                    }
                    free_slot = old_cap;
                    ctx->icmp_queue = new_q;
                    ctx->icmp_queue_capacity = new_cap;
                }
            }
        }
        if (free_slot == ctx->icmp_queue_capacity) {
            free_slot = ctx->icmp_q_idx % ctx->icmp_queue_capacity;
        }
        icmp_echo_reply_t *slot = &ctx->icmp_queue[free_slot];
        slot->src_ip = src_ip;
        slot->ttl = ttl;
        slot->type = hdr->type;
        slot->code = hdr->code;
        slot->id = id;
        slot->seq = seq;
        slot->recv_tsc = read_tsc();

        uint16_t payload_len = len - (uint16_t)sizeof(icmp_header_t);
        if (payload_len > 0) {
            if (payload_len > slot->data_cap || !slot->data) {
                uint8_t *new_buf = (uint8_t *)krealloc(slot->data, payload_len);
                if (new_buf) {
                    slot->data = new_buf;
                    slot->data_cap = payload_len;
                }
            }
            if (slot->data && slot->data_cap >= payload_len) {
                memcpy(slot->data, (const uint8_t *)data + sizeof(icmp_header_t), payload_len);
                slot->data_len = payload_len;
            } else {
                slot->data_len = 0;
            }
        } else {
            slot->data_len = 0;
        }

        ctx->icmp_q_idx = (free_slot + 1) % ctx->icmp_queue_capacity;
    }
}

bool icmp_poll_reply(uint16_t id, uint16_t seq, icmp_echo_reply_t *out_reply, uint32_t timeout_ms) {
    if (id == 0xFFFF && seq == 0xFFFF) return false;
    uint32_t start = timer_millis();

    while (timer_millis() - start < timeout_ms) {
        net_poll();

        net_shared_ctx_t *ctx = net_get_shared_context();
        if (ctx->icmp_queue) {
            for (size_t i = 0; i < ctx->icmp_queue_capacity; i++) {
                if (ctx->icmp_queue[i].id == id && ctx->icmp_queue[i].seq == seq) {
                    if (out_reply) {
                        out_reply->src_ip = ctx->icmp_queue[i].src_ip;
                        out_reply->ttl = ctx->icmp_queue[i].ttl;
                        out_reply->type = ctx->icmp_queue[i].type;
                        out_reply->code = ctx->icmp_queue[i].code;
                        out_reply->id = ctx->icmp_queue[i].id;
                        out_reply->seq = ctx->icmp_queue[i].seq;
                        out_reply->recv_tsc = ctx->icmp_queue[i].recv_tsc;
                        out_reply->data_len = 0;
                        if (ctx->icmp_queue[i].data_len > 0 && ctx->icmp_queue[i].data) {
                            if (ctx->icmp_queue[i].data_len > out_reply->data_cap || !out_reply->data) {
                                uint8_t *new_buf = (uint8_t *)krealloc(out_reply->data, ctx->icmp_queue[i].data_len);
                                if (new_buf) {
                                    out_reply->data = new_buf;
                                    out_reply->data_cap = ctx->icmp_queue[i].data_len;
                                }
                            }
                            if (out_reply->data && out_reply->data_cap >= ctx->icmp_queue[i].data_len) {
                                memcpy(out_reply->data, ctx->icmp_queue[i].data, ctx->icmp_queue[i].data_len);
                                out_reply->data_len = ctx->icmp_queue[i].data_len;
                            }
                        }
                    }
                    ctx->icmp_queue[i].id = 0xFFFF;
                    ctx->icmp_queue[i].seq = 0xFFFF;
                    return true;
                }
            }
        }
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        io_wait();
    }

    return false;
}

bool icmp_ping_ex(ip4_addr_t dst_ip, uint16_t id, uint16_t seq, uint8_t ttl,
                  const void *payload, uint16_t len,
                  uint32_t timeout_ms, uint32_t max_retries, uint32_t retry_interval_ms,
                  icmp_echo_reply_t *out_reply) {
    if (retry_interval_ms == 0) retry_interval_ms = 1000;
    if (timeout_ms == 0) timeout_ms = 2000;

    uint32_t retries_sent = 0;
    uint32_t start = timer_millis();
    uint32_t last_send = 0;
    bool sent_first = false;

    while (timer_millis() - start < timeout_ms) {
        if (!sent_first || (timer_millis() - last_send >= retry_interval_ms)) {
            if (sent_first) {
                if (max_retries > 0 && retries_sent >= max_retries) {
                    break;
                }
                retries_sent++;
            }
            if (icmp_send_echo(dst_ip, id, seq, ttl, payload, len) < 0) {
                return false;
            }
            last_send = timer_millis();
            sent_first = true;
        }

        if (icmp_poll_reply(id, seq, out_reply, 50)) {
            return true;
        }

        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        io_wait();
    }

    return false;
}
