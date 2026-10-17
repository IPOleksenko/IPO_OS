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
    dyn_buf_append(out, &hdr, sizeof(hdr));

    /* chaddr: standard 16 bytes on wire */
    if (pkt->chaddr.size > 0) {
        dyn_buf_append(out, pkt->chaddr.data, pkt->chaddr.size);
    }
    if (pkt->chaddr.size < 16) {
        uint8_t pad[16] = {0};
        dyn_buf_append(out, pad, 16 - pkt->chaddr.size);
    }

    /* sname: standard 64 bytes on wire */
    if (pkt->sname.size > 0) {
        dyn_buf_append(out, pkt->sname.data, pkt->sname.size);
    }
    if (pkt->sname.size < 64) {
        uint8_t pad[64] = {0};
        dyn_buf_append(out, pad, 64 - pkt->sname.size);
    }

    /* file: standard 128 bytes on wire */
    if (pkt->file.size > 0) {
        dyn_buf_append(out, pkt->file.data, pkt->file.size);
    }
    if (pkt->file.size < 128) {
        uint8_t pad[128] = {0};
        dyn_buf_append(out, pad, 128 - pkt->file.size);
    }

    /* magic cookie: 4 bytes */
    uint32_t cookie = pkt->magic_cookie;
    dyn_buf_append(out, &cookie, sizeof(cookie));

    /* options: self-expanding payload */
    if (pkt->options.size > 0) {
        dyn_buf_append(out, pkt->options.data, pkt->options.size);
    }

    /* Standard BOOTP wire packet minimum size: 300 bytes */
    while (out->size < 300) {
        dyn_buf_append_byte(out, 0);
    }

    return true;
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

    /* chaddr (offset 28, 16 bytes) */
    dyn_buf_append(&pkt->chaddr, raw + 28, 16);

    /* sname (offset 44, 64 bytes) */
    dyn_buf_append(&pkt->sname, raw + 44, 64);

    /* file (offset 108, 128 bytes) */
    dyn_buf_append(&pkt->file, raw + 108, 128);

    /* magic cookie (offset 236, 4 bytes) */
    memcpy(&pkt->magic_cookie, raw + 236, sizeof(uint32_t));

    /* options (offset 240, variable length) */
    if (wire_len > 240) {
        dyn_buf_append(&pkt->options, raw + 240, wire_len - 240);
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
    }

    dhcp_packet_free(&pkt);
}

