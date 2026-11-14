#include <net/tcp.h>
#include <net/ipv4.h>
#include <net/net.h>
#include <driver/net/rtl8139.h>
#include <system/timer.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <stdio.h>

static tcp_tcb_t *tcb_head = NULL;
static uint32_t   next_tcb_id = 1;
static uint16_t   next_ephemeral_port = 0;

#define TCP_SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define TCP_SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define TCP_SEQ_GT(a, b)  ((int32_t)((a) - (b)) > 0)
#define TCP_SEQ_GEQ(a, b) ((int32_t)((a) - (b)) >= 0)

static bool tcp_port_in_use(uint16_t port) {
    for (tcp_tcb_t *c = tcb_head; c; c = c->next) {
        if (c->state != TCP_STATE_CLOSED && c->local_port == port) {
            return true;
        }
    }
    return false;
}

uint16_t tcp_alloc_ephemeral_port(void) {
    uint32_t hw_wnd = rtl8139_get_hw_max_window();
    uint32_t base = hw_wnd - (hw_wnd >> 2);
    uint32_t max_p = (hw_wnd > 35u) ? (hw_wnd - 35u) : 0u;
    if (base == 0 || base > 65535u || max_p < base) {
        base = 49152u;
        max_p = 65500u;
    }

    uint16_t ephem_base = (uint16_t)base;
    uint16_t ephem_max  = (uint16_t)max_p;
    if (next_ephemeral_port < ephem_base || next_ephemeral_port > ephem_max) {
        next_ephemeral_port = ephem_base;
    }

    uint32_t span = (uint32_t)(ephem_max - ephem_base + 1u);
    for (uint32_t i = 0; i < span; i++) {
        uint16_t candidate = next_ephemeral_port++;
        if (next_ephemeral_port > ephem_max) {
            next_ephemeral_port = ephem_base;
        }
        if (!tcp_port_in_use(candidate)) {
            return candidate;
        }
    }

    uint16_t fallback = next_ephemeral_port++;
    if (next_ephemeral_port > ephem_max) {
        next_ephemeral_port = ephem_base;
    }
    return fallback;
}

const char *tcp_state_to_str(tcp_state_t state) {
    switch (state) {
        case TCP_STATE_CLOSED:       return "CLOSED";
        case TCP_STATE_LISTEN:       return "LISTEN";
        case TCP_STATE_SYN_SENT:     return "SYN_SENT";
        case TCP_STATE_SYN_RECEIVED: return "SYN_RECEIVED";
        case TCP_STATE_ESTABLISHED:  return "ESTABLISHED";
        case TCP_STATE_FIN_WAIT_1:   return "FIN_WAIT_1";
        case TCP_STATE_FIN_WAIT_2:   return "FIN_WAIT_2";
        case TCP_STATE_CLOSE_WAIT:   return "CLOSE_WAIT";
        case TCP_STATE_CLOSING:      return "CLOSING";
        case TCP_STATE_LAST_ACK:     return "LAST_ACK";
        case TCP_STATE_TIME_WAIT:    return "TIME_WAIT";
        default:                     return "UNKNOWN";
    }
}

void tcp_init(void) {
    tcb_head = NULL;
    next_tcb_id = 1;
    next_ephemeral_port = 0;
}

