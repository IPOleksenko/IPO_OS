#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <stdint.h>
#include <sys/socket.h>

#ifndef INADDR_ANY
#define INADDR_ANY ((in_addr_t)0x00000000)
#endif

#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK ((in_addr_t)0x7F000001)
#endif

#ifndef INADDR_BROADCAST
#define INADDR_BROADCAST ((in_addr_t)0xFFFFFFFF)
#endif

#ifndef INADDR_NONE
#define INADDR_NONE ((in_addr_t)0xFFFFFFFF)
#endif

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_RAW  255

#ifndef _BYTEORDER_HELPERS_DEFINED
#define _BYTEORDER_HELPERS_DEFINED
static inline uint16_t htons(uint16_t v) { return (uint16_t)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8)  |
           ((v & 0x00FF0000u) >> 8)  |
           ((v & 0xFF000000u) >> 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }
#endif

#endif /* _NETINET_IN_H */
