#include <net/dns.h>
#include <net/udp.h>
#include <system/timer.h>
#include <string.h>
#include <stdio.h>
#include <ioport.h>

typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed)) dns_header_t;

static volatile bool dns_resolved = false;
static ip4_addr_t dns_result_ip = 0;
static uint16_t dns_query_id = 0x1234;

static void dns_callback(ip4_addr_t src_ip, uint16_t src_port, const void *data, uint16_t len) {
    (void)src_ip;
    (void)src_port;
    if (!data || len < sizeof(dns_header_t)) return;

    const dns_header_t *hdr = (const dns_header_t *)data;
    if (ntohs(hdr->id) != dns_query_id) return;
    if ((ntohs(hdr->flags) & 0x8000) == 0) return; // Not a response
    if ((ntohs(hdr->flags) & 0x000F) != 0) return; // Response error (e.g. NXDOMAIN)

    uint16_t ancount = ntohs(hdr->ancount);
    if (ancount == 0) return;

    const uint8_t *p = (const uint8_t *)data + sizeof(dns_header_t);
    const uint8_t *end = (const uint8_t *)data + len;

    /* Skip Question Section */
    uint16_t qdcount = ntohs(hdr->qdcount);
    for (uint16_t q = 0; q < qdcount; q++) {
        while (p < end) {
            if (*p == 0) {
                p++;
                break;
            }
            if ((*p & 0xC0) == 0xC0) {
                p += 2;
                break;
            }
            p += (*p + 1);
        }
        p += 4; // Skip QTYPE and QCLASS
    }

    /* Parse Answer Section */
    for (uint16_t a = 0; a < ancount && p < end; a++) {
        /* Skip NAME */
        while (p < end) {
            if (*p == 0) {
                p++;
                break;
            }
            if ((*p & 0xC0) == 0xC0) {
                p += 2;
                break;
            }
            p += (*p + 1);
        }

        if (p + 10 > end) break;

        uint16_t atype = (uint16_t)((p[0] << 8) | p[1]);
        uint16_t aclass = (uint16_t)((p[2] << 8) | p[3]);
        uint16_t rdlength = (uint16_t)((p[8] << 8) | p[9]);
        p += 10; // Past TYPE, CLASS, TTL, RDLENGTH

        if (p + rdlength > end) break;

        if (atype == 1 && aclass == 1 && rdlength == 4) {
            /* Type A, Class IN, 4 bytes IPv4 */
            dns_result_ip = IP4_ADDR(p[0], p[1], p[2], p[3]);
            dns_resolved = true;
            return;
        }

        p += rdlength;
    }
}

bool dns_resolve(const char *hostname, ip4_addr_t *out_ip, uint32_t timeout_ms) {
    if (!hostname || !out_ip) return false;

    /* Check for localhost */
    if (strcmp(hostname, "localhost") == 0) {
        *out_ip = IP4_ADDR_LOOPBACK;
        return true;
    }

    /* Check if already a numeric IP */
    if (str_to_ip(hostname, out_ip)) {
        return true;
    }

    net_if_t *netif = net_get_interface();
    if (!netif || !netif->link_up) return false;

    uint16_t client_port = 53000 + (uint16_t)(timer_millis() % 1000);
    dns_resolved = false;
    dns_result_ip = 0;
    dns_query_id++;

    udp_bind(client_port, dns_callback);

    /* Construct DNS query packet */
    uint8_t query_buf[512];
    dns_header_t *hdr = (dns_header_t *)query_buf;
    hdr->id = htons(dns_query_id);
    hdr->flags = htons(0x0100); // Standard recursive query
    hdr->qdcount = htons(1);
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    uint8_t *qname = query_buf + sizeof(dns_header_t);
    const char *src = hostname;

    while (*src) {
        const char *dot = strchr(src, '.');
        size_t label_len = dot ? (size_t)(dot - src) : strlen(src);
        if (label_len > 63) label_len = 63;

        *qname++ = (uint8_t)label_len;
        memcpy(qname, src, label_len);
        qname += label_len;

        if (!dot) break;
        src = dot + 1;
    }
    *qname++ = 0x00; // Zero terminator for QNAME

    /* QTYPE = 1 (A), QCLASS = 1 (IN) */
    *qname++ = 0x00;
    *qname++ = 0x01;
    *qname++ = 0x00;
    *qname++ = 0x01;

    uint16_t query_len = (uint16_t)(qname - query_buf);
    udp_send(netif->dns, client_port, 53, query_buf, query_len);

    uint32_t start = timer_millis();
    uint32_t last_send = start;
    while (timer_millis() - start < timeout_ms) {
        net_poll();
        if (dns_resolved) {
            udp_unbind(client_port);
            *out_ip = dns_result_ip;
            return true;
        }
        if (timer_millis() - last_send >= 1000) {
            udp_send(netif->dns, client_port, 53, query_buf, query_len);
            last_send = timer_millis();
        }
        io_wait();
    }

    udp_unbind(client_port);
    return false;
}
