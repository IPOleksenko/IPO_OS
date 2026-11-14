#include <net/arp.h>
#include <net/net_state.h>
#include <net/ethernet.h>
#include <system/timer.h>
#include <string.h>
#include <memory/kmalloc.h>
#include <ioport.h>
#include <syscall.h>

#define ARP_ENTRY_TIMEOUT_MS 60000u

static mac_addr_t broadcast_mac;
static bool broadcast_mac_inited = false;

static const mac_addr_t *get_broadcast_mac(void) {
    if (!broadcast_mac_inited) {
        mac_init(&broadcast_mac);
        mac_set_broadcast(&broadcast_mac);
        broadcast_mac_inited = true;
    }
    return &broadcast_mac;
}

static void arp_cache_prune(void) {
    uint32_t now = timer_millis();
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx->arp_table) return;
    for (size_t i = 0; i < ctx->arp_table_capacity; i++) {
        if (ctx->arp_table[i].in_use && (now - ctx->arp_table[i].last_seen_ms >= ARP_ENTRY_TIMEOUT_MS)) {
            ctx->arp_table[i].in_use = false;
        }
    }
}

static void arp_cache_insert(ip4_addr_t ip, const mac_addr_t *mac) {
    arp_cache_prune();
    uint32_t now = timer_millis();
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!mac || mac_get_len(mac) == 0) return;

    /* Update existing entry if present */
    if (ctx->arp_table) {
        for (size_t i = 0; i < ctx->arp_table_capacity; i++) {
            if (ctx->arp_table[i].in_use && ctx->arp_table[i].ip == ip) {
                if (mac_copy(&ctx->arp_table[i].mac, mac)) {
                    ctx->arp_table[i].last_seen_ms = now;
                }
                return;
            }
        }
    }

    /* Find free slot */
    size_t slot = ctx->arp_table_capacity;
    if (ctx->arp_table) {
        for (size_t i = 0; i < ctx->arp_table_capacity; i++) {
            if (!ctx->arp_table[i].in_use) {
                slot = i;
                break;
            }
        }
    }

    /* Expand table if full; fall back to evicting oldest entry on OOM */
    if (slot == ctx->arp_table_capacity) {
        size_t new_cap = (ctx->arp_table_capacity == 0) ? 16 : (ctx->arp_table_capacity * 2);
        if (new_cap > ctx->arp_table_capacity && new_cap <= (SIZE_MAX / sizeof(arp_entry_t))) {
            arp_entry_t *new_table = (arp_entry_t *)krealloc(ctx->arp_table, new_cap * sizeof(arp_entry_t));
            if (new_table) {
                memset(new_table + ctx->arp_table_capacity, 0, (new_cap - ctx->arp_table_capacity) * sizeof(arp_entry_t));
                slot = ctx->arp_table_capacity;
                ctx->arp_table = new_table;
                ctx->arp_table_capacity = new_cap;
            }
        }

        if (slot == ctx->arp_table_capacity) {
            if (ctx->arp_table_capacity == 0 || !ctx->arp_table) {
                return;
            }
            uint32_t oldest_time = 0xFFFFFFFF;
            slot = 0;
            for (size_t i = 0; i < ctx->arp_table_capacity; i++) {
                if (ctx->arp_table[i].last_seen_ms < oldest_time) {
                    oldest_time = ctx->arp_table[i].last_seen_ms;
                    slot = i;
                }
            }
        }
    }

    if (!mac_copy(&ctx->arp_table[slot].mac, mac)) {
        return;
    }
    ctx->arp_table[slot].ip = ip;
    ctx->arp_table[slot].last_seen_ms = now;
    ctx->arp_table[slot].in_use = true;
}

static bool arp_cache_lookup(ip4_addr_t ip, mac_addr_t *out_mac) {
    arp_cache_prune();
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx->arp_table) return false;

    for (size_t i = 0; i < ctx->arp_table_capacity; i++) {
        if (ctx->arp_table[i].in_use && ctx->arp_table[i].ip == ip) {
            if (out_mac && !mac_copy(out_mac, &ctx->arp_table[i].mac)) {
                return false;
            }
            return true;
        }
    }
    return false;
}

void arp_init(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->arp_table_capacity > 0 && ctx->arp_table) {
        for (size_t i = 0; i < ctx->arp_table_capacity; i++) {
            mac_free(&ctx->arp_table[i].mac);
        }
        memset(ctx->arp_table, 0, ctx->arp_table_capacity * sizeof(*ctx->arp_table));
    }
}

void arp_packet_init(arp_packet_t *pkt) {
    if (!pkt) return;
    memset(pkt, 0, sizeof(*pkt));
    mac_init(&pkt->sender_mac);
    mac_init(&pkt->target_mac);
}

void arp_packet_free(arp_packet_t *pkt) {
    if (!pkt) return;
    mac_free(&pkt->sender_mac);
    mac_free(&pkt->target_mac);
    memset(pkt, 0, sizeof(*pkt));
}

