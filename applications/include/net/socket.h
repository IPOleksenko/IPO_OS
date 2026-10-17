#ifndef IPO_NET_SOCKET_H
#define IPO_NET_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <net/tcp.h>
#include <net/dyn_buf.h>

#define AF_UNSPEC 0
#define AF_INET   2
#define PF_INET   AF_INET

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

#define IPPROTO_IP  0
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

#define INADDR_ANY       0x00000000u
#define INADDR_LOOPBACK  0x7F000001u
#define INADDR_BROADCAST 0xFFFFFFFFu

typedef uint32_t socklen_t;
#ifndef _SSIZE_T_DEFINED
typedef int32_t  ssize_t;
#define _SSIZE_T_DEFINED
#endif

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

struct sockaddr_in {
    uint16_t       sin_family;
    uint16_t       sin_port;
    struct in_addr sin_addr;
    char           sin_zero[8];
};

/* High-level socket descriptor state */
typedef struct socket {
    int          fd;
    int          domain;
    int          type;
    int          protocol;
    bool         in_use;
    bool         is_listening;
    int          backlog_limit;

    tcp_tcb_t   *tcb;

    /* Associated endpoints */
    ip4_addr_t   bound_ip;
    uint16_t     bound_port;

    /* Dynamic string endpoint allowing arbitrary length hostnames or IPs */
    dyn_str_t    remote_endpoint_str;
} socket_t;

/* POSIX-like Socket API */
int     socket(int domain, int type, int protocol);
int     bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int     listen(int sockfd, int backlog);
int     accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int     accept_timeout(int sockfd, struct sockaddr *addr, socklen_t *addrlen, uint32_t timeout_ms);
int     connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
int     socket_close(int sockfd);
int     close(int sockfd);

/* Helper for arbitrary-length addresses (supports 100+ chars, hostnames, DNS) */
int     connect_endpoint(int sockfd, const char *host_or_ip, uint16_t port);

void    socket_subsystem_init(void);

#endif /* IPO_NET_SOCKET_H */

