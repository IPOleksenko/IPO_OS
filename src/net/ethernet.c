#include <net/ethernet.h>
#include <driver/net/rtl8139.h>
#include <net/arp.h>
#include <net/ipv4.h>
#include <string.h>

#include <memory/kmalloc.h>

#include <net/ethernet.h>
#include <driver/net/rtl8139.h>
#include <net/arp.h>
#include <net/ipv4.h>
#include <string.h>
#include <stdio.h>
#include <memory/kmalloc.h>

void eth_header_init(eth_header_t *frame) {
    if (!frame) return;
    mac_init(&frame->dest);
    mac_init(&frame->src);
    frame->type = 0;
    dyn_buf_init(&frame->payload, 128);
}

void eth_header_free(eth_header_t *frame) {
    if (!frame) return;
    mac_free(&frame->dest);
    mac_free(&frame->src);
    dyn_buf_free(&frame->payload);
    frame->type = 0;
}

size_t net_get_hw_addr_len(void) {
    net_if_t *netif = net_get_interface();
    if (netif && mac_get_len(&netif->mac) > 0) {
        return mac_get_len(&netif->mac);
    }
    size_t hw_len = rtl8139_get_mac_len();
    if (hw_len > 0) return hw_len;
    return 6; /* Probed from hardware */
}

int eth_send(const mac_addr_t *dest, uint16_t ethertype, const void *payload, uint16_t len) {
    if (!dest || !payload) {
        return -1;
    }

    net_if_t *netif = net_get_interface();
    if (!netif || !netif->link_up) {
        return -1;
    }

    size_t hw_len = net_get_hw_addr_len();
    size_t dest_l = mac_get_len(dest);
    if (dest_l == 0) dest_l = hw_len;
    size_t src_l = (mac_get_len(&netif->mac) > 0) ? mac_get_len(&netif->mac) : hw_len;

    /* Wire header: [destination MAC: dest_l bytes] [source MAC: src_l bytes] [EtherType: 2 bytes] */
    size_t wire_hdr_len = dest_l + src_l + sizeof(uint16_t);
    size_t total_len = wire_hdr_len + len;

    uint8_t *tx_frame = (uint8_t *)kmalloc(total_len);
    if (!tx_frame) {
        return -1;
    }

    const uint8_t *dest_bytes = mac_get_bytes(dest);
    if (dest_bytes && dest_l > 0) {
        memcpy(tx_frame, dest_bytes, dest_l);
    } else {
        memset(tx_frame, 0xFF, dest_l);
    }

    const uint8_t *src_bytes = mac_get_bytes(&netif->mac);
    if (src_bytes && src_l > 0) {
        memcpy(tx_frame + dest_l, src_bytes, src_l);
    } else {
        memset(tx_frame + dest_l, 0, src_l);
    }

    uint16_t net_type = htons(ethertype);
    memcpy(tx_frame + dest_l + src_l, &net_type, sizeof(uint16_t));

    memcpy(tx_frame + wire_hdr_len, payload, len);

    int res = rtl8139_send_packet(tx_frame, (uint16_t)total_len);
    kfree(tx_frame);
    return res;
}

void eth_receive(const void *frame, uint16_t len) {
    net_if_t *netif = net_get_interface();
    if (!netif || !frame) return;

    size_t hw_len = net_get_hw_addr_len();
    size_t min_hdr_len = hw_len * 2 + sizeof(uint16_t);
    if (len < min_hdr_len) {
        return;
    }

    const uint8_t *wire = (const uint8_t *)frame;
    const uint8_t *dest_bytes = wire;

    /* Check if packet is destined for us or broadcast */
    bool is_broadcast = true;
    for (size_t i = 0; i < hw_len; i++) {
        if (dest_bytes[i] != 0xFF) {
            is_broadcast = false;
            break;
        }
    }

    const uint8_t *my_mac = mac_get_bytes(&netif->mac);
    bool is_unicast = (my_mac && memcmp(dest_bytes, my_mac, hw_len) == 0);

    uint16_t wire_type;
    memcpy(&wire_type, wire + hw_len * 2, sizeof(uint16_t));
    uint16_t type = ntohs(wire_type);

    if (!is_broadcast && !is_unicast) {
        return;
    }

    const uint8_t *payload = wire + min_hdr_len;
    uint16_t payload_len = len - (uint16_t)min_hdr_len;

    if (type == ETHERTYPE_ARP) {
        arp_receive(payload, payload_len);
    } else if (type == ETHERTYPE_IPV4) {
        ip4_receive(payload, payload_len);
    }
}
