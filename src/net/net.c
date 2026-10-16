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

bool net_init(const net_config_t *config) {
    if (!config) {
        return false;
    }

    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->magic == NET_SHARED_MAGIC) {
        if (ctx->rtl_dev.initialized) {
            ctx->default_if.link_up = true;
        }
        return ctx->icmp_queue_capacity >= config->icmp_queue_capacity &&
               ctx->arp_table_capacity >= config->arp_table_capacity &&
               ctx->udp_table_capacity >= config->udp_sockets_capacity &&
               ctx->dns_queue_capacity >= config->dns_queue_capacity &&
               ctx->loopback_queue_capacity >= config->loopback_queue_capacity;
    }

    if (ctx->magic != NET_SHARED_MAGIC) {
        memset(ctx, 0, sizeof(net_shared_ctx_t));
        ctx->icmp_queue = net_allocate_array(config->icmp_queue_capacity, sizeof(*ctx->icmp_queue));
        ctx->arp_table = net_allocate_array(config->arp_table_capacity, sizeof(*ctx->arp_table));
        ctx->udp_table = net_allocate_array(config->udp_sockets_capacity, sizeof(*ctx->udp_table));
        ctx->dns_queue = net_allocate_array(config->dns_queue_capacity, sizeof(*ctx->dns_queue));
        ctx->loop_slots = net_allocate_array(config->loopback_queue_capacity, sizeof(*ctx->loop_slots));
        if ((config->icmp_queue_capacity > 0 && !ctx->icmp_queue) ||
            (config->arp_table_capacity > 0 && !ctx->arp_table) ||
            (config->udp_sockets_capacity > 0 && !ctx->udp_table) ||
            (config->dns_queue_capacity > 0 && !ctx->dns_queue) ||
            (config->loopback_queue_capacity > 0 && !ctx->loop_slots)) {
            net_free_configured_buffers(ctx);
            return false;
        }
        ctx->icmp_queue_capacity = config->icmp_queue_capacity;
        ctx->arp_table_capacity = config->arp_table_capacity;
        ctx->udp_table_capacity = config->udp_sockets_capacity;
        ctx->dns_queue_capacity = config->dns_queue_capacity;
        ctx->loopback_queue_capacity = config->loopback_queue_capacity;
        if (ctx->dns_queue_capacity > 0) {
            memset(ctx->dns_queue, 0, ctx->dns_queue_capacity * sizeof(*ctx->dns_queue));
        }
        ctx->magic = NET_SHARED_MAGIC;
        memset(&ctx->default_if.mac, 0, sizeof(ctx->default_if.mac));
        ctx->default_if.ip = IP4_ADDR(192, 168, 7, 2);
        ctx->default_if.netmask = IP4_ADDR(255, 255, 255, 0);
        ctx->default_if.gateway = IP4_ADDR(192, 168, 7, 1);
        ctx->default_if.dns = IP4_ADDR(192, 168, 7, 1);
        ctx->default_if.link_up = false;

        loopback_init();
        arp_init();
        icmp_init();
        udp_init();
        tcp_init();
        socket_subsystem_init();

        if (rtl8139_init()) {
            const uint8_t *mac = rtl8139_get_mac();
            memcpy(ctx->default_if.mac.mac, mac, 6);
            ctx->default_if.link_up = true;
            char ip_str[32];
            ip_to_str(ctx->default_if.ip, ip_str, sizeof(ip_str));
            serial_printf("[net] RTL8139 init SUCCESS, MAC: %02x:%02x:%02x:%02x:%02x:%02x, IP: %s\n",
                          (unsigned int)mac[0], (unsigned int)mac[1], (unsigned int)mac[2],
                          (unsigned int)mac[3], (unsigned int)mac[4], (unsigned int)mac[5],
                          ip_str);
        } else {
            ctx->default_if.link_up = false;
            serial_printf("[net] RTL8139 init FAILED, link is down\n");
        }
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