uint16_t tcp_checksum(ip4_addr_t src_ip, ip4_addr_t dst_ip, const void *tcp_seg, uint16_t seg_len) {
    tcp_pseudo_header_t ph;
    ph.src_ip = htonl(src_ip);
    ph.dst_ip = htonl(dst_ip);
    ph.zero = 0;
    ph.protocol = IPPROTO_TCP;
    ph.tcp_length = htons(seg_len);

    uint32_t sum = 0;

    /* Pseudo-header sum */
    const uint16_t *p = (const uint16_t *)&ph;
    for (size_t i = 0; i < sizeof(ph) / 2; i++) {
        sum += *p++;
    }

    /* TCP segment sum */
    const uint16_t *s = (const uint16_t *)tcp_seg;
    size_t len = seg_len;
    while (len > 1) {
        sum += *s++;
        len -= 2;
    }
    if (len == 1) {
        sum += *(const uint8_t *)s;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

tcp_tcb_t *tcp_alloc_tcb(uint16_t mss, uint32_t default_rto_ms, uint32_t max_rto_ms,
                         uint8_t max_retries, uint32_t time_wait_ms) {
    tcp_tcb_t *tcb = (tcp_tcb_t *)kmalloc(sizeof(tcp_tcb_t));
    if (!tcb) return NULL;

    uint32_t hw_wnd = rtl8139_get_hw_max_window();
    size_t hw_burst = rtl8139_get_hw_max_dma_burst();
    size_t hw_tx_cap = rtl8139_get_hw_rx_ring_size() * 2u;

    memset(tcb, 0, sizeof(tcp_tcb_t));
    tcb->id = next_tcb_id++;
    tcb->state = TCP_STATE_CLOSED;
    tcb->mss = mss ? mss : rtl8139_get_hw_mss();
    tcb->default_rto_ms = default_rto_ms;
    tcb->max_rto_ms = max_rto_ms;
    tcb->max_retries = max_retries;
    tcb->time_wait_duration_ms = time_wait_ms;
    tcb->rto_ms = default_rto_ms;
    tcb->max_rcv_wnd = hw_wnd;
    tcb->max_tx_buf = hw_tx_cap;
    tcb->rcv_wnd = hw_wnd;
    tcb->snd_wnd = hw_wnd;

    dyn_buf_init(&tcb->rx_buf, hw_burst);
    dyn_buf_init(&tcb->tx_buf, hw_burst);
    if (!tcb->rx_buf.data || !tcb->tx_buf.data) {
        dyn_buf_free(&tcb->rx_buf);
        dyn_buf_free(&tcb->tx_buf);
        kfree(tcb);
        return NULL;
    }

    /* Link into global list */
    tcb->next = tcb_head;
    tcb_head = tcb;

    return tcb;
}

void tcp_free_tcb(tcp_tcb_t *tcb) {
    if (!tcb) return;

    /* Abort and free any queued unaccepted child connections */
    while (tcb->next_pending) {
        tcp_tcb_t *child = tcb->next_pending;
        tcb->next_pending = child->next_pending;
        child->next_pending = NULL;
        child->parent_listener = NULL;
        tcp_abort(child);
        tcp_free_tcb(child);
    }

    /* Clear parent pointer on any half-open children referencing this listener */
    for (tcp_tcb_t *c = tcb_head; c; c = c->next) {
        if (c->parent_listener == tcb) {
            c->parent_listener = NULL;
        }
    }

    /* Unlink from global list */
    if (tcb_head == tcb) {
        tcb_head = tcb->next;
    } else {
        tcp_tcb_t *curr = tcb_head;
        while (curr && curr->next != tcb) {
            curr = curr->next;
        }
        if (curr) {
            curr->next = tcb->next;
        }
    }

    dyn_buf_free(&tcb->rx_buf);
    dyn_buf_free(&tcb->tx_buf);
    kfree(tcb);
}

tcp_tcb_t *tcp_find_tcb(ip4_addr_t local_ip, uint16_t local_port,
                        ip4_addr_t remote_ip, uint16_t remote_port) {
    for (tcp_tcb_t *c = tcb_head; c; c = c->next) {
        if (c->state == TCP_STATE_LISTEN || c->state == TCP_STATE_CLOSED) continue;

        if (c->local_port == local_port && c->remote_port == remote_port &&
            c->remote_ip == remote_ip &&
            (c->local_ip == local_ip || c->local_ip == IP4_ADDR_ANY || local_ip == IP4_ADDR_ANY)) {
            return c;
        }
    }
    return NULL;
}

tcp_tcb_t *tcp_find_listener(ip4_addr_t local_ip, uint16_t local_port) {
    for (tcp_tcb_t *c = tcb_head; c; c = c->next) {
        if (c->state == TCP_STATE_LISTEN && c->local_port == local_port) {
            if (c->local_ip == IP4_ADDR_ANY || local_ip == IP4_ADDR_ANY || c->local_ip == local_ip) {
                return c;
            }
        }
    }
    return NULL;
}

int tcp_send_segment(tcp_tcb_t *tcb, uint8_t flags, const void *data, uint16_t len,
                     uint16_t mss, uint32_t default_rto_ms, uint32_t max_rto_ms,
                     uint8_t max_retries, uint32_t time_wait_ms) {
    if (!tcb || (!data && len > 0)) return -1;
    if ((size_t)len > 65535u - sizeof(tcp_header_t) - 20u) return -1;

    if (mss) tcb->mss = mss;
    if (default_rto_ms) tcb->default_rto_ms = default_rto_ms;
    if (max_rto_ms) tcb->max_rto_ms = max_rto_ms;
    if (max_retries) tcb->max_retries = max_retries;
    if (time_wait_ms) tcb->time_wait_duration_ms = time_wait_ms;

    net_if_t *netif = net_get_interface();
    if (!netif) return -1;

    ip4_addr_t src_ip = tcb->local_ip != IP4_ADDR_ANY ? tcb->local_ip : netif->ip;
    ip4_addr_t dst_ip = tcb->remote_ip;

    uint16_t seg_len = (uint16_t)(sizeof(tcp_header_t) + len);
    uint8_t *packet = (uint8_t *)kmalloc(seg_len);
    if (!packet) return -1;

    tcp_header_t *hdr = (tcp_header_t *)packet;
    uint32_t max_hdr_wnd = (uint32_t)((1ULL << (sizeof(hdr->window_size) * 8u)) - 1ULL);
    hdr->src_port = htons(tcb->local_port);
    hdr->dst_port = htons(tcb->remote_port);
    hdr->seq_num = htonl(tcb->snd_nxt);
    hdr->ack_num = (flags & TCP_FLAG_ACK) ? htonl(tcb->rcv_nxt) : 0;
    hdr->data_offset = (sizeof(tcp_header_t) / 4) << 4;
    hdr->flags = flags;
    hdr->window_size = htons(tcb->rcv_wnd > max_hdr_wnd ? (uint16_t)max_hdr_wnd : (uint16_t)tcb->rcv_wnd);
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    if (data && len > 0) {
        memcpy(packet + sizeof(tcp_header_t), data, len);
    }

    hdr->checksum = tcp_checksum(src_ip, dst_ip, packet, seg_len);

    int res = ip4_send(dst_ip, IPPROTO_TCP, 64, packet, seg_len);
    kfree(packet);

    /* Update send sequence state */
    tcb->snd_nxt += (uint32_t)len + ((flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) ? 1u : 0u);
    tcb->last_tx_ms = timer_millis();

    return res;
}

int tcp_send_ack(tcp_tcb_t *tcb) {
    if (!tcb) return -1;
    return tcp_send_segment(tcb, TCP_FLAG_ACK, NULL, 0,
                            tcb->mss, tcb->default_rto_ms, tcb->max_rto_ms,
                            tcb->max_retries, tcb->time_wait_duration_ms);
}

int tcp_send_rst(ip4_addr_t src_ip, uint16_t src_port,
                 ip4_addr_t dst_ip, uint16_t dst_port,
                 uint32_t seq, uint32_t ack) {
    uint16_t seg_len = sizeof(tcp_header_t);
    tcp_header_t hdr;
    hdr.src_port = htons(src_port);
    hdr.dst_port = htons(dst_port);
    hdr.seq_num = htonl(seq);
    hdr.ack_num = htonl(ack);
    hdr.data_offset = (sizeof(tcp_header_t) / 4) << 4;
    hdr.flags = TCP_FLAG_RST | (ack ? TCP_FLAG_ACK : 0);
    hdr.window_size = 0;
    hdr.checksum = 0;
    hdr.urgent_ptr = 0;

    hdr.checksum = tcp_checksum(src_ip, dst_ip, &hdr, seg_len);
    return ip4_send(dst_ip, IPPROTO_TCP, 64, &hdr, seg_len);
}

int tcp_active_open(tcp_tcb_t *tcb, ip4_addr_t remote_ip, uint16_t remote_port,
                    uint16_t mss, uint32_t default_rto_ms, uint32_t max_rto_ms,
                    uint8_t max_retries, uint32_t time_wait_ms) {
    if (!tcb) return -1;

    net_if_t *netif = net_get_interface();
    if (!netif) return -1;

    dyn_buf_reset(&tcb->rx_buf);
    dyn_buf_reset(&tcb->tx_buf);

    if (tcb->local_ip == IP4_ADDR_ANY) {
        tcb->local_ip = netif->ip;
    }
    if (tcb->local_port == 0) {
        tcb->local_port = tcp_alloc_ephemeral_port();
    }

    tcb->remote_ip = remote_ip;
    tcb->remote_port = remote_port;
    tcb->iss = (timer_millis() * 1103515245u + 12345u) & 0x7FFFFFFFu;
    tcb->snd_una = tcb->iss;
    tcb->snd_nxt = tcb->iss;
    tcb->state = TCP_STATE_SYN_SENT;
    tcb->retrans_count = 0;
    tcb->mss = mss ? mss : rtl8139_get_hw_mss();
    tcb->default_rto_ms = default_rto_ms;
    tcb->max_rto_ms = max_rto_ms;
    tcb->max_retries = max_retries;
    tcb->time_wait_duration_ms = time_wait_ms;
    tcb->rto_ms = default_rto_ms;

    serial_printf("[tcp] Connecting to %x:%u from port %u, state SYN_SENT\n",
                  (unsigned int)remote_ip, remote_port, tcb->local_port);

    int res = tcp_send_segment(tcb, TCP_FLAG_SYN, NULL, 0,
                               tcb->mss, default_rto_ms, max_rto_ms, max_retries, time_wait_ms);
    if (res < 0) {
        tcb->state = TCP_STATE_CLOSED;
        return -1;
    }
    return res;
}

void tcp_close(tcp_tcb_t *tcb) {
    if (!tcb) return;

    switch (tcb->state) {
        case TCP_STATE_CLOSED:
        case TCP_STATE_LISTEN:
            tcb->state = TCP_STATE_CLOSED;
            break;
        case TCP_STATE_SYN_SENT:
            tcb->state = TCP_STATE_CLOSED;
            break;
        case TCP_STATE_SYN_RECEIVED:
        case TCP_STATE_ESTABLISHED:
            tcb->state = TCP_STATE_FIN_WAIT_1;
            tcp_send_segment(tcb, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0,
                             tcb->mss, tcb->default_rto_ms, tcb->max_rto_ms,
                             tcb->max_retries, tcb->time_wait_duration_ms);
            break;
        case TCP_STATE_CLOSE_WAIT:
            tcb->state = TCP_STATE_LAST_ACK;
            tcp_send_segment(tcb, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0,
                             tcb->mss, tcb->default_rto_ms, tcb->max_rto_ms,
                             tcb->max_retries, tcb->time_wait_duration_ms);
            break;
        default:
            break;
    }
}

void tcp_abort(tcp_tcb_t *tcb) {
    if (!tcb) return;
    tcp_state_t prev = tcb->state;
    /* Transition to CLOSED before tcp_send_rst because ARP resolution can re-enter net_poll(). */
    tcb->state = TCP_STATE_CLOSED;
    tcb->retrans_count = 0;
    if (prev != TCP_STATE_CLOSED && prev != TCP_STATE_LISTEN) {
        tcp_send_rst(tcb->local_ip, tcb->local_port, tcb->remote_ip, tcb->remote_port,
                     tcb->snd_nxt, tcb->rcv_nxt);
    }
}

void tcp_receive(ip4_addr_t src_ip, ip4_addr_t dst_ip, const void *payload, uint16_t len,
                 uint16_t mss, uint32_t default_rto_ms, uint32_t max_rto_ms,
                 uint8_t max_retries, uint32_t time_wait_ms) {
    if (!payload || len < sizeof(tcp_header_t)) {
        return;
    }

    const tcp_header_t *hdr = (const tcp_header_t *)payload;
    uint8_t data_offset = (hdr->data_offset >> 4) * 4;
    if (data_offset < sizeof(tcp_header_t) || len < data_offset) {
        return;
    }

    /* Verify TCP Checksum if calculated by peer */
    if (hdr->checksum != 0) {
        uint16_t csum = tcp_checksum(src_ip, dst_ip, payload, len);
        if (csum != 0) {
            serial_printf("[tcp] Checksum mismatch (0x%x)\n", (unsigned int)csum);
            return;
        }
    }

    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint32_t seq_num = ntohl(hdr->seq_num);
    uint32_t ack_num = ntohl(hdr->ack_num);
    uint8_t  flags = hdr->flags;
    uint16_t win = ntohs(hdr->window_size);

    const uint8_t *tcp_data = (const uint8_t *)payload + data_offset;
    uint16_t tcp_data_len = len - data_offset;

    /* Lookup active connection */
    tcp_tcb_t *tcb = tcp_find_tcb(dst_ip, dst_port, src_ip, src_port);

    /* Otherwise check listening socket */
    if (!tcb) {
        tcp_tcb_t *listener = tcp_find_listener(dst_ip, dst_port);
        if (listener) {
            if ((flags & TCP_FLAG_SYN) && !(flags & (TCP_FLAG_RST | TCP_FLAG_ACK))) {
                uint16_t c_mss = listener->mss ? listener->mss : mss;
                uint32_t c_rto = listener->default_rto_ms ? listener->default_rto_ms : default_rto_ms;
                uint32_t c_max_rto = listener->max_rto_ms ? listener->max_rto_ms : max_rto_ms;
                uint8_t  c_retries = listener->max_retries ? listener->max_retries : max_retries;
                uint32_t c_tw = listener->time_wait_duration_ms ? listener->time_wait_duration_ms : time_wait_ms;

                tcp_tcb_t *child = tcp_alloc_tcb(c_mss, c_rto, c_max_rto, c_retries, c_tw);
                if (!child) return;

                child->state = TCP_STATE_SYN_RECEIVED;
                child->local_ip = dst_ip;
                child->local_port = dst_port;
                child->remote_ip = src_ip;
                child->remote_port = src_port;
                child->irs = seq_num;
                child->rcv_nxt = seq_num + 1;
                child->iss = (timer_millis() * 1103515245u + 54321u) & 0x7FFFFFFFu;
                child->snd_nxt = child->iss;
                child->snd_una = child->iss;
                child->snd_wnd = win ? win : child->max_rcv_wnd;
                child->parent_listener = listener;
                child->is_passive = true;

                serial_printf("[tcp] Listener port %u: SYN from %x:%u -> SYN_RECEIVED\n",
                              dst_port, (unsigned int)src_ip, src_port);

                tcp_send_segment(child, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0,
                                 c_mss, c_rto, c_max_rto, c_retries, c_tw);
                return;
            }
        }

        if (!(flags & TCP_FLAG_RST)) {
            uint32_t rst_seq = (flags & TCP_FLAG_ACK) ? ack_num : 0;
            uint32_t rst_ack = seq_num + tcp_data_len + ((flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) ? 1 : 0);
            tcp_send_rst(dst_ip, dst_port, src_ip, src_port, rst_seq, rst_ack);
        }
        return;
    }

    tcb->snd_wnd = win;

    switch (tcb->state) {
        case TCP_STATE_SYN_SENT:
            if (flags & TCP_FLAG_RST) {
                tcb->state = TCP_STATE_CLOSED;
                return;
            }
            if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                if (ack_num == tcb->snd_nxt) {
                    tcb->snd_una = ack_num;
                    tcb->irs = seq_num;
                    tcb->rcv_nxt = seq_num + 1;
                    tcb->state = TCP_STATE_ESTABLISHED;
                    tcb->retrans_count = 0;
                    serial_printf("[tcp] Connection established to %x:%u\n",
                                  (unsigned int)tcb->remote_ip, tcb->remote_port);
                    tcp_send_ack(tcb);
                }
            }
            break;

        case TCP_STATE_SYN_RECEIVED:
            if (flags & TCP_FLAG_RST) {
                tcb->state = TCP_STATE_CLOSED;
                return;
            }
            if (flags & TCP_FLAG_SYN) {
                tcb->snd_nxt = tcb->iss;
                tcp_send_segment(tcb, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0,
                                 tcb->mss, tcb->default_rto_ms, tcb->max_rto_ms,
                                 tcb->max_retries, tcb->time_wait_duration_ms);
                return;
            }
            if (flags & TCP_FLAG_ACK) {
                if (ack_num == tcb->snd_nxt) {
                    if (!tcb->parent_listener) {
                        tcp_abort(tcb);
                        return;
                    }
                    tcb->snd_una = ack_num;
                    tcb->state = TCP_STATE_ESTABLISHED;
                    tcb->retrans_count = 0;
                    serial_printf("[tcp] Handshake complete: %x:%u -> ESTABLISHED\n",
                                  (unsigned int)tcb->remote_ip, tcb->remote_port);

                    tcb->next_pending = tcb->parent_listener->next_pending;
                    tcb->parent_listener->next_pending = tcb;
                }
            }
            if (tcb->state != TCP_STATE_ESTABLISHED) break;
            /* fall through */

        case TCP_STATE_ESTABLISHED:
            if (flags & TCP_FLAG_RST) {
                serial_printf("[tcp] Received RST, closing\n");
                tcb->state = TCP_STATE_CLOSED;
                return;
            }

            if ((flags & TCP_FLAG_ACK) &&
                TCP_SEQ_GT(ack_num, tcb->snd_una) &&
                TCP_SEQ_LEQ(ack_num, tcb->snd_nxt)) {
                size_t bytes_acked = (size_t)(ack_num - tcb->snd_una);
                dyn_buf_consume(&tcb->tx_buf, NULL, bytes_acked);
                tcb->snd_una = ack_num;
                tcb->retrans_count = 0;
                tcb->rto_ms = tcb->default_rto_ms ? tcb->default_rto_ms : default_rto_ms;
            }

            if (tcp_data_len > 0) {
                if (seq_num == tcb->rcv_nxt) {
                    if (dyn_buf_append(&tcb->rx_buf, tcp_data, tcp_data_len)) {
                        tcb->rcv_nxt += tcp_data_len;
                        uint32_t max_wnd = tcb->max_rcv_wnd ? tcb->max_rcv_wnd : rtl8139_get_hw_max_window();
                        if (tcb->rx_buf.size < max_wnd) {
                            tcb->rcv_wnd = max_wnd - (uint32_t)tcb->rx_buf.size;
                        } else {
                            tcb->rcv_wnd = 0;
                        }
                    }
                    tcp_send_ack(tcb);
                } else {
                    tcp_send_ack(tcb);
                }
            }

            if ((flags & TCP_FLAG_FIN) && (seq_num + tcp_data_len) == tcb->rcv_nxt) {
                tcb->rcv_nxt++;
                tcp_send_ack(tcb);
                tcb->state = TCP_STATE_CLOSE_WAIT;
                serial_printf("[tcp] Peer sent FIN -> state CLOSE_WAIT\n");
            }
            break;

        case TCP_STATE_FIN_WAIT_1:
            if (flags & TCP_FLAG_RST) {
                tcb->state = TCP_STATE_CLOSED;
                return;
            }
            if ((flags & TCP_FLAG_ACK) && ack_num == tcb->snd_nxt) {
                tcb->snd_una = ack_num;
                tcb->state = TCP_STATE_FIN_WAIT_2;
                tcb->last_tx_ms = timer_millis();
            }
            if (tcp_data_len > 0 && seq_num == tcb->rcv_nxt) {
                tcb->rcv_nxt += tcp_data_len;
                if (!(flags & TCP_FLAG_FIN)) {
                    tcp_send_ack(tcb);
                }
            }
            if ((flags & TCP_FLAG_FIN) && (seq_num + tcp_data_len) == tcb->rcv_nxt) {
                tcb->rcv_nxt++;
                tcp_send_ack(tcb);
                if (tcb->state == TCP_STATE_FIN_WAIT_2) {
                    tcb->state = TCP_STATE_TIME_WAIT;
                    tcb->time_wait_ms = timer_millis();
                } else {
                    tcb->state = TCP_STATE_CLOSING;
                }
            }
            break;

        case TCP_STATE_FIN_WAIT_2:
            if (flags & TCP_FLAG_RST) {
                tcb->state = TCP_STATE_CLOSED;
                return;
            }
            if (tcp_data_len > 0 && seq_num == tcb->rcv_nxt) {
                tcb->rcv_nxt += tcp_data_len;
                if (!(flags & TCP_FLAG_FIN)) {
                    tcp_send_ack(tcb);
                }
            }
            if ((flags & TCP_FLAG_FIN) && (seq_num + tcp_data_len) == tcb->rcv_nxt) {
                tcb->rcv_nxt++;
                tcp_send_ack(tcb);
                tcb->state = TCP_STATE_TIME_WAIT;
                tcb->time_wait_ms = timer_millis();
                serial_printf("[tcp] Received FIN -> state TIME_WAIT\n");
            }
            break;

        case TCP_STATE_CLOSE_WAIT:
            if (flags & TCP_FLAG_RST) {
                tcb->state = TCP_STATE_CLOSED;
            }
            break;

        case TCP_STATE_CLOSING:
            if (flags & TCP_FLAG_RST) {
                tcb->state = TCP_STATE_CLOSED;
                return;
            }
            if ((flags & TCP_FLAG_ACK) && ack_num == tcb->snd_nxt) {
                tcb->snd_una = ack_num;
                tcb->state = TCP_STATE_TIME_WAIT;
                tcb->time_wait_ms = timer_millis();
            }
            break;

        case TCP_STATE_LAST_ACK:
            if (flags & TCP_FLAG_RST) {
                tcb->state = TCP_STATE_CLOSED;
                return;
            }
            if ((flags & TCP_FLAG_ACK) && ack_num == tcb->snd_nxt) {
                tcb->snd_una = ack_num;
                tcb->state = TCP_STATE_CLOSED;
                serial_printf("[tcp] LAST_ACK acknowledged -> state CLOSED\n");
            }
            break;

        case TCP_STATE_TIME_WAIT:
            if (flags & TCP_FLAG_FIN) {
                tcp_send_ack(tcb);
            }
            break;

        default:
            break;
    }
}

void tcp_poll(uint16_t mss, uint32_t default_rto_ms, uint32_t max_rto_ms,
              uint8_t max_retries, uint32_t time_wait_ms) {
    static bool s_in_tcp_poll = false;
    if (s_in_tcp_poll) return;
    s_in_tcp_poll = true;

    uint32_t now = timer_millis();
    tcp_tcb_t *curr = tcb_head;

    while (curr) {
        tcp_tcb_t *next = curr->next;
        bool freed_with_pending = false;

        uint16_t cur_mss = curr->mss ? curr->mss : (mss ? mss : rtl8139_get_hw_mss());
        uint32_t cur_def_rto = curr->default_rto_ms ? curr->default_rto_ms : default_rto_ms;
        uint32_t cur_max_rto = curr->max_rto_ms ? curr->max_rto_ms : max_rto_ms;
        uint8_t  cur_max_retries = curr->max_retries ? curr->max_retries : max_retries;
        uint32_t cur_tw_ms = curr->time_wait_duration_ms ? curr->time_wait_duration_ms : time_wait_ms;

        if (curr->state == TCP_STATE_CLOSED) {
            if (!curr->sock) {
                freed_with_pending = (curr->next_pending != NULL);
                tcp_free_tcb(curr);
            }
        } else if (curr->is_passive && !curr->parent_listener && !curr->sock &&
                   curr->state == TCP_STATE_SYN_RECEIVED) {
            tcp_abort(curr);
            tcp_free_tcb(curr);
        } else if (curr->state == TCP_STATE_FIN_WAIT_2 && !curr->sock &&
                   (now - curr->last_tx_ms >= cur_tw_ms)) {
            curr->state = TCP_STATE_CLOSED;
            tcp_free_tcb(curr);
        } else if (curr->state == TCP_STATE_TIME_WAIT) {
            if (now - curr->time_wait_ms >= cur_tw_ms) {
                curr->state = TCP_STATE_CLOSED;
                if (!curr->sock) {
                    tcp_free_tcb(curr);
                }
            }
        } else if (curr->state == TCP_STATE_SYN_SENT ||
                   curr->state == TCP_STATE_SYN_RECEIVED ||
                   curr->state == TCP_STATE_ESTABLISHED ||
                   curr->state == TCP_STATE_FIN_WAIT_1 ||
                   curr->state == TCP_STATE_LAST_ACK) {
            bool has_unacked = TCP_SEQ_GT(curr->snd_nxt, curr->snd_una) || (curr->tx_buf.size > 0);
            if (has_unacked && (now - curr->last_tx_ms >= curr->rto_ms)) {
                if (curr->retrans_count >= cur_max_retries) {
                    serial_printf("[tcp] Connection timed out after %u retries, aborting\n",
                                  curr->retrans_count);
                    tcp_abort(curr);
                    if (!curr->sock) {
                        tcp_free_tcb(curr);
                    }
                } else {
                    curr->retrans_count++;
                    curr->rto_ms *= 2;
                    if (curr->rto_ms > cur_max_rto) curr->rto_ms = cur_max_rto;
                    curr->last_tx_ms = now;

                    serial_printf("[tcp] RTO fired (retry %u, rto=%ums, state=%s)\n",
                                  curr->retrans_count, curr->rto_ms, tcp_state_to_str(curr->state));

                    if (curr->state == TCP_STATE_SYN_SENT) {
                        curr->snd_nxt = curr->iss;
                        tcp_send_segment(curr, TCP_FLAG_SYN, NULL, 0,
                                         cur_mss, cur_def_rto, cur_max_rto, cur_max_retries, cur_tw_ms);
                    } else if (curr->state == TCP_STATE_SYN_RECEIVED) {
                        curr->snd_nxt = curr->iss;
                        tcp_send_segment(curr, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0,
                                         cur_mss, cur_def_rto, cur_max_rto, cur_max_retries, cur_tw_ms);
                    } else if (curr->tx_buf.size > 0) {
                        size_t chunk = curr->tx_buf.size < cur_mss ? curr->tx_buf.size : cur_mss;
                        curr->snd_nxt = curr->snd_una;
                        tcp_send_segment(curr, TCP_FLAG_ACK | TCP_FLAG_PSH, curr->tx_buf.data, (uint16_t)chunk,
                                         cur_mss, cur_def_rto, cur_max_rto, cur_max_retries, cur_tw_ms);
                    } else if (curr->state == TCP_STATE_FIN_WAIT_1 || curr->state == TCP_STATE_LAST_ACK) {
                        curr->snd_nxt = curr->snd_una;
                        tcp_send_segment(curr, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0,
                                         cur_mss, cur_def_rto, cur_max_rto, cur_max_retries, cur_tw_ms);
                    }
                }
            }
        }

        curr = freed_with_pending ? tcb_head : next;
    }

    s_in_tcp_poll = false;
}

