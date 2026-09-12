#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <stdint.h>

struct stat {
    uint64_t st_dev;
    uint16_t __pad1;
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint16_t __pad2;
    int32_t  st_size;
    int32_t  st_blksize;
    int32_t  st_blocks;
    int32_t  st_atime;
    uint32_t st_atime_nsec;
    int32_t  st_mtime;
    uint32_t st_mtime_nsec;
    int32_t  st_ctime;
    uint32_t st_ctime_nsec;
    uint32_t __unused4;
    uint32_t __unused5;
};

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)

int stat(const char *pathname, struct stat *statbuf);
int fstat(int fd, struct stat *statbuf);

#endif
