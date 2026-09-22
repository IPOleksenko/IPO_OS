#ifndef IPO_NET_DNS_H
#define IPO_NET_DNS_H

#include <net/net.h>

bool dns_resolve(const char *hostname, ip4_addr_t *out_ip, uint32_t timeout_ms);

#endif /* IPO_NET_DNS_H */
