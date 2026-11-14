#include <net/dhcp.h>
#include <net/udp.h>
#include <net/ipv4.h>
#include <net/net_state.h>
#include <system/timer.h>
#include <string.h>
#include <stdio.h>
#include <ioport.h>

typedef enum {
    DHCP_STATE_IDLE,
    DHCP_STATE_DISCOVER_SENT,
    DHCP_STATE_OFFER_RECEIVED,
    DHCP_STATE_REQUEST_SENT,
    DHCP_STATE_ACK_RECEIVED
} dhcp_state_t;

static volatile dhcp_state_t s_dhcp_state = DHCP_STATE_IDLE;
static uint32_t s_dhcp_xid = 0;
static ip4_addr_t s_offered_ip = 0;
static ip4_addr_t s_offered_netmask = 0;
static ip4_addr_t s_offered_gateway = 0;
static ip4_addr_t s_offered_dns = 0;
static ip4_addr_t s_dhcp_server_id = 0;
static volatile bool s_dhcp_ack_received = false;
static uint16_t s_dhcp_max_msg_size = DHCP_DEFAULT_MAX_MSG_SIZE;

uint16_t dhcp_get_max_message_size(void) {
    return s_dhcp_max_msg_size > 0 ? s_dhcp_max_msg_size : DHCP_DEFAULT_MAX_MSG_SIZE;
}

void dhcp_set_max_message_size(uint16_t max_size) {
    if (max_size >= 576) {
        s_dhcp_max_msg_size = max_size;
    }
}

typedef struct {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
} __attribute__((packed)) dhcp_wire_header_t;

void dhcp_packet_init(dhcp_packet_t *pkt) {
    if (!pkt) return;
    memset(pkt, 0, sizeof(*pkt));
    dyn_buf_init(&pkt->chaddr, 16);
    dyn_buf_init(&pkt->sname, 64);
    dyn_buf_init(&pkt->file, 128);
    dyn_buf_init(&pkt->options, 128);
}

void dhcp_packet_free(dhcp_packet_t *pkt) {
    if (!pkt) return;
    dyn_buf_free(&pkt->chaddr);
    dyn_buf_free(&pkt->sname);
    dyn_buf_free(&pkt->file);
    dyn_buf_free(&pkt->options);
    memset(pkt, 0, sizeof(*pkt));
}

static bool dhcp_append_zeros(dyn_buf_t *buf, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!dyn_buf_append_byte(buf, 0)) return false;
    }
    return true;
}

static bool dhcp_append_fixed_field(dyn_buf_t *out, const dyn_buf_t *field, size_t width) {
    size_t copy_len = field->size < width ? field->size : width;
    if (copy_len > 0 && !dyn_buf_append(out, field->data, copy_len)) {
        return false;
    }
    return dhcp_append_zeros(out, width - copy_len);
}

static bool dhcp_append_be32(dyn_buf_t *buf, uint32_t val) {
    return dyn_buf_append_byte(buf, (uint8_t)((val >> 24) & 0xFF)) &&
           dyn_buf_append_byte(buf, (uint8_t)((val >> 16) & 0xFF)) &&
           dyn_buf_append_byte(buf, (uint8_t)((val >> 8) & 0xFF)) &&
           dyn_buf_append_byte(buf, (uint8_t)(val & 0xFF));
}

