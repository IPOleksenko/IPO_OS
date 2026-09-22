#ifndef IPO_NET_TCP_H
#define IPO_NET_TCP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <net/net.h>
#include <net/dyn_buf.h>

/* TCP Protocol Number (RFC 790 / RFC 791) */
#define IPPROTO_TCP 6

/* TCP Flags (RFC 793, RFC 9293) */
#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20
#define TCP_FLAG_ECE 0x40
#define TCP_FLAG_CWR 0x80

/* Standard TCP Header (RFC 793 / RFC 9293) */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset; /* Higher 4 bits: header size in 32-bit words */
    uint8_t  flags;       /* TCP Flags */
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed)) tcp_header_t;

/* IPv4 Pseudo-Header for TCP Checksum calculation (RFC 793 Section 3.1) */
typedef struct {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t tcp_length;
} __attribute__((packed)) tcp_pseudo_header_t;

/* TCP Finite State Machine States (RFC 793 / RFC 9293) */
typedef enum {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_LISTEN,
    TCP_STATE_SYN_SENT,
    TCP_STATE_SYN_RECEIVED,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_CLOSING,
    TCP_STATE_LAST_ACK,
    TCP_STATE_TIME_WAIT
} tcp_state_t;

/* Forward declaration for Socket link */
struct socket;

/* Transmission Control Block (TCB) */
typedef struct tcp_tcb {
    uint32_t       id;
    tcp_state_t    state;

    ip4_addr_t     local_ip;
    uint16_t       local_port;
    ip4_addr_t     remote_ip;
    uint16_t       remote_port;

    /* Send Sequence Variables (RFC 793) */
    uint32_t       snd_una;       /* Oldest unacknowledged sequence number */
    uint32_t       snd_nxt;       /* Next sequence number to be sent */
    uint32_t       snd_wnd;       /* Remote receive window */
    uint32_t       iss;           /* Initial send sequence number */

    /* Receive Sequence Variables (RFC 793) */
    uint32_t       rcv_nxt;       /* Next expected incoming sequence number */
    uint32_t       rcv_wnd;       /* Advertised receive window */
    uint32_t       irs;           /* Initial receive sequence number */

    /* Self-expanding dynamic streaming buffers */
    dyn_buf_t      rx_buf;        /* Received payload stream available for socket recv() */
    dyn_buf_t      tx_buf;        /* Queued data awaiting transmission or ACK */

    /* Retransmission & Timeout Timers (RFC 6298) */
    uint32_t       rto_ms;        /* Retransmission Timeout in milliseconds */
    uint32_t       last_tx_ms;    /* Timestamp of last transmitted segment */
    uint8_t        retrans_count; /* Number of retries for current unacked data */
    uint32_t       time_wait_ms;  /* TIME_WAIT entry timestamp */

    /* Backlog & Connection Management */
    bool           is_passive;
    struct tcp_tcb *parent_listener;
    struct tcp_tcb *next_pending;

    /* Associated high-level socket descriptor reference */
    struct socket  *sock;

    struct tcp_tcb *next;         /* Global linked list of active TCBs */
} tcp_tcb_t;

/* Core TCP functions */
void        tcp_init(void);
void        tcp_poll(void);
void        tcp_receive(ip4_addr_t src_ip, ip4_addr_t dst_ip, const void *payload, uint16_t len);

tcp_tcb_t  *tcp_alloc_tcb(void);
void        tcp_free_tcb(tcp_tcb_t *tcb);
tcp_tcb_t  *tcp_find_tcb(ip4_addr_t local_ip, uint16_t local_port,
                          ip4_addr_t remote_ip, uint16_t remote_port);
tcp_tcb_t  *tcp_find_listener(ip4_addr_t local_ip, uint16_t local_port);

int         tcp_send_segment(tcp_tcb_t *tcb, uint8_t flags, const void *data, uint16_t len);
int         tcp_send_ack(tcp_tcb_t *tcb);
int         tcp_send_rst(ip4_addr_t src_ip, uint16_t src_port,
                         ip4_addr_t dst_ip, uint16_t dst_port,
                         uint32_t seq, uint32_t ack);

int         tcp_active_open(tcp_tcb_t *tcb, ip4_addr_t remote_ip, uint16_t remote_port);
void        tcp_close(tcp_tcb_t *tcb);
void        tcp_abort(tcp_tcb_t *tcb);

uint16_t    tcp_checksum(ip4_addr_t src_ip, ip4_addr_t dst_ip, const void *tcp_seg, uint16_t seg_len);
const char *tcp_state_to_str(tcp_state_t state);

#endif /* IPO_NET_TCP_H */

