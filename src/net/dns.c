#include <net/dns.h>
#include <net/udp.h>
#include <net/net_state.h>
#include <system/timer.h>
#include <string.h>
#include <stdio.h>
#include <memory/kmalloc.h>
#include <ioport.h>
#include <syscall.h>

typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed)) dns_header_t;

void dns_process_response_packet(const void *data, uint16_t len) {
    if (!data || len < sizeof(dns_header_t)) return;

    const dns_header_t *hdr = (const dns_header_t *)data;
    if ((ntohs(hdr->flags) & 0x8000) == 0) return; // Not a response
    if ((ntohs(hdr->flags) & 0x000F) != 0) return; // Response error (e.g. NXDOMAIN)

    uint16_t ancount = ntohs(hdr->ancount);
    if (ancount == 0) return;

    const uint8_t *p = (const uint8_t *)data + sizeof(dns_header_t);
    const uint8_t *end = (const uint8_t *)data + len;

    /* Skip Question Section */
    uint16_t qdcount = ntohs(hdr->qdcount);
    for (uint16_t q = 0; q < qdcount; q++) {
        while (p < end) {
            if (*p == 0) {
                p++;
                break;
            }
            if ((*p & 0xC0) == 0xC0) {
                p += 2;
                break;
            }
            p += (*p + 1);
        }
        p += 4; // Skip QTYPE and QCLASS
    }

    /* Parse Answer Section */
    for (uint16_t a = 0; a < ancount && p < end; a++) {
        /* Skip NAME */
        while (p < end) {
            if (*p == 0) {
                p++;
                break;
            }
            if ((*p & 0xC0) == 0xC0) {
                p += 2;
                break;
            }
            p += (*p + 1);
        }

        if (p + 10 > end) break;

        uint16_t atype = (uint16_t)((p[0] << 8) | p[1]);
        uint16_t aclass = (uint16_t)((p[2] << 8) | p[3]);
        uint16_t rdlength = (uint16_t)((p[8] << 8) | p[9]);
        p += 10; // Past TYPE, CLASS, TTL, RDLENGTH

        if (p + rdlength > end) break;

        if (atype == 1 && aclass == 1 && rdlength == 4) {
            /* Type A, Class IN, 4 bytes IPv4 */
            ip4_addr_t resolved_ip = IP4_ADDR(p[0], p[1], p[2], p[3]);
            net_shared_ctx_t *ctx = net_get_shared_context();
            if (ctx->dns_queue_capacity == 0) {
                return;
            }
            size_t slot = ctx->dns_queue_capacity;
            for (size_t i = 0; i < ctx->dns_queue_capacity; i++) {
                if (!ctx->dns_queue[i].valid) {
                    slot = i;
                    break;
                }
            }
            if (slot == ctx->dns_queue_capacity) {
                /* Queue full of unconsumed answers -> dynamically double capacity */
                size_t old_cap = ctx->dns_queue_capacity;
                size_t new_cap = (old_cap == 0) ? 8 : (old_cap * 2);
                if (new_cap > old_cap && new_cap <= (SIZE_MAX / sizeof(dns_reply_entry_t))) {
                    dns_reply_entry_t *new_q = (dns_reply_entry_t *)krealloc(ctx->dns_queue, new_cap * sizeof(dns_reply_entry_t));
                    if (new_q) {
                        memset(new_q + old_cap, 0, (new_cap - old_cap) * sizeof(dns_reply_entry_t));
                        slot = old_cap;
                        ctx->dns_queue = new_q;
                        ctx->dns_queue_capacity = new_cap;
                    }
                }
            }
            if (slot == ctx->dns_queue_capacity) {
                /* Fallback if allocation failed: circular overwrite */
                slot = ctx->dns_q_idx % ctx->dns_queue_capacity;
            }
            ctx->dns_queue[slot].id = ntohs(hdr->id);
            ctx->dns_queue[slot].ip = resolved_ip;
            ctx->dns_queue[slot].valid = true;
            ctx->dns_q_idx = (slot + 1) % ctx->dns_queue_capacity;
            return;
        }

        p += rdlength;
    }
}

bool dns_resolve(const char *hostname, ip4_addr_t *out_ip, uint32_t timeout_ms) {
    if (!hostname || !out_ip) return false;

    /* Check for localhost */
    if (strcmp(hostname, "localhost") == 0) {
        *out_ip = IP4_ADDR_LOOPBACK;
        return true;
    }

    /* Check if already a numeric IP */
    if (str_to_ip(hostname, out_ip)) {
        return true;
    }

    net_if_t *netif = net_get_interface();
    if (!netif || !netif->link_up) return false;

    uint32_t pid = (uint32_t)ipo_syscall(IPO_SYSCALL_GETPID, 0, NULL);
    uint16_t client_port = 52000 + (uint16_t)((pid * 43 + timer_millis()) % 10000);
    uint16_t query_id = (uint16_t)(((pid & 0x3F) << 10) ^ (timer_millis() & 0x3FF) ^ 0xA5A5);
    if (query_id == 0) query_id = 0x1234;

    /* Construct DNS query packet */
    uint8_t query_buf[512];
    dns_header_t *hdr = (dns_header_t *)query_buf;
    hdr->id = htons(query_id);
    hdr->flags = htons(0x0100); // Standard recursive query
    hdr->qdcount = htons(1);
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    uint8_t *qname = query_buf + sizeof(dns_header_t);
    const char *src = hostname;

    while (*src && *src != '/' && *src != ':') {
        size_t label_len = 0;
        while (src[label_len] && src[label_len] != '.' && src[label_len] != '/' && src[label_len] != ':') {
            label_len++;
        }
        if (label_len == 0) break;
        if (label_len > 63) label_len = 63;

        *qname++ = (uint8_t)label_len;
        memcpy(qname, src, label_len);
        qname += label_len;

        src += label_len;
        if (*src == '.') src++;
        else break;
    }
    *qname++ = 0x00; // Zero terminator for QNAME

    /* QTYPE = 1 (A), QCLASS = 1 (IN) */
    *qname++ = 0x00;
    *qname++ = 0x01;
    *qname++ = 0x00;
    *qname++ = 0x01;

    uint16_t query_len = (uint16_t)(qname - query_buf);
    ip4_addr_t primary_dns = (netif->dns != 0) ? netif->dns : IP4_ADDR(8, 8, 8, 8);
    udp_send(primary_dns, client_port, 53, query_buf, query_len);

    uint32_t start = timer_millis();
    uint32_t last_send = start;
    uint32_t retry_count = 0;

    while (timer_millis() - start < timeout_ms) {
        net_poll();

        net_shared_ctx_t *ctx = net_get_shared_context();
        for (size_t i = 0; i < ctx->dns_queue_capacity; i++) {
            if (ctx->dns_queue[i].valid && ctx->dns_queue[i].id == query_id) {
                *out_ip = ctx->dns_queue[i].ip;
                ctx->dns_queue[i].valid = false;
                return true;
            }
        }

        if (timer_millis() - last_send >= 350) {
            ip4_addr_t target_dns = netif->dns;
            if (target_dns == 0 || (retry_count % 3) == 1) {
                target_dns = IP4_ADDR(8, 8, 8, 8);
            } else if ((retry_count % 3) == 2) {
                target_dns = IP4_ADDR(1, 1, 1, 1);
            }
            udp_send(target_dns, client_port, 53, query_buf, query_len);
            last_send = timer_millis();
            retry_count++;
        }

        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        io_wait();
    }

    return false;
}
