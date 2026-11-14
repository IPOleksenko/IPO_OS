#include <net/socket.h>
#include <net/tcp.h>
#include <net/dns.h>
#include <net/net.h>
#include <driver/net/rtl8139.h>
#include <system/timer.h>
#include <system/state.h>
#include <driver/input/keyboard.h>
#include <driver/input/keymap/keymap.h>
#include <vga.h>
#include <wm.h>
#include <memory/kmalloc.h>
#include <ioport.h>
#include <syscall.h>
#include <string.h>
#include <stdio.h>

static socket_t **sock_table = NULL;
static size_t     sock_table_cap = 0;

void socket_subsystem_init(void) {
    if (!sock_table) {
        sock_table_cap = rtl8139_get_hw_socket_capacity();
        if (sock_table_cap == 0) sock_table_cap = 1;
        sock_table = (socket_t **)kmalloc(sizeof(socket_t *) * sock_table_cap);
        if (sock_table) {
            memset(sock_table, 0, sizeof(socket_t *) * sock_table_cap);
        }
    }
}

static bool socket_poll_interrupted(void) {
    if (system_is_interrupted()) {
        return true;
    }
    if (!keyboard_is_app_input_mode() && !wm_session_active() && !vga_is_graphics_mode()) {
        keyboard_poll();
        uint8_t sc = keyboard_get_scancode();
        if (sc == 0x01 || sc == 0x10 || (sc == 0x2E && keyboard_is_ctrl_pressed())) {
            system_request_interrupt();
            return true;
        }
    }
    return false;
}

static socket_t *alloc_socket(void) {
    socket_subsystem_init();
    if (!sock_table) return NULL;

    size_t idx = sock_table_cap;
    for (size_t i = 0; i < sock_table_cap; i++) {
        if (!sock_table[i]) {
            idx = i;
            break;
        }
    }

    if (idx == sock_table_cap) {
        if (sock_table_cap > SIZE_MAX / (2u * sizeof(socket_t *))) return NULL;
        size_t new_cap = sock_table_cap * 2;
        socket_t **new_table = (socket_t **)krealloc(sock_table, sizeof(socket_t *) * new_cap);
        if (!new_table) return NULL;

        memset(new_table + sock_table_cap, 0, sizeof(socket_t *) * (new_cap - sock_table_cap));
        sock_table = new_table;
        sock_table_cap = new_cap;
    }

    socket_t *s = (socket_t *)kmalloc(sizeof(socket_t));
    if (!s) return NULL;
    memset(s, 0, sizeof(socket_t));
    s->fd = (int)(idx + 1);
    s->in_use = true;
    dyn_str_init(&s->remote_endpoint_str, NULL);
    sock_table[idx] = s;
    return s;
}

static socket_t *get_socket(int sockfd) {
    if (sockfd <= 0 || !sock_table) return NULL;
    size_t idx = (size_t)(sockfd - 1);
    if (idx >= sock_table_cap) return NULL;
    socket_t *s = sock_table[idx];
    if (s && s->in_use) return s;
    return NULL;
}

int socket(int domain, int type, int protocol) {
    if (domain != AF_INET && domain != AF_UNSPEC) {
        return -1;
    }
    if (type != SOCK_STREAM) {
        return -1;
    }

    socket_t *s = alloc_socket();
    if (!s) return -1;

    s->domain = domain;
    s->type = type;
    s->protocol = protocol ? protocol : IPPROTO_TCP;
    s->bound_ip = IP4_ADDR_ANY;
    s->bound_port = 0;
    s->is_listening = false;
    s->tcb = NULL;

    return s->fd;
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    socket_t *s = get_socket(sockfd);
    if (!s || !addr || addrlen < sizeof(struct sockaddr_in)) {
        return -1;
    }

    const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
    s->bound_ip = ntohl(in->sin_addr.s_addr);
    s->bound_port = ntohs(in->sin_port);

    /* If port is 0, assign dynamic ephemeral port */
    if (s->bound_port == 0) {
        s->bound_port = tcp_alloc_ephemeral_port();
    }

    serial_printf("[socket] Bound socket %d to IP %x, port %u\n",
                  sockfd, (unsigned int)s->bound_ip, s->bound_port);
    return 0;
}

