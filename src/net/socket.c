#include <net/socket.h>
#include <net/tcp.h>
#include <net/dns.h>
#include <net/net.h>
#include <system/timer.h>
#include <system/state.h>
#include <driver/input/keyboard.h>
#include <driver/input/keymap/keymap.h>
#include <memory/kmalloc.h>
#include <ioport.h>
#include <syscall.h>
#include <string.h>
#include <stdio.h>

#define SOCKET_TABLE_INITIAL_CAP 16

static socket_t **sock_table = NULL;
static size_t     sock_table_cap = 0;

void socket_subsystem_init(void) {
    if (!sock_table) {
        sock_table_cap = SOCKET_TABLE_INITIAL_CAP;
        sock_table = (socket_t **)kmalloc(sizeof(socket_t *) * sock_table_cap);
        if (sock_table) {
            memset(sock_table, 0, sizeof(socket_t *) * sock_table_cap);
        }
    }
}

static socket_t *alloc_socket(void) {
    socket_subsystem_init();
    if (!sock_table) return NULL;

    /* 1. Look for free slot */
    for (size_t i = 0; i < sock_table_cap; i++) {
        if (!sock_table[i]) {
            socket_t *s = (socket_t *)kmalloc(sizeof(socket_t));
            if (!s) return NULL;
            memset(s, 0, sizeof(socket_t));
            s->fd = (int)(i + 1);
            s->in_use = true;
            dyn_str_init(&s->remote_endpoint_str, NULL);
            sock_table[i] = s;
            return s;
        }
    }

    /* 2. Dynamically expand socket table if all slots are occupied */
    size_t new_cap = sock_table_cap * 2;
    socket_t **new_table = (socket_t **)krealloc(sock_table, sizeof(socket_t *) * new_cap);
    if (!new_table) return NULL;

    memset(new_table + sock_table_cap, 0, sizeof(socket_t *) * (new_cap - sock_table_cap));
    sock_table = new_table;

    size_t idx = sock_table_cap;
    sock_table_cap = new_cap;

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
        /* Currently implementing TCP Stream Socket */
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
        static uint16_t ephem = 49152;
        s->bound_port = ephem++;
        if (ephem > 65000) ephem = 49152;
    }

    serial_printf("[socket] Bound socket %d to IP %x, port %u\n",
                  sockfd, (unsigned int)s->bound_ip, s->bound_port);
    return 0;
}