bool dhcp_request_lease(uint32_t timeout_ms) {
    net_if_t *netif = net_get_interface();
    if (!netif || !netif->link_up) {
        return false;
    }

    /* Generate pseudo-random transaction ID based on TSC clock */
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

    /* Build DHCP DISCOVER packet with self-expanding buffers */
    dhcp_packet_t discover_pkt;
    dhcp_packet_init(&discover_pkt);
    discover_pkt.op = DHCP_OP_BOOTREQUEST;
    discover_pkt.htype = DHCP_HTYPE_ETHERNET;
    discover_pkt.hlen = DHCP_HLEN_ETHERNET;
    discover_pkt.hops = 0;
    discover_pkt.xid = htonl(s_dhcp_xid);
    discover_pkt.secs = 0;
    discover_pkt.flags = htons(0x8000); // Broadcast response requested
    dyn_buf_append(&discover_pkt.chaddr, netif->mac.mac, 6);
    discover_pkt.magic_cookie = htonl(DHCP_MAGIC_COOKIE);

    /* Options appended dynamically to self-expanding discover_pkt.options */
    uint8_t opt_msg_type[3] = { DHCP_OPT_MESSAGE_TYPE, 1, DHCP_DISCOVER };
    dyn_buf_append(&discover_pkt.options, opt_msg_type, sizeof(opt_msg_type));

    uint8_t opt_params[5] = { DHCP_OPT_PARAM_REQUEST, 3, DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER, DHCP_OPT_DNS };
    dyn_buf_append(&discover_pkt.options, opt_params, sizeof(opt_params));

    uint8_t opt_end = DHCP_OPT_END;
    dyn_buf_append(&discover_pkt.options, &opt_end, 1);

    dyn_buf_t discover_wire;
    dyn_buf_init(&discover_wire, 320);
    dhcp_packet_serialize(&discover_pkt, &discover_wire);

    /* Send DHCP Discover */
    s_dhcp_state = DHCP_STATE_DISCOVER_SENT;
    udp_send(IP4_ADDR_BROADCAST, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, discover_wire.data, (uint16_t)discover_wire.size);

    /* Wait for DHCP Offer */
    uint32_t start_ms = timer_millis();
    uint32_t last_send_ms = start_ms;

    while (timer_millis() - start_ms < timeout_ms && s_dhcp_state == DHCP_STATE_DISCOVER_SENT) {
        net_poll();
        if (s_dhcp_state == DHCP_STATE_OFFER_RECEIVED) {
            break;
        }
        if (timer_millis() - last_send_ms >= 500) {
            udp_send(IP4_ADDR_BROADCAST, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, discover_wire.data, (uint16_t)discover_wire.size);
            last_send_ms = timer_millis();
        }
        io_wait();
    }

    dyn_buf_free(&discover_wire);
    dhcp_packet_free(&discover_pkt);

    if (s_dhcp_state != DHCP_STATE_OFFER_RECEIVED || s_offered_ip == 0) {
        s_dhcp_state = DHCP_STATE_IDLE;
        return false;
    }

    /* Build DHCP REQUEST packet with self-expanding buffers */
    dhcp_packet_t request_pkt;
    dhcp_packet_init(&request_pkt);
    request_pkt.op = DHCP_OP_BOOTREQUEST;
    request_pkt.htype = DHCP_HTYPE_ETHERNET;
    request_pkt.hlen = DHCP_HLEN_ETHERNET;
    request_pkt.hops = 0;
    request_pkt.xid = htonl(s_dhcp_xid);
    request_pkt.secs = 0;
    request_pkt.flags = htons(0x8000);
    dyn_buf_append(&request_pkt.chaddr, netif->mac.mac, 6);
    request_pkt.magic_cookie = htonl(DHCP_MAGIC_COOKIE);

    uint8_t req_msg_type[3] = { DHCP_OPT_MESSAGE_TYPE, 1, DHCP_REQUEST };
    dyn_buf_append(&request_pkt.options, req_msg_type, sizeof(req_msg_type));

    uint8_t req_ip[6] = {
        DHCP_OPT_REQUESTED_IP, 4,
        (uint8_t)((s_offered_ip >> 24) & 0xFF),
        (uint8_t)((s_offered_ip >> 16) & 0xFF),
        (uint8_t)((s_offered_ip >> 8) & 0xFF),
        (uint8_t)(s_offered_ip & 0xFF)
    };
    dyn_buf_append(&request_pkt.options, req_ip, sizeof(req_ip));

    if (s_dhcp_server_id != 0) {
        uint8_t srv_id[6] = {
            DHCP_OPT_SERVER_ID, 4,
            (uint8_t)((s_dhcp_server_id >> 24) & 0xFF),
            (uint8_t)((s_dhcp_server_id >> 16) & 0xFF),
            (uint8_t)((s_dhcp_server_id >> 8) & 0xFF),
            (uint8_t)(s_dhcp_server_id & 0xFF)
        };
        dyn_buf_append(&request_pkt.options, srv_id, sizeof(srv_id));
    }

    uint8_t req_params[5] = { DHCP_OPT_PARAM_REQUEST, 3, DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER, DHCP_OPT_DNS };
    dyn_buf_append(&request_pkt.options, req_params, sizeof(req_params));

    uint8_t req_end = DHCP_OPT_END;
    dyn_buf_append(&request_pkt.options, &req_end, 1);

    dyn_buf_t request_wire;
    dyn_buf_init(&request_wire, 320);
    dhcp_packet_serialize(&request_pkt, &request_wire);

    /* Send DHCP Request */
    s_dhcp_state = DHCP_STATE_REQUEST_SENT;
    udp_send(IP4_ADDR_BROADCAST, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, request_wire.data, (uint16_t)request_wire.size);

    /* Wait for DHCP ACK */
    start_ms = timer_millis();
    last_send_ms = start_ms;

    while (timer_millis() - start_ms < timeout_ms && !s_dhcp_ack_received) {
        net_poll();
        if (s_dhcp_ack_received) {
            break;
        }
        if (timer_millis() - last_send_ms >= 500) {
            udp_send(IP4_ADDR_BROADCAST, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, request_wire.data, (uint16_t)request_wire.size);
            last_send_ms = timer_millis();
        }
        io_wait();
    }

    dyn_buf_free(&request_wire);
    dhcp_packet_free(&request_pkt);

    if (!s_dhcp_ack_received || s_offered_ip == 0) {
        s_dhcp_state = DHCP_STATE_IDLE;
        return false;
    }

    /* Successfully acquired DHCP lease: apply parameters to interface */
    if (s_offered_netmask == 0) s_offered_netmask = IP4_ADDR(255, 255, 255, 0);
    if (s_offered_gateway == 0) s_offered_gateway = (s_offered_ip & 0xFFFFFF00u) | 1u;
    if (s_offered_dns == 0) s_offered_dns = s_offered_gateway;

    net_set_ip(s_offered_ip, s_offered_netmask, s_offered_gateway, s_offered_dns);

    char ip_s[32], mask_s[32], gw_s[32], dns_s[32];
    ip_to_str(s_offered_ip, ip_s, sizeof(ip_s));
    ip_to_str(s_offered_netmask, mask_s, sizeof(mask_s));
    ip_to_str(s_offered_gateway, gw_s, sizeof(gw_s));
    ip_to_str(s_offered_dns, dns_s, sizeof(dns_s));

    serial_printf("[dhcp] Lease acquired: IP=%s, Mask=%s, Gateway=%s, DNS=%s\n",
                  ip_s, mask_s, gw_s, dns_s);

    s_dhcp_state = DHCP_STATE_IDLE;
    return true;
}