bool arp_packet_parse(arp_packet_t *pkt, const void *data, size_t len) {
    if (!pkt || !data || len < 8) return false;
    arp_packet_init(pkt);

    const uint8_t *raw = (const uint8_t *)data;
    pkt->hw_type    = (uint16_t)((raw[0] << 8) | raw[1]);
    pkt->proto_type = (uint16_t)((raw[2] << 8) | raw[3]);
    pkt->hw_len     = raw[4];
    pkt->proto_len  = raw[5];
    pkt->opcode     = (uint16_t)((raw[6] << 8) | raw[7]);

    size_t needed = 8 + (pkt->hw_len * 2) + (pkt->proto_len * 2);
    if (len < needed) {
        arp_packet_free(pkt);
        return false;
    }

    size_t off = 8;
    if (!mac_set(&pkt->sender_mac, raw + off, pkt->hw_len)) {
        arp_packet_free(pkt);
        return false;
    }
    off += pkt->hw_len;

    if (pkt->proto_len == 4) {
        uint32_t s_ip;
        memcpy(&s_ip, raw + off, 4);
        pkt->sender_ip = ntohl(s_ip);
    }
    off += pkt->proto_len;

    if (!mac_set(&pkt->target_mac, raw + off, pkt->hw_len)) {
        arp_packet_free(pkt);
        return false;
    }
    off += pkt->hw_len;

    if (pkt->proto_len == 4) {
        uint32_t t_ip;
        memcpy(&t_ip, raw + off, 4);
        pkt->target_ip = ntohl(t_ip);
    }

    return true;
}

size_t arp_packet_serialize(const arp_packet_t *pkt, void *buf, size_t buf_size) {
    if (!pkt || !buf) return 0;
    uint8_t hw_len = pkt->hw_len ? pkt->hw_len : (uint8_t)mac_get_len(&pkt->sender_mac);
    if (hw_len == 0) hw_len = 6;
    uint8_t proto_len = pkt->proto_len ? pkt->proto_len : 4;
    size_t needed = 8 + (size_t)hw_len * 2 + (size_t)proto_len * 2;
    if (buf_size < needed) return 0;

    uint8_t *raw = (uint8_t *)buf;
    raw[0] = (uint8_t)(pkt->hw_type >> 8);
    raw[1] = (uint8_t)(pkt->hw_type & 0xFF);
    raw[2] = (uint8_t)(pkt->proto_type >> 8);
    raw[3] = (uint8_t)(pkt->proto_type & 0xFF);
    raw[4] = hw_len;
    raw[5] = proto_len;
    raw[6] = (uint8_t)(pkt->opcode >> 8);
    raw[7] = (uint8_t)(pkt->opcode & 0xFF);

    size_t off = 8;
    const uint8_t *s_mac = mac_get_bytes(&pkt->sender_mac);
    size_t s_len = mac_get_len(&pkt->sender_mac);
    for (size_t i = 0; i < hw_len; i++) {
        raw[off + i] = (s_mac && i < s_len) ? s_mac[i] : 0;
    }
    off += hw_len;

    memset(raw + off, 0, proto_len);
    uint32_t s_ip = htonl(pkt->sender_ip);
    memcpy(raw + off, &s_ip, proto_len <= 4 ? proto_len : 4);
    off += proto_len;

    const uint8_t *t_mac = mac_get_bytes(&pkt->target_mac);
    size_t t_len = mac_get_len(&pkt->target_mac);
    for (size_t i = 0; i < hw_len; i++) {
        raw[off + i] = (t_mac && i < t_len) ? t_mac[i] : 0;
    }
    off += hw_len;

    memset(raw + off, 0, proto_len);
    uint32_t t_ip = htonl(pkt->target_ip);
    memcpy(raw + off, &t_ip, proto_len <= 4 ? proto_len : 4);
    off += proto_len;

    return off;
}

size_t arp_packet_calc_size(const arp_packet_t *pkt) {
    if (!pkt) return 0;
    uint8_t hw_len = pkt->hw_len ? pkt->hw_len : (uint8_t)mac_get_len(&pkt->sender_mac);
    if (hw_len == 0) hw_len = (uint8_t)mac_get_len(&pkt->target_mac);
    if (hw_len == 0) hw_len = 6;
    uint8_t proto_len = pkt->proto_len ? pkt->proto_len : 4;
    return 8 + (size_t)hw_len * 2 + (size_t)proto_len * 2;
}

size_t arp_packet_serialize_dynamic(const arp_packet_t *pkt, uint8_t **out_raw) {
    if (!pkt || !out_raw) return 0;
    *out_raw = NULL;

    size_t needed = arp_packet_calc_size(pkt);
    size_t raw_cap = (needed > 0) ? needed : 28;

    uint8_t *raw = (uint8_t *)kmalloc(raw_cap);
    if (!raw) return 0;

    size_t len = arp_packet_serialize(pkt, raw, raw_cap);
    while (len == 0 && raw_cap < 65536) {
        size_t new_cap = raw_cap * 2;
        uint8_t *new_raw = (uint8_t *)krealloc(raw, new_cap);
        if (!new_raw) {
            kfree(raw);
            return 0;
        }
        raw = new_raw;
        raw_cap = new_cap;
        len = arp_packet_serialize(pkt, raw, raw_cap);
    }

    if (len == 0) {
        kfree(raw);
        return 0;
    }

    *out_raw = raw;
    return len;
}

