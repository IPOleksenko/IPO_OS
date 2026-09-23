#include <net/tcp.h>
#include <net/ipv4.h>
#include <net/net.h>
#include <system/timer.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <stdio.h>

#define TCP_MSS             1460u
#define TCP_DEFAULT_RTO_MS  1000u
#define TCP_MAX_RTO_MS      8000u
#define TCP_MAX_RETRIES     5
#define TCP_TIME_WAIT_MS    2000u

static tcp_tcb_t *tcb_head = NULL;
static uint32_t   next_tcb_id = 1;
static uint16_t   next_ephemeral_port = 49152;

static const char *tcp_state_names[] = {
    "CLOSED", "LISTEN", "SYN_SENT", "SYN_RECEIVED", "ESTABLISHED",
    "FIN_WAIT_1", "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT"
};

const char *tcp_state_to_str(tcp_state_t state) {
    if ((int)state >= 0 && state <= TCP_STATE_TIME_WAIT) {
        return tcp_state_names[state];
    }
    return "UNKNOWN";
}

void tcp_init(void) {
    tcb_head = NULL;
    next_tcb_id = 1;
    next_ephemeral_port = 49152;
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

tcp_tcb_t *tcp_alloc_tcb(void) {
    tcp_tcb_t *tcb = (tcp_tcb_t *)kmalloc(sizeof(tcp_tcb_t));
    if (!tcb) return NULL;

    memset(tcb, 0, sizeof(tcp_tcb_t));
    tcb->id = next_tcb_id++;
    tcb->state = TCP_STATE_CLOSED;
    tcb->rto_ms = TCP_DEFAULT_RTO_MS;
    tcb->rcv_wnd = 65535;
    tcb->snd_wnd = 65535;

    dyn_buf_init(&tcb->rx_buf, 2048);
    dyn_buf_init(&tcb->tx_buf, 2048);

    /* Link into global list */
    tcb->next = tcb_head;
    tcb_head = tcb;

    return tcb;
}

void tcp_free_tcb(tcp_tcb_t *tcb) {
    if (!tcb) return;

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
        if (c->state == TCP_STATE_LISTEN) continue;

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

int tcp_send_segment(tcp_tcb_t *tcb, uint8_t flags, const void *data, uint16_t len) {
    if (!tcb) return -1;

    net_if_t *netif = net_get_interface();
    if (!netif) return -1;

    ip4_addr_t src_ip = tcb->local_ip != IP4_ADDR_ANY ? tcb->local_ip : netif->ip;
    ip4_addr_t dst_ip = tcb->remote_ip;

    uint16_t seg_len = (uint16_t)(sizeof(tcp_header_t) + len);
    uint8_t *packet = (uint8_t *)kmalloc(seg_len);
    if (!packet) return -1;

    tcp_header_t *hdr = (tcp_header_t *)packet;
    hdr->src_port = htons(tcb->local_port);
    hdr->dst_port = htons(tcb->remote_port);
    hdr->seq_num = htonl(tcb->snd_nxt);
    hdr->ack_num = (flags & TCP_FLAG_ACK) ? htonl(tcb->rcv_nxt) : 0;
    hdr->data_offset = (sizeof(tcp_header_t) / 4) << 4;
    hdr->flags = flags;
    hdr->window_size = htons(tcb->rcv_wnd > 65535 ? 65535 : (uint16_t)tcb->rcv_wnd);
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    if (data && len > 0) {
        memcpy(packet + sizeof(tcp_header_t), data, len);
    }

    hdr->checksum = tcp_checksum(src_ip, dst_ip, packet, seg_len);

    int res = ip4_send(dst_ip, IPPROTO_TCP, 64, packet, seg_len);
    kfree(packet);

    /* Update send sequence state */
    if (flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) {
        tcb->snd_nxt++;
    } else {
        tcb->snd_nxt += len;
    }
    tcb->last_tx_ms = timer_millis();

    return res;
}

int tcp_send_ack(tcp_tcb_t *tcb) {
    if (!tcb) return -1;
    return tcp_send_segment(tcb, TCP_FLAG_ACK, NULL, 0);
}

int tcp_send_rst(ip4_addr_t src_ip, uint16_t src_port,
                 ip4_addr_t dst_ip, uint16_t dst_port,
                 uint32_t seq, uint32_t ack) {
    uint16_t seg_len = sizeof(tcp_header_t);
    uint8_t packet[sizeof(tcp_header_t)];

    tcp_header_t *hdr = (tcp_header_t *)packet;
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->seq_num = htonl(seq);
    hdr->ack_num = htonl(ack);
    hdr->data_offset = (sizeof(tcp_header_t) / 4) << 4;
    hdr->flags = TCP_FLAG_RST | (ack ? TCP_FLAG_ACK : 0);
    hdr->window_size = 0;
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    hdr->checksum = tcp_checksum(src_ip, dst_ip, packet, seg_len);
    return ip4_send(dst_ip, IPPROTO_TCP, 64, packet, seg_len);
}

int tcp_active_open(tcp_tcb_t *tcb, ip4_addr_t remote_ip, uint16_t remote_port) {
    if (!tcb) return -1;

    net_if_t *netif = net_get_interface();
    if (!netif) return -1;

    if (tcb->local_ip == IP4_ADDR_ANY) {
        tcb->local_ip = netif->ip;
    }
    if (tcb->local_port == 0) {
        tcb->local_port = next_ephemeral_port++;
        if (next_ephemeral_port > 65500) next_ephemeral_port = 49152;
    }

    tcb->remote_ip = remote_ip;
    tcb->remote_port = remote_port;
    tcb->iss = (timer_millis() * 1103515245u + 12345u) & 0x7FFFFFFFu;
    tcb->snd_una = tcb->iss;
    tcb->snd_nxt = tcb->iss;
    tcb->state = TCP_STATE_SYN_SENT;
    tcb->retrans_count = 0;
    tcb->rto_ms = TCP_DEFAULT_RTO_MS;

    serial_printf("[tcp] Connecting to %x:%u from port %u, state SYN_SENT\n",
                  (unsigned int)remote_ip, remote_port, tcb->local_port);

    return tcp_send_segment(tcb, TCP_FLAG_SYN, NULL, 0);
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
            tcp_send_segment(tcb, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            break;
        case TCP_STATE_CLOSE_WAIT:
            tcb->state = TCP_STATE_LAST_ACK;
            tcp_send_segment(tcb, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            break;
        default:
            break;
    }
}

void tcp_abort(tcp_tcb_t *tcb) {
    if (!tcb) return;
    if (tcb->state != TCP_STATE_CLOSED && tcb->state != TCP_STATE_LISTEN) {
        tcp_send_rst(tcb->local_ip, tcb->local_port, tcb->remote_ip, tcb->remote_port,
                     tcb->snd_nxt, tcb->rcv_nxt);
    }
    tcb->state = TCP_STATE_CLOSED;
}

void tcp_receive(ip4_addr_t src_ip, ip4_addr_t dst_ip, const void *payload, uint16_t len) {
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

    /* 1. Lookup established connection */
    tcp_tcb_t *tcb = tcp_find_tcb(dst_ip, dst_port, src_ip, src_port);

    /* 2. If no connection, check for listening socket */
    if (!tcb) {
        tcp_tcb_t *listener = tcp_find_listener(dst_ip, dst_port);
        if (listener) {
            /* Passive connection request (LISTEN) */
            if (flags & TCP_FLAG_SYN) {
                tcp_tcb_t *child = tcp_alloc_tcb();
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
                child->snd_wnd = win ? win : 65535;
                child->parent_listener = listener;
                child->is_passive = true;

                serial_printf("[tcp] Listener port %u: SYN from %x:%u -> SYN_RECEIVED\n",
                              dst_port, (unsigned int)src_ip, src_port);

                /* Send SYN + ACK (RFC 793 / RFC 9293) */
                tcp_send_segment(child, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
                return;
            }
        }

        /* Not found and not SYN for listener -> Send RST */
        if (!(flags & TCP_FLAG_RST)) {
            uint32_t rst_seq = (flags & TCP_FLAG_ACK) ? ack_num : 0;
            uint32_t rst_ack = seq_num + tcp_data_len + ((flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) ? 1 : 0);
            tcp_send_rst(dst_ip, dst_port, src_ip, src_port, rst_seq, rst_ack);
        }
        return;
    }

    /* Update send window */
    tcb->snd_wnd = win;

    /* State Machine Dispatch (RFC 793 / RFC 9293) */
    switch (tcb->state) {
        case TCP_STATE_SYN_SENT:
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
            } else if (flags & TCP_FLAG_RST) {
                tcb->state = TCP_STATE_CLOSED;
            }
            break;

        case TCP_STATE_SYN_RECEIVED:
            if (flags & TCP_FLAG_ACK) {
                if (ack_num == tcb->snd_nxt) {
                    tcb->snd_una = ack_num;
                    tcb->state = TCP_STATE_ESTABLISHED;
                    tcb->retrans_count = 0;
                    serial_printf("[tcp] Handshake complete: %x:%u -> ESTABLISHED\n",
                                  (unsigned int)tcb->remote_ip, tcb->remote_port);

                    /* Add to parent listener's completed connection queue */
                    if (tcb->parent_listener) {
                        tcb->next_pending = tcb->parent_listener->next_pending;
                        tcb->parent_listener->next_pending = tcb;
                    }
                }
            }
            /* Fall through to process any data attached to final ACK */
            if (tcb->state != TCP_STATE_ESTABLISHED) break;

        case TCP_STATE_ESTABLISHED:
            if (flags & TCP_FLAG_RST) {
                serial_printf("[tcp] Received RST, closing\n");
                tcb->state = TCP_STATE_CLOSED;
                return;
            }

            /* Process ACK */
            if (flags & TCP_FLAG_ACK) {
                if (ack_num > tcb->snd_una && ack_num <= tcb->snd_nxt) {
                    size_t bytes_acked = ack_num - tcb->snd_una;
                    dyn_buf_consume(&tcb->tx_buf, NULL, bytes_acked);
                    tcb->snd_una = ack_num;
                    tcb->retrans_count = 0;
                    tcb->rto_ms = TCP_DEFAULT_RTO_MS;
                }
            }

            /* Process incoming data payload */
            if (tcp_data_len > 0) {
                if (seq_num == tcb->rcv_nxt) {
                    /* In-order data: append to self-expanding dynamic buffer */
                    dyn_buf_append(&tcb->rx_buf, tcp_data, tcp_data_len);
                    tcb->rcv_nxt += tcp_data_len;
                    if (tcb->rx_buf.size < 65535) {
                        tcb->rcv_wnd = 65535 - tcb->rx_buf.size;
                    } else {
                        tcb->rcv_wnd = 0; /* Flow control: close window to pause sender */
                    }
                    tcp_send_ack(tcb);
                } else if (seq_num < tcb->rcv_nxt) {
                    /* Duplicate data, re-ACK */
                    tcp_send_ack(tcb);
                }
            }

            /* Process FIN */
            if (flags & TCP_FLAG_FIN) {
                tcb->rcv_nxt++;
                tcp_send_ack(tcb);
                tcb->state = TCP_STATE_CLOSE_WAIT;
                serial_printf("[tcp] Peer sent FIN -> state CLOSE_WAIT\n");
            }
            break;

        case TCP_STATE_FIN_WAIT_1:
            if (flags & TCP_FLAG_ACK) {
                if (ack_num == tcb->snd_nxt) {
                    tcb->snd_una = ack_num;
                    tcb->state = TCP_STATE_FIN_WAIT_2;
                }
            }
            if (flags & TCP_FLAG_FIN) {
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
            if (flags & TCP_FLAG_FIN) {
                tcb->rcv_nxt++;
                tcp_send_ack(tcb);
                tcb->state = TCP_STATE_TIME_WAIT;
                tcb->time_wait_ms = timer_millis();
                serial_printf("[tcp] Received FIN -> state TIME_WAIT\n");
            }
            break;

        case TCP_STATE_CLOSE_WAIT:
            /* Waiting for application to close */
            break;

        case TCP_STATE_CLOSING:
            if (flags & TCP_FLAG_ACK) {
                tcb->state = TCP_STATE_TIME_WAIT;
                tcb->time_wait_ms = timer_millis();
            }
            break;

        case TCP_STATE_LAST_ACK:
            if (flags & TCP_FLAG_ACK) {
                tcb->state = TCP_STATE_CLOSED;
                serial_printf("[tcp] LAST_ACK acknowledged -> state CLOSED\n");
            }
            break;

        case TCP_STATE_TIME_WAIT:
            /* Re-acknowledge any stray FIN */
            if (flags & TCP_FLAG_FIN) {
                tcp_send_ack(tcb);
            }
            break;

        default:
            break;
    }
}

void tcp_poll(void) {
    uint32_t now = timer_millis();
    tcp_tcb_t *curr = tcb_head;

    while (curr) {
        tcp_tcb_t *next = curr->next;

        /* 1. Handle TIME_WAIT expiry */
        if (curr->state == TCP_STATE_TIME_WAIT) {
            if (now - curr->time_wait_ms >= TCP_TIME_WAIT_MS) {
                curr->state = TCP_STATE_CLOSED;
                if (!curr->sock) {
                    tcp_free_tcb(curr);
                }
            }
        }
        /* 2. Handle Retransmission Timeout (RTO) for unacked data / SYNs */
        else if (curr->state == TCP_STATE_SYN_SENT ||
                 curr->state == TCP_STATE_SYN_RECEIVED ||
                 curr->state == TCP_STATE_ESTABLISHED ||
                 curr->state == TCP_STATE_FIN_WAIT_1 ||
                 curr->state == TCP_STATE_LAST_ACK) {

            bool has_unacked = (curr->snd_nxt > curr->snd_una) || (curr->tx_buf.size > 0);
            if (has_unacked && (now - curr->last_tx_ms >= curr->rto_ms)) {
                if (curr->retrans_count >= TCP_MAX_RETRIES) {
                    serial_printf("[tcp] Connection timed out after %u retries, aborting\n",
                                  curr->retrans_count);
                    tcp_abort(curr);
                } else {
                    curr->retrans_count++;
                    curr->rto_ms *= 2; /* Exponential backoff */
                    if (curr->rto_ms > TCP_MAX_RTO_MS) curr->rto_ms = TCP_MAX_RTO_MS;

                    serial_printf("[tcp] RTO fired (retry %u, rto=%ums, state=%s)\n",
                                  curr->retrans_count, curr->rto_ms, tcp_state_to_str(curr->state));

                    if (curr->state == TCP_STATE_SYN_SENT) {
                        curr->snd_nxt = curr->iss;
                        tcp_send_segment(curr, TCP_FLAG_SYN, NULL, 0);
                    } else if (curr->state == TCP_STATE_SYN_RECEIVED) {
                        curr->snd_nxt = curr->iss;
                        tcp_send_segment(curr, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
                    } else if (curr->tx_buf.size > 0) {
                        /* Retransmit pending payload chunk from tx_buf */
                        size_t chunk = curr->tx_buf.size < TCP_MSS ? curr->tx_buf.size : TCP_MSS;
                        curr->snd_nxt = curr->snd_una;
                        tcp_send_segment(curr, TCP_FLAG_ACK | TCP_FLAG_PSH, curr->tx_buf.data, (uint16_t)chunk);
                    } else if (curr->state == TCP_STATE_FIN_WAIT_1 || curr->state == TCP_STATE_LAST_ACK) {
                        tcp_send_segment(curr, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
                    }
                }
            }
        }

        curr = next;
    }
}

