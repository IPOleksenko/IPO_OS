#include <net/socket.h>
#include <syscall.h>

static int net_socket_call(uint32_t syscall_number, uint32_t argc, uint32_t *argv) {
    return ipo_syscall(syscall_number, argc, argv);
}

int socket(int domain, int type, int protocol) {
    uint32_t args[3] = { (uint32_t)domain, (uint32_t)type, (uint32_t)protocol };
    return net_socket_call(IPO_SYSCALL_NET_SOCKET, 3u, args);
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    uint32_t args[3] = {
        (uint32_t)sockfd,
        (uint32_t)(uintptr_t)addr,
        addrlen
    };
    return net_socket_call(IPO_SYSCALL_NET_BIND, 3u, args);
}

int listen(int sockfd, int backlog) {
    uint32_t args[2] = { (uint32_t)sockfd, (uint32_t)backlog };
    return net_socket_call(IPO_SYSCALL_NET_LISTEN, 2u, args);
}

int accept_timeout(int sockfd, struct sockaddr *addr, socklen_t *addrlen, uint32_t timeout_ms) {
    uint32_t args[4] = {
        (uint32_t)sockfd,
        (uint32_t)(uintptr_t)addr,
        (uint32_t)(uintptr_t)addrlen,
        timeout_ms
    };
    return net_socket_call(IPO_SYSCALL_NET_ACCEPT, 4u, args);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    return accept_timeout(sockfd, addr, addrlen, 0xFFFFFFFFu);
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    uint32_t args[3] = {
        (uint32_t)sockfd,
        (uint32_t)(uintptr_t)addr,
        addrlen
    };
    return net_socket_call(IPO_SYSCALL_NET_CONNECT, 3u, args);
}

int connect_endpoint(int sockfd, const char *host_or_ip, uint16_t port) {
    uint32_t args[3] = {
        (uint32_t)sockfd,
        (uint32_t)(uintptr_t)host_or_ip,
        (uint32_t)port
    };
    return net_socket_call(IPO_SYSCALL_NET_CONNECT_ENDPOINT, 3u, args);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    uint32_t args[4] = {
        (uint32_t)sockfd,
        (uint32_t)(uintptr_t)buf,
        (uint32_t)len,
        (uint32_t)flags
    };
    return (ssize_t)net_socket_call(IPO_SYSCALL_NET_SEND, 4u, args);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    uint32_t args[4] = {
        (uint32_t)sockfd,
        (uint32_t)(uintptr_t)buf,
        (uint32_t)len,
        (uint32_t)flags
    };
    return (ssize_t)net_socket_call(IPO_SYSCALL_NET_RECV, 4u, args);
}

int socket_close(int sockfd) {
    uint32_t args[1] = { (uint32_t)sockfd };
    return net_socket_call(IPO_SYSCALL_NET_CLOSE, 1u, args);
}

int __attribute__((weak)) close(int sockfd) {
    return socket_close(sockfd);
}
