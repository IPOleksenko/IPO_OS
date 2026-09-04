#include <net/ipv4.h>
#include <net/ethernet.h>
#include <net/arp.h>
#include <net/icmp.h>
#include <net/udp.h>
#include <net/loopback.h>
#include <string.h>

static uint16_t ip_ident_counter = 1;
static uint8_t ip_tx_buf[1536];

int ip4_send(ip4_addr_t dst_ip, uint8_t protocol, uint8_t ttl, const void *payload, uint16_t len) {
    if (!payload || len > (1500 - sizeof(ip4_header_t))) {
        return -1;
    }

    net_if_t *netif = net_get_interface();
    if (!netif) return -1;

    /* Check for loopback destination 127.0.0.0/8 */
    if ((dst_ip >> 24) == 127) {
        ip4_header_t *hdr = (ip4_header_t *)ip_tx_buf;
        hdr->ihl_version = 0x45;
        hdr->tos = 0;
        hdr->total_len = htons((uint16_t)(sizeof(ip4_header_t) + len));
        hdr->id = htons(ip_ident_counter++);
        hdr->flags_frag = 0;
        hdr->ttl = ttl ? ttl : 64;
        hdr->protocol = protocol;
        hdr->checksum = 0;
        hdr->src_ip = htonl(IP4_ADDR_LOOPBACK);
        hdr->dst_ip = htonl(dst_ip);
        hdr->checksum = net_checksum(hdr, sizeof(ip4_header_t));

        memcpy(ip_tx_buf + sizeof(ip4_header_t), payload, len);
        return loopback_send(ip_tx_buf, (uint16_t)(sizeof(ip4_header_t) + len));
    }

    if (!netif->link_up) {
        return -1;
    }

    /* Determine next hop */
    ip4_addr_t next_hop;
    if ((dst_ip & netif->netmask) == (netif->ip & netif->netmask)) {
        next_hop = dst_ip;
    } else {
        next_hop = netif->gateway;
    }

    mac_addr_t next_hop_mac;
    if (!arp_resolve(next_hop, &next_hop_mac, 1000)) {
        return -2; // ARP resolution failed
    }

    /* Build IPv4 packet */
    ip4_header_t *hdr = (ip4_header_t *)ip_tx_buf;
    hdr->ihl_version = 0x45;
    hdr->tos = 0;
    hdr->total_len = htons((uint16_t)(sizeof(ip4_header_t) + len));
    hdr->id = htons(ip_ident_counter++);
    hdr->flags_frag = 0;
    hdr->ttl = ttl ? ttl : 64;
    hdr->protocol = protocol;
    hdr->checksum = 0;
    hdr->src_ip = htonl(netif->ip);
    hdr->dst_ip = htonl(dst_ip);
    hdr->checksum = net_checksum(hdr, sizeof(ip4_header_t));

    memcpy(ip_tx_buf + sizeof(ip4_header_t), payload, len);

    uint16_t total_len = (uint16_t)(sizeof(ip4_header_t) + len);
    return eth_send(&next_hop_mac, ETHERTYPE_IPV4, ip_tx_buf, total_len);
}

#include <stdio.h>

void ip4_receive(const void *packet, uint16_t len) {
    if (!packet || len < sizeof(ip4_header_t)) {
        return;
    }

    const ip4_header_t *hdr = (const ip4_header_t *)packet;
    uint8_t version = (hdr->ihl_version >> 4) & 0x0F;
    uint8_t ihl = (hdr->ihl_version & 0x0F) * 4;

    if (version != 4 || ihl < sizeof(ip4_header_t) || len < ihl) {
        return;
    }

    uint16_t total_len = ntohs(hdr->total_len);
    if (total_len > len) {
        total_len = len;
    }

    /* Verify checksum */
    uint16_t csum = net_checksum(hdr, ihl);
    if (csum != 0) {
        serial_printf("[ip4] checksum failed: 0x%x\n", (unsigned int)csum);
        return;
    }

    net_if_t *netif = net_get_interface();
    ip4_addr_t dst_ip = ntohl(hdr->dst_ip);
    ip4_addr_t src_ip = ntohl(hdr->src_ip);

    /* Accept if destined to our IP, broadcast, or loopback */
    if (netif && dst_ip != netif->ip && dst_ip != IP4_ADDR_BROADCAST && (dst_ip >> 24) != 127) {
        return;
    }

    const uint8_t *payload = (const uint8_t *)packet + ihl;
    uint16_t payload_len = total_len - ihl;
    uint8_t ttl = hdr->ttl;

    if (hdr->protocol == IPPROTO_ICMP) {
        icmp_receive(src_ip, ttl, payload, payload_len);
    } else if (hdr->protocol == IPPROTO_UDP) {
        udp_receive(src_ip, payload, payload_len);
    }
}