static void dhcp_append_common_options(dyn_buf_t *opts, const uint8_t *chaddr, size_t mac_l) {
    /* Client Identifier (Hardware Type = 1 [Ethernet] + MAC) */
    if (chaddr && mac_l > 0 && mac_l <= 64) {
        dyn_buf_append_byte(opts, DHCP_OPT_CLIENT_ID);
        dyn_buf_append_byte(opts, (uint8_t)(1 + mac_l));
        dyn_buf_append_byte(opts, 1);
        dyn_buf_append(opts, chaddr, mac_l);
    }

    /* Maximum DHCP Message Size */
    uint16_t cur_max_size = dhcp_get_max_message_size();
    dyn_buf_append_byte(opts, DHCP_OPT_MAX_MSG_SIZE);
    dyn_buf_append_byte(opts, 2);
    dyn_buf_append_byte(opts, (uint8_t)(cur_max_size >> 8));
    dyn_buf_append_byte(opts, (uint8_t)(cur_max_size & 0xFF));

    /* Host Name ("ipo-os") */
    const char *hostname = "ipo-os";
    size_t host_len = 6;
    dyn_buf_append_byte(opts, DHCP_OPT_HOSTNAME);
    dyn_buf_append_byte(opts, (uint8_t)host_len);
    dyn_buf_append(opts, hostname, host_len);

    /* Parameter Request List */
    dyn_buf_append_byte(opts, DHCP_OPT_PARAM_REQUEST);
    dyn_buf_append_byte(opts, 5);
    dyn_buf_append_byte(opts, DHCP_OPT_SUBNET_MASK);
    dyn_buf_append_byte(opts, DHCP_OPT_ROUTER);
    dyn_buf_append_byte(opts, DHCP_OPT_DNS);
    dyn_buf_append_byte(opts, DHCP_OPT_DOMAIN_NAME);
    dyn_buf_append_byte(opts, DHCP_OPT_BROADCAST_ADDR);

    dyn_buf_append_byte(opts, DHCP_OPT_END);
}

static uint32_t dhcp_retry_interval_ms(size_t retry_idx) {
    return (retry_idx < 3) ? ((uint32_t)(retry_idx + 1) * 1000u) : 4000u;
}

bool dhcp_packet_serialize(const dhcp_packet_t *pkt, dyn_buf_t *out) {
    if (!pkt || !out) return false;

    dyn_buf_reset(out);

    dhcp_wire_header_t hdr;
    hdr.op = pkt->op;
    hdr.htype = pkt->htype;
    hdr.hlen = pkt->hlen;
    hdr.hops = pkt->hops;
    hdr.xid = pkt->xid;
    hdr.secs = pkt->secs;
    hdr.flags = pkt->flags;
    hdr.ciaddr = pkt->ciaddr;
    hdr.yiaddr = pkt->yiaddr;
    hdr.siaddr = pkt->siaddr;
    hdr.giaddr = pkt->giaddr;

    uint32_t cookie = pkt->magic_cookie;
    if (!dyn_buf_append(out, &hdr, sizeof(hdr)) ||
        !dhcp_append_fixed_field(out, &pkt->chaddr, 16) ||
        !dhcp_append_fixed_field(out, &pkt->sname, 64) ||
        !dhcp_append_fixed_field(out, &pkt->file, 128) ||
        !dyn_buf_append(out, &cookie, sizeof(cookie))) {
        return false;
    }

    if (pkt->options.size > 0 && !dyn_buf_append(out, pkt->options.data, pkt->options.size)) {
        return false;
    }

    /* Minimum BOOTP payload size is 300 bytes */
    while (out->size < 300) {
        if (!dyn_buf_append_byte(out, 0)) return false;
    }

    return out->size <= 65535u - 28u;
}

bool dhcp_packet_parse(dhcp_packet_t *pkt, const void *wire_data, size_t wire_len) {
    if (!pkt || !wire_data || wire_len < 240) return false;

    dhcp_packet_init(pkt);
    const uint8_t *raw = (const uint8_t *)wire_data;

    const dhcp_wire_header_t *hdr = (const dhcp_wire_header_t *)raw;
    pkt->op = hdr->op;
    pkt->htype = hdr->htype;
    pkt->hlen = hdr->hlen;
    pkt->hops = hdr->hops;
    pkt->xid = hdr->xid;
    pkt->secs = hdr->secs;
    pkt->flags = hdr->flags;
    pkt->ciaddr = hdr->ciaddr;
    pkt->yiaddr = hdr->yiaddr;
    pkt->siaddr = hdr->siaddr;
    pkt->giaddr = hdr->giaddr;

    if (!dyn_buf_append(&pkt->chaddr, raw + 28, 16) ||
        !dyn_buf_append(&pkt->sname, raw + 44, 64) ||
        !dyn_buf_append(&pkt->file, raw + 108, 128)) {
        dhcp_packet_free(pkt);
        return false;
    }

    memcpy(&pkt->magic_cookie, raw + 236, sizeof(uint32_t));

    if (wire_len > 240 && !dyn_buf_append(&pkt->options, raw + 240, wire_len - 240)) {
        dhcp_packet_free(pkt);
        return false;
    }

    return true;
}

