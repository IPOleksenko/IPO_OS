#include <net/dns.h>
#include <net/udp.h>
#include <net/dyn_buf.h>
#include <net/net_state.h>
#include <system/timer.h>
#include <system/state.h>
#include <string.h>
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

static const uint8_t *dns_skip_name(const uint8_t *p, const uint8_t *end) {
    while (p < end) {
        uint8_t len = *p;
        if (len == 0) {
            return p + 1;
        }
        if ((len & 0xC0u) == 0xC0u) {
            return (p + 2 <= end) ? (p + 2) : NULL;
        }
        if ((len & 0xC0u) != 0) {
            return NULL;
        }
        if ((size_t)(end - p) < (size_t)len + 1u) {
            return NULL;
        }
        p += 1u + len;
    }
    return NULL;
}

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
        p = dns_skip_name(p, end);
        if (!p || (size_t)(end - p) < 4u) return;
        p += 4;
    }

    /* Parse Answer Section */
    for (uint16_t a = 0; a < ancount && p < end; a++) {
        p = dns_skip_name(p, end);
        if (!p || (size_t)(end - p) < 10u) break;

        uint16_t atype = (uint16_t)((p[0] << 8) | p[1]);
        uint16_t aclass = (uint16_t)((p[2] << 8) | p[3]);
        uint16_t rdlength = (uint16_t)((p[8] << 8) | p[9]);
        p += 10; // Past TYPE, CLASS, TTL, RDLENGTH

        if (p + rdlength > end) break;

        if (atype == 1 && aclass == 1 && rdlength == 4) {
            /* Type A, Class IN, 4 bytes IPv4 */
            ip4_addr_t resolved_ip = IP4_ADDR(p[0], p[1], p[2], p[3]);
            net_shared_ctx_t *ctx = net_get_shared_context();
            if (ctx->dns_queue_capacity == 0 || !ctx->dns_queue) {
                size_t init_cap = 16;
                dns_reply_entry_t *init_q = (dns_reply_entry_t *)kmalloc(init_cap * sizeof(dns_reply_entry_t));
                if (!init_q) return;
                memset(init_q, 0, init_cap * sizeof(dns_reply_entry_t));
                ctx->dns_queue = init_q;
                ctx->dns_queue_capacity = init_cap;
                ctx->dns_q_idx = 0;
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

bool dns_resolve_ex(const char *hostname, ip4_addr_t *out_ip, uint32_t timeout_ms,
                    uint32_t max_retries, uint32_t retry_interval_ms) {
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

    if (retry_interval_ms == 0) retry_interval_ms = 350;
    if (timeout_ms == 0) timeout_ms = 3000;

    uint32_t pid = (uint32_t)ipo_syscall(IPO_SYSCALL_GETPID, 0, NULL);
    uint16_t client_port = 52000 + (uint16_t)((pid * 43 + timer_millis()) % 10000);
    uint16_t query_id = (uint16_t)(((pid & 0x3F) << 10) ^ (timer_millis() & 0x3FF) ^ 0xA5A5);
    if (query_id == 0) query_id = 0x1234;

    /* Construct DNS query packet */
    dyn_buf_t qbuf;
    dyn_buf_init(&qbuf, 128);
    if (!qbuf.data) return false;

    dns_header_t hdr;
    hdr.id = htons(query_id);
    hdr.flags = htons(0x0100); // Standard recursive query
    hdr.qdcount = htons(1);
    hdr.ancount = 0;
    hdr.nscount = 0;
    hdr.arcount = 0;
    bool ok = dyn_buf_append(&qbuf, &hdr, sizeof(hdr));

    const char *src = hostname;
    while (ok && *src && *src != '/' && *src != ':') {
        size_t label_len = 0;
        while (src[label_len] && src[label_len] != '.' && src[label_len] != '/' && src[label_len] != ':') {
            label_len++;
        }
        if (label_len == 0) break;
        if (label_len > 63) label_len = 63;

        uint8_t llen = (uint8_t)label_len;
        ok = dyn_buf_append_byte(&qbuf, llen) && dyn_buf_append(&qbuf, src, label_len);

        src += label_len;
        if (*src == '.') src++;
        else break;
    }
    ok = ok &&
         dyn_buf_append_byte(&qbuf, 0x00) &&
         dyn_buf_append_byte(&qbuf, 0x00) &&
         dyn_buf_append_byte(&qbuf, 0x01) &&
         dyn_buf_append_byte(&qbuf, 0x00) &&
         dyn_buf_append_byte(&qbuf, 0x01);

    if (!ok || qbuf.size > 65535u - 28u) {
        dyn_buf_free(&qbuf);
        return false;
    }

    ip4_addr_t primary_dns = (netif->dns != 0) ? netif->dns : IP4_ADDR(8, 8, 8, 8);
    udp_send(primary_dns, client_port, 53, qbuf.data, (uint16_t)qbuf.size);

    uint32_t start = timer_millis();
    uint32_t last_send = start;
    uint32_t retries_sent = 0;
    bool resolved = false;

    while (timer_millis() - start < timeout_ms) {
        if (system_is_interrupted()) break;
        net_poll();

        net_shared_ctx_t *ctx = net_get_shared_context();
        if (ctx->dns_queue) {
            for (size_t i = 0; i < ctx->dns_queue_capacity; i++) {
                if (ctx->dns_queue[i].valid && ctx->dns_queue[i].id == query_id) {
                    *out_ip = ctx->dns_queue[i].ip;
                    ctx->dns_queue[i].valid = false;
                    resolved = true;
                    break;
                }
            }
        }
        if (resolved) break;

        if (timer_millis() - last_send >= retry_interval_ms) {
            if (max_retries > 0 && retries_sent >= max_retries) {
                /* User-specified retry limit reached */
                break;
            }
            ip4_addr_t target_dns = netif->dns;
            if (target_dns == 0 || (retries_sent % 3) == 1) {
                target_dns = IP4_ADDR(8, 8, 8, 8);
            } else if ((retries_sent % 3) == 2) {
                target_dns = IP4_ADDR(1, 1, 1, 1);
            }
            udp_send(target_dns, client_port, 53, qbuf.data, (uint16_t)qbuf.size);
            last_send = timer_millis();
            retries_sent++;
        }

        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        io_wait();
    }

    dyn_buf_free(&qbuf);
    return resolved;
}

bool dns_resolve(const char *hostname, ip4_addr_t *out_ip, uint32_t timeout_ms) {
    return dns_resolve_ex(hostname, out_ip, timeout_ms, 0, 350);
}
