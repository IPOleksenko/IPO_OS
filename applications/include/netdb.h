#ifndef _NETDB_H
#define _NETDB_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hostent {
    char  *h_name;            /* Official name of host */
    char **h_aliases;         /* Alias list */
    int    h_addrtype;        /* Host address type (AF_INET) */
    int    h_length;          /* Length of address */
    char **h_addr_list;       /* List of addresses from name server */
};
#define h_addr h_addr_list[0] /* Backward compatibility */

struct servent {
    char  *s_name;
    char **s_aliases;
    int    s_port;
    char  *s_proto;
};

struct protoent {
    char  *p_name;
    char **p_aliases;
    int    p_proto;
};

#define NETDB_INTERNAL  -1
#define NETDB_SUCCESS    0
#define HOST_NOT_FOUND   1
#define TRY_AGAIN        2
#define NO_RECOVERY      3
#define NO_DATA          4
#define NO_ADDRESS       NO_DATA

extern int h_errno;

struct hostent *gethostbyname(const char *name);
struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type);
struct servent *getservbyname(const char *name, const char *proto);
struct servent *getservbyport(int port, const char *proto);
struct protoent *getprotobyname(const char *name);
struct protoent *getprotobynumber(int proto);
void            herror(const char *s);
const char     *hstrerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* _NETDB_H */