void dhcp_process_packet(const void *data, uint16_t len) {
    if (!data || len < 240) {
        return;
    }

    dhcp_packet_t pkt;
    if (!dhcp_packet_parse(&pkt, data, len)) {
        return;
    }

    if (pkt.op != DHCP_OP_BOOTREPLY) {
        dhcp_packet_free(&pkt);
        return;
    }

    if (ntohl(pkt.xid) != s_dhcp_xid) {
        dhcp_packet_free(&pkt);
        return;
    }

    if (ntohl(pkt.magic_cookie) != DHCP_MAGIC_COOKIE) {
        dhcp_packet_free(&pkt);
        return;
    }

    uint8_t msg_type = 0;
    ip4_addr_t subnet_mask = 0;
    ip4_addr_t router = 0;
    ip4_addr_t dns = 0;
    ip4_addr_t server_id = 0;

    const uint8_t *opt = pkt.options.data;
    const uint8_t *end = pkt.options.data + pkt.options.size;

    while (opt < end && *opt != DHCP_OPT_END) {
        if (*opt == DHCP_OPT_PAD) {
            opt++;
            continue;
        }

        if (opt + 1 >= end) break;
        uint8_t opt_code = opt[0];
        uint8_t opt_len = opt[1];
        const uint8_t *opt_val = opt + 2;

        if (opt_val + opt_len > end) break;

        switch (opt_code) {
            case DHCP_OPT_MESSAGE_TYPE:
                if (opt_len >= 1) {
                    msg_type = opt_val[0];
                }
                break;
            case DHCP_OPT_SUBNET_MASK:
                if (opt_len >= 4) {
                    subnet_mask = IP4_ADDR(opt_val[0], opt_val[1], opt_val[2], opt_val[3]);
                }
                break;
            case DHCP_OPT_ROUTER:
                if (opt_len >= 4) {
                    router = IP4_ADDR(opt_val[0], opt_val[1], opt_val[2], opt_val[3]);
                }
                break;
            case DHCP_OPT_DNS:
                if (opt_len >= 4) {
                    dns = IP4_ADDR(opt_val[0], opt_val[1], opt_val[2], opt_val[3]);
                }
                break;
            case DHCP_OPT_SERVER_ID:
                if (opt_len >= 4) {
                    server_id = IP4_ADDR(opt_val[0], opt_val[1], opt_val[2], opt_val[3]);
                }
                break;
            default:
                break;
        }

        opt += (2 + opt_len);
    }

    if (msg_type == DHCP_OFFER && s_dhcp_state == DHCP_STATE_DISCOVER_SENT) {
        s_offered_ip = ntohl(pkt.yiaddr);
        if (subnet_mask != 0) s_offered_netmask = subnet_mask;
        if (router != 0) s_offered_gateway = router;
        if (dns != 0) s_offered_dns = dns;
        if (server_id != 0) s_dhcp_server_id = server_id;
        s_dhcp_state = DHCP_STATE_OFFER_RECEIVED;
    } else if (msg_type == DHCP_ACK && s_dhcp_state == DHCP_STATE_REQUEST_SENT) {
        if (pkt.yiaddr != 0) s_offered_ip = ntohl(pkt.yiaddr);
        if (subnet_mask != 0) s_offered_netmask = subnet_mask;
        if (router != 0) s_offered_gateway = router;
        if (dns != 0) s_offered_dns = dns;
        s_dhcp_ack_received = true;
        s_dhcp_state = DHCP_STATE_ACK_RECEIVED;
    } else if (msg_type == DHCP_NAK && s_dhcp_state == DHCP_STATE_REQUEST_SENT) {
        serial_printf("[dhcp] Received DHCPNAK from server\n");
        s_dhcp_state = DHCP_STATE_IDLE;
        s_offered_ip = 0;
    }

    dhcp_packet_free(&pkt);
}

