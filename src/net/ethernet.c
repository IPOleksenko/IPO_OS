#include <net/ethernet.h>
#include <driver/net/rtl8139.h>
#include <net/arp.h>
#include <net/ipv4.h>
#include <string.h>

#include <memory/kmalloc.h>

int eth_send(const mac_addr_t *dest, uint16_t ethertype, const void *payload, uint16_t len) {
    if (!dest || !payload) {
        return -1;
    }

    net_if_t *netif = net_get_interface();
    if (!netif || !netif->link_up) {
        return -1;
    }

    uint16_t total_len = (uint16_t)(sizeof(eth_header_t) + len);
    uint8_t *tx_frame = (uint8_t *)kmalloc(total_len);
    if (!tx_frame) {
        return -1;
    }

    eth_header_t *hdr = (eth_header_t *)tx_frame;
    memcpy(&hdr->dest, dest, sizeof(mac_addr_t));
    memcpy(&hdr->src, &netif->mac, sizeof(mac_addr_t));
    hdr->type = htons(ethertype);

    memcpy(tx_frame + sizeof(eth_header_t), payload, len);

    int res = rtl8139_send_packet(tx_frame, total_len);
    kfree(tx_frame);
    return res;
}

#include <stdio.h>

void eth_receive(const void *frame, uint16_t len) {
    if (!frame || len < sizeof(eth_header_t)) {
        return;
    }

    const eth_header_t *hdr = (const eth_header_t *)frame;
    net_if_t *netif = net_get_interface();
    if (!netif) return;

    /* Check if packet is destined for us or broadcast */
    bool is_broadcast = true;
    for (int i = 0; i < 6; i++) {
        if (hdr->dest.mac[i] != 0xFF) {
            is_broadcast = false;
            break;
        }
    }

    bool is_unicast = (memcmp(hdr->dest.mac, netif->mac.mac, 6) == 0);
    uint16_t type = ntohs(hdr->type);

    if (!is_broadcast && !is_unicast) {
        return;
    }

    const uint8_t *payload = (const uint8_t *)frame + sizeof(eth_header_t);
    uint16_t payload_len = len - (uint16_t)sizeof(eth_header_t);

    if (type == ETHERTYPE_ARP) {
        arp_receive(payload, payload_len);
    } else if (type == ETHERTYPE_IPV4) {
        ip4_receive(payload, payload_len);
    }
}
