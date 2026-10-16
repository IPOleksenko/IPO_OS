#ifndef _ARPA_INET_H
#define _ARPA_INET_H

#include <stdint.h>
#include <netinet/in.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

in_addr_t       inet_addr(const char *cp);
int             inet_aton(const char *cp, struct in_addr *inp);
char           *inet_ntoa(struct in_addr in);
const char     *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int             inet_pton(int af, const char *src, void *dst);

#ifdef __cplusplus
}
#endif

#endif /* _ARPA_INET_H */