static void dhcp_init_bootrequest(dhcp_packet_t *pkt, const net_if_t *netif,
                                  uint32_t xid, const uint8_t **out_mac, size_t *out_mac_len) {
    dhcp_packet_init(pkt);
    const uint8_t *chaddr_bytes = mac_get_bytes(&netif->mac);
    size_t mac_l = mac_get_len(&netif->mac);
    pkt->op = DHCP_OP_BOOTREQUEST;
    pkt->htype = DHCP_HTYPE_ETHERNET;
    pkt->hlen = (uint8_t)(mac_l <= 16 ? mac_l : 6);
    pkt->xid = htonl(xid);
    pkt->flags = htons(0x8000); /* Broadcast flag */
    if (chaddr_bytes && mac_l > 0) {
        size_t chaddr_copy = mac_l <= 16 ? mac_l : 16;
        dyn_buf_append(&pkt->chaddr, chaddr_bytes, chaddr_copy);
    } else {
        uint8_t zeros[16] = {0};
        size_t zlen = (mac_l > 0 && mac_l <= 16) ? mac_l : 6;
        dyn_buf_append(&pkt->chaddr, zeros, zlen);
    }
    pkt->magic_cookie = htonl(DHCP_MAGIC_COOKIE);
    *out_mac = chaddr_bytes;
    *out_mac_len = mac_l;
}

static bool dhcp_send_and_wait(const dyn_buf_t *wire, uint32_t timeout_ms,
                               dhcp_state_t sent_state, dhcp_state_t target_state) {
    if (!wire->data || wire->size == 0) return false;

    const size_t max_retries = 4;
    size_t retry_idx = 0;
    s_dhcp_state = sent_state;
    udp_send(IP4_ADDR_BROADCAST, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, wire->data, (uint16_t)wire->size);

    uint32_t start_ms = timer_millis();
    uint32_t last_send_ms = start_ms;

    while (timer_millis() - start_ms < timeout_ms) {
        net_poll();
        if (s_dhcp_state == target_state) {
            return true;
        }
        if (s_dhcp_state != sent_state) {
            return false;
        }
        uint32_t current_interval = dhcp_retry_interval_ms(retry_idx);
        if (timer_millis() - last_send_ms >= current_interval) {
            udp_send(IP4_ADDR_BROADCAST, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, wire->data, (uint16_t)wire->size);
            last_send_ms = timer_millis();
            if (retry_idx < max_retries) retry_idx++;
        }
        io_wait();
    }
    return false;
}