int listen(int sockfd, int backlog) {
    socket_t *s = get_socket(sockfd);
    if (!s) return -1;
    if (s->tcb && s->tcb->state != TCP_STATE_CLOSED && s->tcb->state != TCP_STATE_LISTEN) {
        return -1;
    }

    uint16_t hw_mss = rtl8139_get_hw_mss();

    /* Allocate or re-use TCB */
    if (!s->tcb) {
        s->tcb = tcp_alloc_tcb(hw_mss, 1000u, 8000u, 5u, 2000u);
        if (!s->tcb) return -1;
    }

    s->tcb->local_ip = s->bound_ip;   /* Can be 0.0.0.0 (INADDR_ANY) */
    s->tcb->local_port = s->bound_port;
    s->tcb->remote_ip = IP4_ADDR_ANY;
    s->tcb->remote_port = 0;
    s->tcb->state = TCP_STATE_LISTEN;
    s->tcb->sock = s;
    s->tcb->is_passive = true;
    s->tcb->next_pending = NULL;

    s->is_listening = true;
    s->backlog_limit = backlog > 0 ? backlog : 5;

    serial_printf("[socket] Socket %d listening on port %u (accepts any client IP)\n",
                  sockfd, s->bound_port);
    return 0;
}

int accept_timeout(int sockfd, struct sockaddr *addr, socklen_t *addrlen, uint32_t timeout_ms) {
    socket_t *s = get_socket(sockfd);
    if (!s || !s->is_listening || !s->tcb) {
        return -1;
    }

    uint32_t start_ms = timer_millis();

    /* Wait for completed connection in listener's backlog queue */
    while (s->tcb->next_pending == NULL) {
        if (socket_poll_interrupted()) {
            return -1;
        }

        net_poll();

        if (timeout_ms != 0xFFFFFFFFu) {
            if (timeout_ms == 0 || (timer_millis() - start_ms >= timeout_ms)) {
                return -1;
            }
        }

        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        io_wait();
    }

    /* Pop child TCB from queue */
    tcp_tcb_t *child = s->tcb->next_pending;
    s->tcb->next_pending = child->next_pending;
    child->next_pending = NULL;

    /* Allocate new socket descriptor for this accepted client */
    socket_t *client_sock = alloc_socket();
    if (!client_sock) {
        tcp_abort(child);
        tcp_free_tcb(child);
        return -1;
    }

    client_sock->domain = s->domain;
    client_sock->type = s->type;
    client_sock->protocol = s->protocol;
    client_sock->bound_ip = child->local_ip;
    client_sock->bound_port = child->local_port;
    client_sock->tcb = child;
    child->sock = client_sock;

    char ip_str[32];
    ip_to_str(child->remote_ip, ip_str, sizeof(ip_str));
    dyn_str_set(&client_sock->remote_endpoint_str, ip_str);

    /* Fill client address information if requested by caller */
    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *in = (struct sockaddr_in *)addr;
        in->sin_family = AF_INET;
        in->sin_port = htons(child->remote_port);
        in->sin_addr.s_addr = htonl(child->remote_ip);
        *addrlen = sizeof(struct sockaddr_in);
    }

    serial_printf("[socket] Accepted connection: socket %d connected from client %s:%u\n",
                  client_sock->fd, ip_str, child->remote_port);

    return client_sock->fd;
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    return accept_timeout(sockfd, addr, addrlen, 0xFFFFFFFFu);
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    socket_t *s = get_socket(sockfd);
    if (!s || !addr || addrlen < sizeof(struct sockaddr_in)) {
        return -1;
    }
    if (s->tcb && s->tcb->state != TCP_STATE_CLOSED) {
        return -1;
    }

    const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
    ip4_addr_t remote_ip = ntohl(in->sin_addr.s_addr);
    uint16_t remote_port = ntohs(in->sin_port);
    uint16_t hw_mss = rtl8139_get_hw_mss();

    if (!s->tcb) {
        s->tcb = tcp_alloc_tcb(hw_mss, 1000u, 8000u, 5u, 2000u);
        if (!s->tcb) return -1;
    }

    s->tcb->sock = s;
    s->tcb->local_ip = s->bound_ip;
    s->tcb->local_port = s->bound_port;

    int res = tcp_active_open(s->tcb, remote_ip, remote_port, hw_mss, 1000u, 8000u, 5u, 2000u);
    if (res < 0) return -1;

    /* Wait for handshake to complete (timeout: 5 seconds) */
    uint32_t start = timer_millis();
    while (s->tcb->state != TCP_STATE_ESTABLISHED) {
        if (socket_poll_interrupted()) {
            tcp_abort(s->tcb);
            return -1;
        }
        net_poll();
        if (s->tcb->state == TCP_STATE_CLOSED) {
            return -1;
        }
        if (timer_millis() - start > 5000) {
            tcp_abort(s->tcb);
            return -1;
        }
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        io_wait();
    }

    return 0;
}

