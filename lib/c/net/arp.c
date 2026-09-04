#include <net/arp.h>
#include <net/net_state.h>
#include <net/ethernet.h>
#include <system/timer.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <ioport.h>

#define ARP_ENTRY_TIMEOUT_MS 60000u

typedef struct arp_entry {
    ip4_addr_t ip;
    mac_addr_t mac;
    uint32_t   last_seen_ms;
    struct arp_entry *next;
} arp_entry_t;

static inline arp_entry_t **get_arp_cache(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return &ctx->arp_cache;
}

static const mac_addr_t broadcast_mac = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
};

static void arp_cache_prune(void) {
    uint32_t now = timer_millis();
    arp_entry_t **curr = get_arp_cache();
    while (*curr) {
        if (now - (*curr)->last_seen_ms >= ARP_ENTRY_TIMEOUT_MS) {
            arp_entry_t *to_free = *curr;
            *curr = (*curr)->next;
            kfree(to_free);
        } else {
            curr = &(*curr)->next;
        }
    }
}

void arp_poll(void) {
    arp_cache_prune();
}

static void arp_cache_insert(ip4_addr_t ip, const mac_addr_t *mac) {
    arp_cache_prune();
    uint32_t now = timer_millis();
    arp_entry_t *cur = *get_arp_cache();
    while (cur) {
        if (cur->ip == ip) {
            memcpy(&cur->mac, mac, sizeof(mac_addr_t));
            cur->last_seen_ms = now;
            return;
        }
        cur = cur->next;
    }

    arp_entry_t *entry = (arp_entry_t *)kmalloc(sizeof(arp_entry_t));
    if (!entry) return;
    entry->ip = ip;
    memcpy(&entry->mac, mac, sizeof(mac_addr_t));
    entry->last_seen_ms = now;
    entry->next = *get_arp_cache();
    *get_arp_cache() = entry;
}

static bool arp_cache_lookup(ip4_addr_t ip, mac_addr_t *out_mac) {
    arp_cache_prune();

    arp_entry_t *cur = *get_arp_cache();
    while (cur) {
        if (cur->ip == ip) {
            if (out_mac) {
                memcpy(out_mac, &cur->mac, sizeof(mac_addr_t));
            }
            return true;
        }
        cur = cur->next;
    }
    return false;
}

void arp_init(void) {
    arp_entry_t *cur = *get_arp_cache();
    while (cur) {
        arp_entry_t *next = cur->next;
        kfree(cur);
        cur = next;
    }
    *get_arp_cache() = NULL;
}

void arp_send_request(ip4_addr_t target_ip) {
    net_if_t *netif = net_get_interface();
    if (!netif) return;

    arp_packet_t pkt;
    pkt.hw_type = htons(1);
    pkt.proto_type = htons(0x0800);
    pkt.hw_len = 6;
    pkt.proto_len = 4;
    pkt.opcode = htons(ARP_OP_REQUEST);
    memcpy(&pkt.sender_mac, &netif->mac, sizeof(mac_addr_t));
    pkt.sender_ip = htonl(netif->ip);
    memset(&pkt.target_mac, 0, sizeof(mac_addr_t));
    pkt.target_ip = htonl(target_ip);

    eth_send(&broadcast_mac, ETHERTYPE_ARP, &pkt, sizeof(pkt));
}

void arp_receive(const void *data, uint16_t len) {
    if (!data || len < sizeof(arp_packet_t)) return;

    const arp_packet_t *pkt = (const arp_packet_t *)data;

    if (ntohs(pkt->hw_type) != 1 || ntohs(pkt->proto_type) != 0x0800) {
        return;
    }
    net_if_t *netif = net_get_interface();
    if (!netif) return;

    ip4_addr_t sender_ip = ntohl(pkt->sender_ip);
    ip4_addr_t target_ip = ntohl(pkt->target_ip);
    uint16_t opcode = ntohs(pkt->opcode);

    /* Update cache with sender info */
    arp_cache_insert(sender_ip, &pkt->sender_mac);

    /* If it's a request for our IP, reply */
    if (opcode == ARP_OP_REQUEST && target_ip == netif->ip) {
        arp_packet_t reply;
        reply.hw_type = htons(1);
        reply.proto_type = htons(0x0800);
        reply.hw_len = 6;
        reply.proto_len = 4;
        reply.opcode = htons(ARP_OP_REPLY);
        memcpy(&reply.sender_mac, &netif->mac, sizeof(mac_addr_t));
        reply.sender_ip = htonl(netif->ip);
        memcpy(&reply.target_mac, &pkt->sender_mac, sizeof(mac_addr_t));
        reply.target_ip = htonl(sender_ip);

        eth_send(&pkt->sender_mac, ETHERTYPE_ARP, &reply, sizeof(reply));
    }
}

bool arp_resolve(ip4_addr_t target_ip, mac_addr_t *out_mac, uint32_t timeout_ms) {
    if (arp_cache_lookup(target_ip, out_mac)) {
        return true;
    }

    arp_send_request(target_ip);

    uint32_t start = timer_millis();
    uint32_t last_req = start;
    while (timer_millis() - start < timeout_ms) {
        net_poll();
        if (arp_cache_lookup(target_ip, out_mac)) {
            return true;
        }
        if (timer_millis() - last_req >= 250) {
            arp_send_request(target_ip);
            last_req = timer_millis();
        }
        io_wait();
    }

    return false;
}
