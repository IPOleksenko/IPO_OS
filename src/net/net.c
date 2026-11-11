#include <net/net.h>
#include <net/net_state.h>
#include <driver/net/rtl8139.h>
#include <net/ethernet.h>
#include <net/arp.h>
#include <net/icmp.h>
#include <net/udp.h>
#include <net/tcp.h>
#include <net/socket.h>
#include <net/loopback.h>
#include <net/dhcp.h>
#include <memory/kmalloc.h>
#include <stdio.h>
#include <string.h>

static uint8_t *rx_packet_buf = NULL;
static size_t rx_packet_buf_cap = 0;

static bool ensure_rx_packet_buf(size_t needed) {
    if (rx_packet_buf && rx_packet_buf_cap >= needed) {
        return true;
    }
    size_t new_cap = (rx_packet_buf_cap == 0) ? 2048 : (rx_packet_buf_cap * 2);
    while (new_cap < needed) {
        new_cap *= 2;
    }
    uint8_t *new_buf = (uint8_t *)krealloc(rx_packet_buf, new_cap);
    if (!new_buf) {
        return false;
    }
    rx_packet_buf = new_buf;
    rx_packet_buf_cap = new_cap;
    return true;
}

net_shared_ctx_t g_net_shared_ctx __attribute__((aligned(4)));

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

char *ip_to_str_dyn(ip4_addr_t ip) {
    size_t sz = 32;
    char *buf = (char *)kmalloc(sz);
    if (!buf) return NULL;
    ip_to_str(ip, buf, sz);
    return buf;
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

static void net_free_configured_buffers(net_shared_ctx_t *ctx) {
    kfree(ctx->arp_table);
    kfree(ctx->udp_table);
    kfree(ctx->dns_queue);
    kfree(ctx->icmp_queue);
    kfree(ctx->loop_slots);
    if (ctx->rx_buffer) {
        kfree(ctx->rx_buffer);
        ctx->rx_buffer = NULL;
    }
    if (ctx->tx_buffers) {
        for (size_t i = 0; i < ctx->tx_buffers_count; i++) {
            if (ctx->tx_buffers[i]) {
                kfree(ctx->tx_buffers[i]);
            }
        }
        kfree(ctx->tx_buffers);
        ctx->tx_buffers = NULL;
        ctx->tx_buffers_count = 0;
    }
    ctx->arp_table = NULL;
    ctx->udp_table = NULL;
    ctx->dns_queue = NULL;
    ctx->icmp_queue = NULL;
    ctx->loop_slots = NULL;
}

static void *net_allocate_array(size_t count, size_t element_size) {
    if (count == 0 || count > SIZE_MAX / element_size) {
        return NULL;
    }
    return kmalloc(count * element_size);
}

bool net_init(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->magic == NET_SHARED_MAGIC) {
        if (ctx->rtl_dev.initialized) {
            ctx->default_if.link_up = true;
        }
        return true;
    }

    size_t icmp_cap = 16;
    size_t arp_cap  = 16;
    size_t udp_cap  = 16;
    size_t dns_cap  = 16;
    size_t loop_cap = 16;

    memset(ctx, 0, sizeof(net_shared_ctx_t));
    ctx->icmp_queue = net_allocate_array(icmp_cap, sizeof(*ctx->icmp_queue));
    ctx->arp_table  = net_allocate_array(arp_cap,  sizeof(*ctx->arp_table));
    ctx->udp_table  = net_allocate_array(udp_cap,  sizeof(*ctx->udp_table));
    ctx->dns_queue  = net_allocate_array(dns_cap,  sizeof(*ctx->dns_queue));
    ctx->loop_slots = net_allocate_array(loop_cap, sizeof(*ctx->loop_slots));

    if (!ctx->icmp_queue || !ctx->arp_table || !ctx->udp_table ||
        !ctx->dns_queue  || !ctx->loop_slots) {
        net_free_configured_buffers(ctx);
        return false;
    }

    ctx->icmp_queue_capacity = icmp_cap;
    ctx->arp_table_capacity  = arp_cap;
    ctx->udp_table_capacity  = udp_cap;
    ctx->dns_queue_capacity  = dns_cap;
    ctx->loopback_queue_capacity = loop_cap;

    for (size_t i = 0; i < ctx->icmp_queue_capacity; i++) {
        ctx->icmp_queue[i].id = 0xFFFF;
        ctx->icmp_queue[i].seq = 0xFFFF;
    }
    memset(ctx->arp_table, 0, ctx->arp_table_capacity * sizeof(*ctx->arp_table));
    memset(ctx->udp_table, 0, ctx->udp_table_capacity * sizeof(*ctx->udp_table));
    memset(ctx->dns_queue, 0, ctx->dns_queue_capacity * sizeof(*ctx->dns_queue));
    memset(ctx->loop_slots, 0, ctx->loopback_queue_capacity * sizeof(*ctx->loop_slots));

    ctx->magic = NET_SHARED_MAGIC;
    ctx->loopback_ip = IP4_ADDR(127, 0, 0, 1);
    mac_init(&ctx->default_if.mac);
    ctx->default_if.ip = 0;
    ctx->default_if.netmask = 0;
    ctx->default_if.gateway = 0;
    ctx->default_if.dns = 0;
    ctx->default_if.link_up = false;

        loopback_init();
        arp_init();
        icmp_init();
        udp_init();
        tcp_init();
        socket_subsystem_init();
        if (rtl8139_init()) {
            size_t hw_mac_len = 0;
            const uint8_t *mac = rtl8139_get_mac(&hw_mac_len);
            mac_set(&ctx->default_if.mac, mac, hw_mac_len);
            ctx->default_if.link_up = true;
            char *mac_str = mac_format_dyn(&ctx->default_if.mac);
            printf("[net] RTL8139 init SUCCESS, MAC: %s\n", mac_str ? mac_str : "unknown");
            if (mac_str) kfree(mac_str);

            /* Dynamic configuration via DHCP */
            printf("[net] Discovering network configuration via DHCP...\n");
            if (dhcp_request_lease(10000)) {
                printf("[net] Dynamic network configuration established via DHCP.\n");
            } else {
                const uint8_t *hw_mac = mac_get_bytes(&ctx->default_if.mac);
                size_t mac_l = mac_get_len(&ctx->default_if.mac);
                uint8_t b3 = (hw_mac && mac_l >= 2) ? hw_mac[mac_l - 2] : 1;
                uint8_t b4 = (hw_mac && mac_l >= 1) ? hw_mac[mac_l - 1] : 2;
                if (b3 == 0) b3 = 1;
                if (b3 == 255) b3 = 254;
                if (b4 == 0) b4 = 1;
                if (b4 == 255) b4 = 254;

                ctx->default_if.ip = IP4_ADDR(169, 254, b3, b4);
                ctx->default_if.netmask = IP4_ADDR(255, 255, 0, 0);
                ctx->default_if.gateway = 0;
                ctx->default_if.dns = 0;

                char ip_s[32];
                ip_to_str(ctx->default_if.ip, ip_s, sizeof(ip_s));
                printf("[net] DHCP not detected, auto-configured Link-Local IP from hardware MAC: %s/16\n", ip_s);
            }
        } else {
            ctx->default_if.link_up = false;
            printf("[net] RTL8139 init FAILED, link is down\n");
        }

    return true;
}