int connect_endpoint(int sockfd, const char *host_or_ip, uint16_t port) {
    socket_t *s = get_socket(sockfd);
    if (!s || !host_or_ip) return -1;

    /* Store arbitrary-length host/IP without truncation */
    dyn_str_set(&s->remote_endpoint_str, host_or_ip);

    ip4_addr_t target_ip = 0;
    if (!str_to_ip(host_or_ip, &target_ip)) {
        /* Resolving domain name via DNS */
        if (!dns_resolve(host_or_ip, &target_ip, 3000)) {
            serial_printf("[socket] Failed to resolve host '%s'\n", host_or_ip);
            return -1;
        }
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(target_ip);

    return connect(sockfd, (const struct sockaddr *)&addr, sizeof(addr));
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    socket_t *s = get_socket(sockfd);
    if (!s || !s->tcb || !buf || len == 0) return -1;

    if (s->tcb->state != TCP_STATE_ESTABLISHED && s->tcb->state != TCP_STATE_CLOSE_WAIT) {
        return -1;
    }

    const uint8_t *src = (const uint8_t *)buf;
    size_t total_sent = 0;

    while (total_sent < len) {
        if (system_is_interrupted()) {
            return (total_sent > 0) ? (ssize_t)total_sent : -1;
        }

        /* TCP sliding-window flow control */
        uint32_t wait_start = timer_millis();
        uint16_t cur_mss = s->tcb->mss ? s->tcb->mss : rtl8139_get_hw_mss();
        size_t max_tx_cap = s->tcb->max_tx_buf ? s->tcb->max_tx_buf : (rtl8139_get_hw_rx_ring_size() * 2u);
        uint32_t wnd = s->tcb->snd_wnd ? s->tcb->snd_wnd : cur_mss;
        if ((flags & 0x40 /* MSG_DONTWAIT */) &&
            ((s->tcb->tx_buf.size >= max_tx_cap) || ((s->tcb->snd_nxt - s->tcb->snd_una) >= wnd))) {
            return (total_sent > 0) ? (ssize_t)total_sent : -1;
        }

        while ((s->tcb->tx_buf.size >= max_tx_cap) || ((s->tcb->snd_nxt - s->tcb->snd_una) >= wnd)) {
            if (s->tcb->state != TCP_STATE_ESTABLISHED && s->tcb->state != TCP_STATE_CLOSE_WAIT) {
                return (total_sent > 0) ? (ssize_t)total_sent : -1;
            }
            if (system_is_interrupted() || (timer_millis() - wait_start > 15000)) {
                return (total_sent > 0) ? (ssize_t)total_sent : -1;
            }
            net_poll();
            wnd = s->tcb->snd_wnd ? s->tcb->snd_wnd : cur_mss;
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
            io_wait();
        }

        size_t chunk = len - total_sent;
        if (chunk > cur_mss) chunk = cur_mss;
        uint32_t in_flight = s->tcb->snd_nxt - s->tcb->snd_una;
        if (s->tcb->snd_wnd > 0 && wnd > in_flight && chunk > (size_t)(wnd - in_flight)) {
            chunk = (size_t)(wnd - in_flight);
        }

        /* Append chunk to retransmission queue */
        if (!dyn_buf_append(&s->tcb->tx_buf, src + total_sent, chunk)) {
            break;
        }

        int sent = tcp_send_segment(s->tcb, TCP_FLAG_ACK | TCP_FLAG_PSH, src + total_sent, (uint16_t)chunk,
                                    cur_mss,
                                    s->tcb->default_rto_ms ? s->tcb->default_rto_ms : 1000u,
                                    s->tcb->max_rto_ms ? s->tcb->max_rto_ms : 8000u,
                                    s->tcb->max_retries ? s->tcb->max_retries : 5u,
                                    s->tcb->time_wait_duration_ms ? s->tcb->time_wait_duration_ms : 2000u);
        if (sent < 0) {
            if (s->tcb->tx_buf.size >= chunk) {
                s->tcb->tx_buf.size -= chunk;
            }
            break;
        }

        total_sent += chunk;
        net_poll();
    }

    return (ssize_t)total_sent;
}

static ssize_t socket_pop_rx(socket_t *s, void *buf, size_t len, int flags) {
    if (flags & 0x02 /* MSG_PEEK */) {
        size_t peek_len = (len < s->tcb->rx_buf.size) ? len : s->tcb->rx_buf.size;
        if (peek_len > 0 && s->tcb->rx_buf.data) {
            memcpy(buf, s->tcb->rx_buf.data, peek_len);
        }
        return (ssize_t)peek_len;
    }
    size_t consumed = dyn_buf_consume(&s->tcb->rx_buf, buf, len);
    uint32_t max_wnd = s->tcb->max_rcv_wnd ? s->tcb->max_rcv_wnd : rtl8139_get_hw_max_window();
    uint32_t half_wnd = max_wnd >> 1;
    if (s->tcb->rcv_wnd < half_wnd) {
        uint32_t new_wnd = (s->tcb->rx_buf.size < max_wnd) ? (max_wnd - (uint32_t)s->tcb->rx_buf.size) : 0;
        if (new_wnd > s->tcb->rcv_wnd) {
            s->tcb->rcv_wnd = new_wnd;
            tcp_send_ack(s->tcb);
        }
    }
    return (ssize_t)consumed;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    socket_t *s = get_socket(sockfd);
    if (!s || !s->tcb || !buf || len == 0) return -1;

    if (s->tcb->rx_buf.size == 0) {
        net_poll();
    }

    /* Return data immediately if already buffered in rx_buf */
    if (s->tcb->rx_buf.size > 0) {
        return socket_pop_rx(s, buf, len, flags);
    }

    /* If non-blocking requested and no data available */
    if (flags & 0x40 /* MSG_DONTWAIT */) {
        if (s->tcb->state == TCP_STATE_CLOSE_WAIT ||
            s->tcb->state == TCP_STATE_CLOSED ||
            s->tcb->state == TCP_STATE_TIME_WAIT) {
            return 0;
        }
        return -1;
    }

    /* If connection closed by remote, return 0 (EOF) */
    if (s->tcb->state == TCP_STATE_CLOSE_WAIT ||
        s->tcb->state == TCP_STATE_CLOSED ||
        s->tcb->state == TCP_STATE_TIME_WAIT) {
        return 0;
    }

    /* Wait for data or EOF */
    while (s->tcb->rx_buf.size == 0) {
        if (socket_poll_interrupted()) {
            return -1;
        }

        net_poll();

        if (s->tcb->rx_buf.size > 0) {
            return socket_pop_rx(s, buf, len, flags);
        }

        if (s->tcb->state == TCP_STATE_CLOSE_WAIT ||
            s->tcb->state == TCP_STATE_CLOSED ||
            s->tcb->state == TCP_STATE_TIME_WAIT) {
            return 0; /* EOF */
        }

        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        io_wait();
    }

    return -1;
}

int socket_close(int sockfd) {
    socket_t *s = get_socket(sockfd);
    if (!s) return -1;

    if (s->tcb) {
        tcp_tcb_t *tcb = s->tcb;
        s->tcb = NULL;
        tcb->sock = NULL;
        tcp_close(tcb);
        if (tcb->state == TCP_STATE_CLOSED) {
            tcp_free_tcb(tcb);
        }
    }

    dyn_str_free(&s->remote_endpoint_str);
    s->in_use = false;
    s->is_listening = false;

    size_t idx = (size_t)(sockfd - 1);
    sock_table[idx] = NULL;
    kfree(s);

    return 0;
}

__attribute__((weak)) int close(int sockfd) {
    return socket_close(sockfd);
}
