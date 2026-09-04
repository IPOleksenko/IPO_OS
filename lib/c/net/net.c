#include <net/net.h>
#include <net/net_state.h>
#include <driver/net/rtl8139.h>
#include <net/ethernet.h>
#include <net/arp.h>
#include <net/icmp.h>
#include <net/udp.h>
#include <net/loopback.h>
#include <stdio.h>
#include <string.h>

static uint8_t rx_packet_buf[1536];

uint16_t net_checksum(const void *data, size_t len) {
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }

    if (len == 1) {
        sum += *(const uint8_t *)ptr;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}

void ip_to_str(ip4_addr_t ip, char *buf, size_t size) {
    if (!buf || size == 0) return;
    snprintf(buf, size, "%u.%u.%u.%u",
             (unsigned int)((ip >> 24) & 0xFF),
             (unsigned int)((ip >> 16) & 0xFF),
             (unsigned int)((ip >> 8) & 0xFF),
             (unsigned int)(ip & 0xFF));
}

bool str_to_ip(const char *str, ip4_addr_t *out_ip) {
    if (!str || !out_ip) return false;

    unsigned int a = 0, b = 0, c = 0, d = 0;
    int parts = 0;
    unsigned int val = 0;
    bool has_digit = false;

    while (*str) {
        if (*str >= '0' && *str <= '9') {
            val = val * 10 + (*str - '0');
            if (val > 255) return false;
            has_digit = true;
        } else if (*str == '.') {
            if (!has_digit || parts >= 3) return false;
            if (parts == 0) a = val;
            else if (parts == 1) b = val;
            else if (parts == 2) c = val;
            parts++;
            val = 0;
            has_digit = false;
        } else {
            return false;
        }
        str++;
    }

    if (!has_digit || parts != 3) return false;
    d = val;

    *out_ip = IP4_ADDR(a, b, c, d);
    return true;
}

bool net_init(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->magic != NET_SHARED_MAGIC) {
        memset(ctx, 0, sizeof(net_shared_ctx_t));
        ctx->magic = NET_SHARED_MAGIC;
        ctx->default_if.mac = (mac_addr_t){{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}};
        ctx->default_if.ip = IP4_ADDR(10, 0, 2, 15);
        ctx->default_if.netmask = IP4_ADDR(255, 255, 255, 0);
        ctx->default_if.gateway = IP4_ADDR(10, 0, 2, 2);
        ctx->default_if.dns = IP4_ADDR(10, 0, 2, 3);
        ctx->default_if.link_up = false;

        loopback_init();
        arp_init();
        icmp_init();
        udp_init();

        if (rtl8139_init()) {
            const uint8_t *mac = rtl8139_get_mac();
            memcpy(ctx->default_if.mac.mac, mac, 6);
            ctx->default_if.link_up = true;
            serial_printf("[net] RTL8139 init SUCCESS, MAC: %x:%x:%x:%x:%x:%x, IP: 10.0.2.15\n",
                          (unsigned int)mac[0], (unsigned int)mac[1], (unsigned int)mac[2],
                          (unsigned int)mac[3], (unsigned int)mac[4], (unsigned int)mac[5]);
        } else {
            ctx->default_if.link_up = false;
            serial_printf("[net] RTL8139 init FAILED, link is down\n");
        }
    } else {
        if (ctx->rtl_dev.initialized) {
            ctx->default_if.link_up = true;
        }
    }

    return true;
}

void net_poll(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->magic != NET_SHARED_MAGIC) {
        net_init();
    }

    loopback_poll();

    if (!ctx->default_if.link_up) return;

    int len;
    while ((len = rtl8139_receive_packet(rx_packet_buf, sizeof(rx_packet_buf))) > 0) {
        eth_receive(rx_packet_buf, (uint16_t)len);
    }
}

net_if_t *net_get_interface(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->magic != NET_SHARED_MAGIC) {
        net_init();
    }
    return &ctx->default_if;
}