void net_poll(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->magic != NET_SHARED_MAGIC) {
        return;
    }

    loopback_poll();
    tcp_poll();

    if (!ctx->default_if.link_up) return;

    int peek_len;
    while ((peek_len = rtl8139_peek_packet_size()) > 0) {
        if (!ensure_rx_packet_buf((size_t)peek_len + 64u)) {
            break;
        }
        int len = rtl8139_receive_packet(rx_packet_buf, (uint32_t)rx_packet_buf_cap);
        if (len <= 0) break;
        eth_receive(rx_packet_buf, (uint16_t)len);
    }

    if (ensure_rx_packet_buf(2048)) {
        int len;
        while ((len = rtl8139_receive_packet(rx_packet_buf, (uint32_t)rx_packet_buf_cap)) > 0) {
            eth_receive(rx_packet_buf, (uint16_t)len);
        }
    }
}

net_if_t *net_get_interface(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->magic != NET_SHARED_MAGIC) {
        return NULL;
    }
    return &ctx->default_if;
}

void net_set_ip(ip4_addr_t ip, ip4_addr_t netmask, ip4_addr_t gateway, ip4_addr_t dns) {
    net_if_t *netif = net_get_interface();
    if (!netif) return;
    if (ip != 0) netif->ip = ip;
    if (netmask != 0) netif->netmask = netmask;
    if (gateway != 0) netif->gateway = gateway;
    if (dns != 0) netif->dns = dns;
    if (ip != 0) {
        arp_send_gratuitous();
    }
}