static void arp_send_packet(const mac_addr_t *eth_dest, uint16_t opcode, uint8_t hw_len,
                            const mac_addr_t *sender_mac, ip4_addr_t sender_ip,
                            const mac_addr_t *target_mac, ip4_addr_t target_ip) {
    arp_packet_t pkt;
    arp_packet_init(&pkt);
    pkt.hw_type = 1;
    pkt.proto_type = 0x0800;
    pkt.hw_len = hw_len;
    pkt.proto_len = 4;
    pkt.opcode = opcode;
    if (sender_mac) mac_copy(&pkt.sender_mac, sender_mac);
    pkt.sender_ip = sender_ip;
    if (target_mac) mac_copy(&pkt.target_mac, target_mac);
    pkt.target_ip = target_ip;

    uint8_t *raw = NULL;
    size_t len = arp_packet_serialize_dynamic(&pkt, &raw);
    if (raw && len > 0) {
        eth_send(eth_dest, ETHERTYPE_ARP, raw, (uint16_t)len);
    }
    if (raw) {
        kfree(raw);
    }
    arp_packet_free(&pkt);
}

void arp_send_request(ip4_addr_t target_ip) {
    net_if_t *netif = net_get_interface();
    if (!netif) return;

    size_t mac_l = mac_get_len(&netif->mac);
    uint8_t hw_len = (uint8_t)(mac_l ? mac_l : 6);
    arp_send_packet(get_broadcast_mac(), ARP_OP_REQUEST, hw_len,
                    &netif->mac, netif->ip, NULL, target_ip);
}

void arp_send_gratuitous(void) {
    net_if_t *netif = net_get_interface();
    if (!netif || netif->ip == 0) return;

    size_t mac_l = mac_get_len(&netif->mac);
    uint8_t hw_len = (uint8_t)(mac_l ? mac_l : 6);
    arp_send_packet(get_broadcast_mac(), ARP_OP_REQUEST, hw_len,
                    &netif->mac, netif->ip, get_broadcast_mac(), netif->ip);
}

void arp_receive(const void *data, uint16_t len) {
    if (!data || len < 8) return;

    arp_packet_t pkt;
    if (!arp_packet_parse(&pkt, data, len)) {
        return;
    }

    if (pkt.hw_type != 1 || pkt.proto_type != 0x0800 || pkt.proto_len != 4) {
        arp_packet_free(&pkt);
        return;
    }
    net_if_t *netif = net_get_interface();
    if (!netif) {
        arp_packet_free(&pkt);
        return;
    }

    ip4_addr_t sender_ip = pkt.sender_ip;
    ip4_addr_t target_ip = pkt.target_ip;
    uint16_t opcode = pkt.opcode;

    if (sender_ip != 0 && mac_get_len(&pkt.sender_mac) > 0) {
        arp_cache_insert(sender_ip, &pkt.sender_mac);
    }

    if (opcode == ARP_OP_REQUEST && netif->ip != 0 && target_ip == netif->ip) {
        size_t mac_l = mac_get_len(&netif->mac);
        uint8_t hw_len = pkt.hw_len ? pkt.hw_len : (uint8_t)(mac_l ? mac_l : 6);
        arp_send_packet(&pkt.sender_mac, ARP_OP_REPLY, hw_len,
                        &netif->mac, netif->ip, &pkt.sender_mac, sender_ip);
    }
    arp_packet_free(&pkt);
}

bool arp_resolve_ex(ip4_addr_t target_ip, mac_addr_t *out_mac, uint32_t timeout_ms,
                    uint32_t max_retries, uint32_t retry_interval_ms) {
    if (arp_cache_lookup(target_ip, out_mac)) {
        return true;
    }

    if (retry_interval_ms == 0) retry_interval_ms = 250;
    if (timeout_ms == 0) timeout_ms = 1000;

    arp_send_request(target_ip);

    uint32_t start = timer_millis();
    uint32_t last_req = start;
    uint32_t retries_sent = 0;

    while (timer_millis() - start < timeout_ms) {
        net_poll();
        if (arp_cache_lookup(target_ip, out_mac)) {
            return true;
        }
        if (timer_millis() - last_req >= retry_interval_ms) {
            if (max_retries > 0 && retries_sent >= max_retries) {
                break;
            }
            arp_send_request(target_ip);
            last_req = timer_millis();
            retries_sent++;
        }
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        io_wait();
    }

    return false;
}

bool arp_resolve(ip4_addr_t target_ip, mac_addr_t *out_mac, uint32_t timeout_ms) {
    return arp_resolve_ex(target_ip, out_mac, timeout_ms, 0, 250);
}
