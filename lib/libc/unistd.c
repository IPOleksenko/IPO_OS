#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <syscall.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int errno = 0;

int open(const char *pathname, int flags, ...) {
    if (!pathname) {
        errno = EINVAL;
        return -1;
    }

    if (flags & O_CREAT) {
        struct ipo_inode st;
        if (ipo_stat(pathname, &st) != 0) {
            ipo_create(pathname, 0x2 /* IPO_INODE_TYPE_FILE */);
        } else if (flags & O_TRUNC) {
            ipo_delete(pathname);
            ipo_create(pathname, 0x2);
        }
    }

    int fd = ipo_open(pathname);
    if (fd < 0) {
        errno = ENOENT;
        return -1;
    }

    if (flags & O_APPEND) {
        ipo_seek(fd, 0, SEEK_END);
    } else {
        ipo_seek(fd, 0, SEEK_SET);
    }

    return fd;
}

int close(int fd) {
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (fd < 3) {
        return 0;
    }
    return ipo_close(fd);
}

static char *stdin_line_buf = NULL;
static size_t stdin_line_cap = 0;
static size_t stdin_line_len = 0;
static size_t stdin_line_pos = 0;

ssize_t read(int fd, void *buf, size_t count) {
    if (fd < 0 || !buf) {
        errno = EBADF;
        return -1;
    }
    if (count == 0) return 0;

    if (fd == STDIN_FILENO) {
        if (stdin_line_pos >= stdin_line_len) {
            stdin_line_pos = 0;
            stdin_line_len = 0;
            char *line = NULL;
            int n = ipo_read_line_dynamic(&line);
            if (n == -2) {
                errno = EINTR;
                return -1;
            }
            if (n == -3) {
                /* EOF (Ctrl+D) */
                return 0;
            }
            if (n < 0 || line == NULL) {
                return -1;
            }
            size_t needed = (size_t)n + 2;
            if (stdin_line_cap < needed) {
                char *new_buf = (char *)realloc(stdin_line_buf, needed);
                if (!new_buf) {
                    ipo_kfree(line);
                    errno = ENOMEM;
                    return -1;
                }
                stdin_line_buf = new_buf;
                stdin_line_cap = needed;
            }
            memcpy(stdin_line_buf, line, (size_t)n);
            stdin_line_buf[n] = '\n';
            stdin_line_buf[n + 1] = '\0';
            stdin_line_len = (size_t)n + 1;
            ipo_kfree(line);
        }

        size_t avail = stdin_line_len - stdin_line_pos;
        size_t to_read = count < avail ? count : avail;
        memcpy(buf, stdin_line_buf + stdin_line_pos, to_read);
        stdin_line_pos += to_read;
        return (ssize_t)to_read;
    }
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        errno = EBADF;
        return -1;
    }

    uint32_t args[3];
    args[0] = (uint32_t)fd;
    args[1] = (uint32_t)(uintptr_t)buf;
    args[2] = (uint32_t)count;
    int res = ipo_syscall(IPO_SYSCALL_FS_READ, 3u, args);
    if (res < 0) {
        errno = EIO;
        return -1;
    }
    return res;
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (fd < 0 || !buf) {
        errno = EBADF;
        return -1;
    }
    if (count == 0) return 0;

    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        uint32_t args[2];
        args[0] = (uint32_t)(uintptr_t)buf;
        args[1] = (uint32_t)count;
        int res = ipo_syscall(IPO_SYSCALL_WRITE, 2u, args);
        if (res < 0) {
            errno = EIO;
            return -1;
        }
        return (ssize_t)res;
    }
    if (fd == STDIN_FILENO) {
        errno = EBADF;
        return -1;
    }

    uint32_t args[3];
    args[0] = (uint32_t)fd;
    args[1] = (uint32_t)(uintptr_t)buf;
    args[2] = (uint32_t)count;
    int res = ipo_syscall(IPO_SYSCALL_FS_WRITE, 3u, args);
    if (res < 0) {
        errno = EIO;
        return -1;
    }
    return res;
}

off_t lseek(int fd, off_t offset, int whence) {
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (fd < 3) return 0;
    int res = ipo_seek(fd, (int32_t)offset, whence);
    if (res < 0) {
        errno = EINVAL;
        return -1;
    }
    return (off_t)res;
}

int unlink(const char *pathname) {
    if (!pathname) {
        errno = EINVAL;
        return -1;
    }
    if (ipo_delete(pathname) == 0) {
        return 0;
    }
    errno = ENOENT;
    return -1;
}

int rmdir(const char *pathname) {
    return unlink(pathname);
}

char *getcwd(char *buf, size_t size) {
    if (!buf || size == 0) {
        errno = EINVAL;
        return NULL;
    }
    if (ipo_getcwd(buf, (uint32_t)size) == 0) {
        return NULL;
    }
    return buf;
}

int chdir(const char *path) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    if (ipo_chdir(path) != 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int access(const char *pathname, int mode) {
    (void)mode;
    if (!pathname) {
        errno = EINVAL;
        return -1;
    }
    struct ipo_inode st;
    if (ipo_stat(pathname, &st) == 0) {
        return 0;
    }
    errno = ENOENT;
    return -1;
}

int isatty(int fd) {
    return (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) ? 1 : 0;
}

pid_t getpid(void) {
    return 1;
}

pid_t getppid(void) {
    return 1;
}

uid_t getuid(void) {
    return 0;
}

gid_t getgid(void) {
    return 0;
}

void *sbrk(intptr_t increment) {
    void *p = ipo_sbrk((int32_t)increment);
    if (p == (void *)-1) {
        errno = ENOMEM;
    }
    return p;
}

int brk(void *addr) {
    (void)addr;
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    uint32_t start = ipo_time(NULL);
    while (ipo_time(NULL) - start < seconds) {
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
    }
    return 0;
}

int usleep(useconds_t usec) {
    unsigned int secs = usec / 1000000;
    if (secs > 0) sleep(secs);
    else ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
    return 0;
}

int execv(const char *path, char *const argv[]) {
    int argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }
    int pid = ipo_exec(path, argc, (char **)argv);
    if (pid < 0) {
        errno = ENOENT;
        return -1;
    }
    exit(ipo_get_exit_code());
    return 0;
}

int execvp(const char *file, char *const argv[]) {
    return execv(file, argv);
}

