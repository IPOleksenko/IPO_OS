#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <stdint.h>

struct stat {
    uint32_t st_mode;
    uint32_t st_size;
};

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)

int stat(const char *pathname, struct stat *statbuf);
int fstat(int fd, struct stat *statbuf);

#endif