bool dhcp_request_lease(uint32_t timeout_ms) {
    net_if_t *netif = net_get_interface();
    if (!netif || !netif->link_up) {
        return false;
    }

    /* Use 0.0.0.0 source address during handshake; restore previous IP on failure */
    ip4_addr_t prev_ip = netif->ip;
    netif->ip = 0;

    uint64_t tsc = read_tsc();
    s_dhcp_xid = (uint32_t)(tsc ^ (tsc >> 32) ^ 0x39A410ECu);
    if (s_dhcp_xid == 0) s_dhcp_xid = 0x12345678u;

    s_dhcp_state = DHCP_STATE_IDLE;
    s_offered_ip = 0;
    s_offered_netmask = 0;
    s_offered_gateway = 0;
    s_offered_dns = 0;
    s_dhcp_server_id = 0;
    s_dhcp_ack_received = false;

    dhcp_packet_t discover_pkt;
    const uint8_t *chaddr_bytes = NULL;
    size_t mac_l = 0;
    dhcp_init_bootrequest(&discover_pkt, netif, s_dhcp_xid, &chaddr_bytes, &mac_l);

    dyn_buf_append_byte(&discover_pkt.options, DHCP_OPT_MESSAGE_TYPE);
    dyn_buf_append_byte(&discover_pkt.options, 1);
    dyn_buf_append_byte(&discover_pkt.options, DHCP_DISCOVER);
    dhcp_append_common_options(&discover_pkt.options, chaddr_bytes, mac_l);

    dyn_buf_t discover_wire;
    dyn_buf_init(&discover_wire, 320);
    bool got_offer = dhcp_packet_serialize(&discover_pkt, &discover_wire) &&
                     dhcp_send_and_wait(&discover_wire, timeout_ms,
                                        DHCP_STATE_DISCOVER_SENT, DHCP_STATE_OFFER_RECEIVED);
    dyn_buf_free(&discover_wire);
    dhcp_packet_free(&discover_pkt);

    if (!got_offer || s_offered_ip == 0) {
        netif->ip = prev_ip;
        s_dhcp_state = DHCP_STATE_IDLE;
        return false;
    }

    dhcp_packet_t request_pkt;
    dhcp_init_bootrequest(&request_pkt, netif, s_dhcp_xid, &chaddr_bytes, &mac_l);

    dyn_buf_append_byte(&request_pkt.options, DHCP_OPT_MESSAGE_TYPE);
    dyn_buf_append_byte(&request_pkt.options, 1);
    dyn_buf_append_byte(&request_pkt.options, DHCP_REQUEST);

    dyn_buf_append_byte(&request_pkt.options, DHCP_OPT_REQUESTED_IP);
    dyn_buf_append_byte(&request_pkt.options, 4);
    dhcp_append_be32(&request_pkt.options, s_offered_ip);

    if (s_dhcp_server_id != 0) {
        dyn_buf_append_byte(&request_pkt.options, DHCP_OPT_SERVER_ID);
        dyn_buf_append_byte(&request_pkt.options, 4);
        dhcp_append_be32(&request_pkt.options, s_dhcp_server_id);
    }

    dhcp_append_common_options(&request_pkt.options, chaddr_bytes, mac_l);

    dyn_buf_t request_wire;
    dyn_buf_init(&request_wire, 320);
    bool got_ack = dhcp_packet_serialize(&request_pkt, &request_wire) &&
                   dhcp_send_and_wait(&request_wire, timeout_ms,
                                      DHCP_STATE_REQUEST_SENT, DHCP_STATE_ACK_RECEIVED);
    dyn_buf_free(&request_wire);
    dhcp_packet_free(&request_pkt);

    if (!got_ack || !s_dhcp_ack_received || s_offered_ip == 0) {
        netif->ip = prev_ip;
        s_dhcp_state = DHCP_STATE_IDLE;
        return false;
    }

    if (s_offered_netmask == 0) s_offered_netmask = IP4_ADDR(255, 255, 255, 0);
    if (s_offered_gateway == 0) s_offered_gateway = (s_offered_ip & 0xFFFFFF00u) | 1u;
    if (s_offered_dns == 0) s_offered_dns = s_offered_gateway;

    net_set_ip(s_offered_ip, s_offered_netmask, s_offered_gateway, s_offered_dns);

    char ip_s[16], mask_s[16], gw_s[16], dns_s[16];
    ip_to_str(s_offered_ip, ip_s, sizeof(ip_s));
    ip_to_str(s_offered_netmask, mask_s, sizeof(mask_s));
    ip_to_str(s_offered_gateway, gw_s, sizeof(gw_s));
    ip_to_str(s_offered_dns, dns_s, sizeof(dns_s));
    printf("[dhcp] Lease acquired: IP=%s, Mask=%s, Gateway=%s, DNS=%s\n",
           ip_s, mask_s, gw_s, dns_s);

    s_dhcp_state = DHCP_STATE_IDLE;
    return true;
}