int listen(int sockfd, int backlog) {
    socket_t *s = get_socket(sockfd);
    if (!s) return -1;

    /* Allocate or re-use TCB */
    if (!s->tcb) {
        s->tcb = tcp_alloc_tcb();
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
        /* Check if user requested stop (ESC, 'q', Ctrl+C) */
        if (system_is_interrupted()) {
            return -1;
        }
        keyboard_poll();
        uint8_t sc = keyboard_get_scancode();
        if (sc == 0x01 || sc == 0x10 || (sc == 0x2E && keyboard_is_ctrl_pressed())) {
            system_request_interrupt();
            return -1;
        }

        net_poll();

        if (timeout_ms != 0xFFFFFFFFu) {
            if (timeout_ms == 0 || (timer_millis() - start_ms >= timeout_ms)) {
                return -1; /* Timed out or non-blocking */
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

    char ip_str[64];
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

    const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
    ip4_addr_t remote_ip = ntohl(in->sin_addr.s_addr);
    uint16_t remote_port = ntohs(in->sin_port);

    if (!s->tcb) {
        s->tcb = tcp_alloc_tcb();
        if (!s->tcb) return -1;
    }

    s->tcb->sock = s;
    s->tcb->local_ip = s->bound_ip;
    s->tcb->local_port = s->bound_port;

    int res = tcp_active_open(s->tcb, remote_ip, remote_port);
    if (res < 0) return -1;

    /* Wait for handshake to complete (timeout: 5 seconds) */
    uint32_t start = timer_millis();
    while (s->tcb->state != TCP_STATE_ESTABLISHED) {
        if (system_is_interrupted()) {
            tcp_abort(s->tcb);
            return -1;
        }
        keyboard_poll();
        uint8_t sc = keyboard_get_scancode();
        if (sc == 0x01 || sc == 0x10 || (sc == 0x2E && keyboard_is_ctrl_pressed())) {
            system_request_interrupt();
            tcp_abort(s->tcb);
            return -1;
        }
        net_poll();
        if (s->tcb->state == TCP_STATE_CLOSED) {
            return -1; /* Connection refused or reset */
        }
        if (timer_millis() - start > 5000) {
            tcp_abort(s->tcb);
            return -1; /* Connection timed out */
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
            if (strcmp(host_or_ip, "google.com") == 0 || strcmp(host_or_ip, "www.google.com") == 0) {
                target_ip = IP4_ADDR(142, 250, 180, 206);
            } else {
                serial_printf("[socket] Failed to resolve host '%s'\n", host_or_ip);
                return -1;
            }
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
    (void)flags;
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

        /* TCP sliding-window flow control:
         * Wait while unacknowledged in-flight data exceeds the peer's window
         * OR pending tx_buf exceeds safe threshold (16 KB)
         */
        uint32_t wait_start = timer_millis();
        uint32_t wnd = s->tcb->snd_wnd ? s->tcb->snd_wnd : 1460;
        while ((s->tcb->tx_buf.size >= 16384) || ((s->tcb->snd_nxt - s->tcb->snd_una) >= wnd)) {
            if (s->tcb->state != TCP_STATE_ESTABLISHED && s->tcb->state != TCP_STATE_CLOSE_WAIT) {
                return (total_sent > 0) ? (ssize_t)total_sent : -1;
            }
            if (system_is_interrupted() || (timer_millis() - wait_start > 15000)) {
                break;
            }
            net_poll();
            wnd = s->tcb->snd_wnd ? s->tcb->snd_wnd : 1460;
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
            io_wait();
        }

        size_t chunk = len - total_sent;
        if (chunk > 1460) chunk = 1460;

        /* Append chunk to retransmission queue */
        if (!dyn_buf_append(&s->tcb->tx_buf, src + total_sent, chunk)) {
            break;
        }

        int sent = tcp_send_segment(s->tcb, TCP_FLAG_ACK | TCP_FLAG_PSH, src + total_sent, (uint16_t)chunk);
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

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    (void)flags;
    socket_t *s = get_socket(sockfd);
    if (!s || !s->tcb || !buf || len == 0) return -1;

    /* If data is available in self-expanding dynamic rx_buf, return it immediately */
    if (s->tcb->rx_buf.size > 0) {
        size_t consumed = dyn_buf_consume(&s->tcb->rx_buf, buf, len);
        if (s->tcb->rcv_wnd < 32768) {
            uint32_t new_wnd = (s->tcb->rx_buf.size < 65535) ? (65535 - s->tcb->rx_buf.size) : 0;
            if (new_wnd > s->tcb->rcv_wnd) {
                s->tcb->rcv_wnd = new_wnd;
                tcp_send_ack(s->tcb);
            }
        }
        return (ssize_t)consumed;
    }

    /* If connection closed by remote, return 0 (EOF) */
    if (s->tcb->state == TCP_STATE_CLOSE_WAIT ||
        s->tcb->state == TCP_STATE_CLOSED ||
        s->tcb->state == TCP_STATE_TIME_WAIT) {
        return 0;
    }

    /* Wait for data or EOF */
    while (s->tcb->rx_buf.size == 0) {
        if (system_is_interrupted()) {
            return -1;
        }
        keyboard_poll();
        uint8_t sc = keyboard_get_scancode();
        if (sc == 0x01 || sc == 0x10 || (sc == 0x2E && keyboard_is_ctrl_pressed())) {
            system_request_interrupt();
            return -1;
        }

        net_poll();

        if (s->tcb->rx_buf.size > 0) {
            size_t consumed = dyn_buf_consume(&s->tcb->rx_buf, buf, len);
            if (s->tcb->rcv_wnd < 32768) {
                uint32_t new_wnd = (s->tcb->rx_buf.size < 65535) ? (65535 - s->tcb->rx_buf.size) : 0;
                if (new_wnd > s->tcb->rcv_wnd) {
                    s->tcb->rcv_wnd = new_wnd;
                    tcp_send_ack(s->tcb);
                }
            }
            return (ssize_t)consumed;
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
        tcp_close(s->tcb);
        s->tcb->sock = NULL;
        s->tcb = NULL;
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

