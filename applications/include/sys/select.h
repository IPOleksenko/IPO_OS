#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#include <sys/types.h>
#include <sys/time.h>
#include <string.h>

#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define FD_ZERO(set) memset((set), 0, sizeof(fd_set))
#define FD_SET(fd, set) do { \
    int _fd = (fd); \
    if (_fd >= 0 && _fd < FD_SETSIZE) \
        (set)->fds_bits[_fd / (8 * sizeof(unsigned long))] |= (1UL << (_fd % (8 * sizeof(unsigned long)))); \
} while (0)

#define FD_CLR(fd, set) do { \
    int _fd = (fd); \
    if (_fd >= 0 && _fd < FD_SETSIZE) \
        (set)->fds_bits[_fd / (8 * sizeof(unsigned long))] &= ~(1UL << (_fd % (8 * sizeof(unsigned long)))); \
} while (0)

#define FD_ISSET(fd, set) (((fd) >= 0 && (fd) < FD_SETSIZE) ? \
    (((set)->fds_bits[(fd) / (8 * sizeof(unsigned long))] & (1UL << ((fd) % (8 * sizeof(unsigned long))))) != 0) : 0)

#ifdef __cplusplus
extern "C" {
#endif

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SELECT_H */
