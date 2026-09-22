#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <stdint.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

extern char **environ;

int close(int fd);
int read(int fd, void *buf, size_t count);
int write(int fd, const void *buf, size_t count);
long lseek(int fd, long offset, int whence);
int unlink(const char *pathname);
char *getcwd(char *buf, size_t size);
int execvp(const char *file, char *const argv[]);

#endif
