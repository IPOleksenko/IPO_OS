#ifndef IPO_NET_DNS_H
#define IPO_NET_DNS_H

#include <net/net.h>

bool dns_resolve(const char *hostname, ip4_addr_t *out_ip, uint32_t timeout_ms);
bool dns_resolve_ex(const char *hostname, ip4_addr_t *out_ip, uint32_t timeout_ms,
                    uint32_t max_retries, uint32_t retry_interval_ms);
void dns_process_response_packet(const void *data, uint16_t len);

#endif /* IPO_NET_DNS_H */
