#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <syscall.h>

int ipo_syscall(uint32_t num, uint32_t argc, uint32_t *argv) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num),
          "b"(argc),
          "c"(argv)
        : "memory"
    );
    return ret;
}

uintptr_t __stack_chk_guard = 0x595e9fbd;

void __attribute__((noreturn)) __stack_chk_fail(void) {
    fputs("*** stack smashing detected ***: terminated\n", stderr);
    abort();
}

void __attribute__((noreturn, visibility("hidden"))) __stack_chk_fail_local(void) {
    __stack_chk_fail();
}

int stat(const char *pathname, struct stat *statbuf) {
    if (!pathname || !statbuf) {
        errno = EFAULT;
        return -1;
    }
    struct ipo_inode st;
    if (ipo_stat(pathname, &st) == 0) {
        memset(statbuf, 0, sizeof(struct stat));
        if (st.mode & IPO_INODE_TYPE_DIR) {
            statbuf->st_mode = S_IFDIR | 0777;
        } else {
            statbuf->st_mode = S_IFREG | 0777;
        }
        statbuf->st_size = (uint32_t)st.size;
        return 0;
    }
    int fd = open(pathname, O_RDONLY);
    if (fd < 0) {
        errno = ENOENT;
        return -1;
    }
    int res = fstat(fd, statbuf);
    close(fd);
    return res;
}

int fstat(int fd, struct stat *statbuf) {
    if (fd < 0 || !statbuf) {
        errno = EBADF;
        return -1;
    }

    off_t cur = lseek(fd, 0, SEEK_CUR);
    off_t size = lseek(fd, 0, SEEK_END);
    if (cur != (off_t)-1) {
        lseek(fd, cur, SEEK_SET);
    }

    memset(statbuf, 0, sizeof(struct stat));
    statbuf->st_mode = S_IFREG | 0777;
    statbuf->st_size = (size == (off_t)-1) ? 0 : (int32_t)size;
    return 0;
}

time_t time(time_t *t) {
    time_t now = (time_t)ipo_time(NULL);
    if (t) *t = now;
    return now;
}

clock_t clock(void) {
    return (clock_t)(ipo_time(NULL) * 1000);
}

double difftime(time_t time1, time_t time0) {
    return (double)(time1 - time0);
}

static struct tm static_tm;

struct tm *gmtime(const time_t *timep) {
    if (!timep) return NULL;
    time_t t = *timep;

    memset(&static_tm, 0, sizeof(static_tm));
    static_tm.tm_sec = t % 60;
    t /= 60;
    static_tm.tm_min = t % 60;
    t /= 60;
    static_tm.tm_hour = t % 24;
    t /= 24;

    static_tm.tm_wday = (t + 4) % 7; // 1970-01-01 was Thursday (4)

    // Rough conversion for demonstration & toolchain time display
    int year = 1970;
    while (1) {
        int days_in_year = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) ? 366 : 365;
        if (t >= days_in_year) {
            t -= days_in_year;
            year++;
        } else {
            break;
        }
    }
    static_tm.tm_year = year - 1900;
    static_tm.tm_yday = t;

    int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
        days_in_month[1] = 29;
    }
    int mon = 0;
    while (mon < 12 && t >= days_in_month[mon]) {
        t -= days_in_month[mon];
        mon++;
    }
    static_tm.tm_mon = mon;
    static_tm.tm_mday = t + 1;

    return &static_tm;
}

struct tm *localtime(const time_t *timep) {
    return gmtime(timep);
}

time_t mktime(struct tm *timeptr) {
    if (!timeptr) return (time_t)-1;
    // Simple inverse calculation
    int year = timeptr->tm_year + 1900;
    int mon = timeptr->tm_mon;
    int day = timeptr->tm_mday;

    int days = 0;
    for (int y = 1970; y < year; y++) {
        days += ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 366 : 365;
    }
    int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
        days_in_month[1] = 29;
    }
    for (int m = 0; m < mon; m++) {
        days += days_in_month[m];
    }
    days += (day - 1);

    time_t t = days * 86400 + timeptr->tm_hour * 3600 + timeptr->tm_min * 60 + timeptr->tm_sec;
    return t;
}

static char time_buf[64];
char *asctime(const struct tm *timeptr) {
    if (!timeptr) return NULL;
    snprintf(time_buf, sizeof(time_buf), "%04d-%02d-%02d %02d:%02d:%02d\n",
             timeptr->tm_year + 1900, timeptr->tm_mon + 1, timeptr->tm_mday,
             timeptr->tm_hour, timeptr->tm_min, timeptr->tm_sec);
    return time_buf;
}

char *ctime(const time_t *timep) {
    return asctime(localtime(timep));
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    if (!s || max == 0 || !format) return 0;
    size_t written = 0;
    const char *f = format;
    while (*f && written + 1 < max) {
        if (*f == '%') {
            f++;
            char buf[32];
            int n = 0;
            switch (*f) {
            case 'Y':
                n = snprintf(buf, sizeof(buf), "%04d", tm ? tm->tm_year + 1900 : 2026);
                break;
            case 'y':
                n = snprintf(buf, sizeof(buf), "%02d", tm ? (tm->tm_year + 1900) % 100 : 26);
                break;
            case 'm':
                n = snprintf(buf, sizeof(buf), "%02d", tm ? tm->tm_mon + 1 : 1);
                break;
            case 'd':
                n = snprintf(buf, sizeof(buf), "%02d", tm ? tm->tm_mday : 1);
                break;
            case 'H':
                n = snprintf(buf, sizeof(buf), "%02d", tm ? tm->tm_hour : 0);
                break;
            case 'M':
                n = snprintf(buf, sizeof(buf), "%02d", tm ? tm->tm_min : 0);
                break;
            case 'S':
                n = snprintf(buf, sizeof(buf), "%02d", tm ? tm->tm_sec : 0);
                break;
            case 'F':
                n = snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                             tm ? tm->tm_year + 1900 : 2026,
                             tm ? tm->tm_mon + 1 : 1,
                             tm ? tm->tm_mday : 1);
                break;
            case 'T':
                n = snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                             tm ? tm->tm_hour : 0,
                             tm ? tm->tm_min : 0,
                             tm ? tm->tm_sec : 0);
                break;
            case '%':
                buf[0] = '%';
                buf[1] = '\0';
                n = 1;
                break;
            default:
                buf[0] = '%';
                buf[1] = *f;
                buf[2] = '\0';
                n = *f ? 2 : 1;
                break;
            }
            for (int i = 0; i < n && written + 1 < max; i++) {
                s[written++] = buf[i];
            }
            if (*f) f++;
        } else {
            s[written++] = *f++;
        }
    }
    s[written] = '\0';
    return written;
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (tv) {
        tv->tv_sec = time(NULL);
        tv->tv_usec = 0;
    }
    return 0;
}

static sighandler_t signal_handlers[32] = {0};

sighandler_t signal(int signum, sighandler_t handler) {
    if (signum < 0 || signum >= 32) {
        errno = EINVAL;
        return SIG_ERR;
    }
    sighandler_t old = signal_handlers[signum];
    signal_handlers[signum] = handler;
    return old;
}

int raise(int sig) {
    if (sig < 0 || sig >= 32) {
        errno = EINVAL;
        return -1;
    }
    sighandler_t h = signal_handlers[sig];
    if (h == SIG_IGN) return 0;
    if (h == SIG_DFL || !h) {
        if (sig == SIGSEGV || sig == SIGABRT || sig == SIGILL || sig == SIGFPE) {
            exit(128 + sig);
        }
        return 0;
    }
    h(sig);
    return 0;
}

pid_t wait(int *wstatus) {
    return waitpid(-1, wstatus, 0);
}

pid_t waitpid(pid_t pid, int *wstatus, int options) {
    (void)options;
    if (wstatus) {
        int code = ipo_get_exit_code();
        *wstatus = (code << 8);
    }
    return pid;
}

void *__memcpy_chk(void *dest, const void *src, size_t len, size_t destlen) {
    (void)destlen;
    return memcpy(dest, src, len);
}

void *__memmove_chk(void *dest, const void *src, size_t len, size_t destlen) {
    (void)destlen;
    return memmove(dest, src, len);
}

void *__memset_chk(void *dest, int c, size_t len, size_t destlen) {
    (void)destlen;
    return memset(dest, c, len);
}

char *__strcpy_chk(char *dest, const char *src, size_t destlen) {
    (void)destlen;
    return strcpy(dest, src);
}

char *__strncpy_chk(char *dest, const char *src, size_t len, size_t destlen) {
    (void)destlen;
    return strncpy(dest, src, len);
}

int __sprintf_chk(char *s, int flag, size_t os, const char *fmt, ...) {
    (void)flag; (void)os;
    va_list ap;
    va_start(ap, fmt);
    int res = vsprintf(s, fmt, ap);
    va_end(ap);
    return res;
}

int __snprintf_chk(char *s, size_t maxlen, int flag, size_t os, const char *fmt, ...) {
    (void)flag; (void)os;
    va_list ap;
    va_start(ap, fmt);
    int res = vsnprintf(s, maxlen, fmt, ap);
    va_end(ap);
    return res;
}

int __vsnprintf_chk(char *s, size_t maxlen, int flag, size_t os, const char *fmt, va_list ap) {
    (void)flag; (void)os;
    return vsnprintf(s, maxlen, fmt, ap);
}

int __fprintf_chk(FILE *fp, int flag, const char *fmt, ...) {
    (void)flag;
    va_list ap;
    va_start(ap, fmt);
    int res = vfprintf(fp, fmt, ap);
    va_end(ap);
    return res;
}

int __printf_chk(int flag, const char *fmt, ...) {
    (void)flag;
    va_list ap;
    va_start(ap, fmt);
    int res = vprintf(fmt, ap);
    va_end(ap);
    return res;
}

char *__strcat_chk(char *dest, const char *src, size_t destlen) {
    (void)destlen;
    return strcat(dest, src);
}

void *mempcpy(void *dest, const void *src, size_t n) {
    memcpy(dest, src, n);
    return (char *)dest + n;
}

void *__mempcpy_chk(void *dest, const void *src, size_t n, size_t destlen) {
    (void)destlen;
    return mempcpy(dest, src, n);
}

int *__errno_location(void) {
    return &errno;
}

int fstat64(int fd, struct stat *buf) {
    return fstat(fd, buf);
}

int stat64(const char *path, struct stat *buf) {
    return stat(path, buf);
}

FILE *fopen64(const char *pathname, const char *mode) {
    return fopen(pathname, mode);
}

int fseeko64(FILE *stream, off_t offset, int whence) {
    return fseek(stream, (long)offset, whence);
}

off_t ftello64(FILE *stream) {
    return (off_t)ftell(stream);
}

int getrlimit(int resource, void *rlim) {
    (void)resource;
    if (rlim) {
        uint32_t *r = (uint32_t *)rlim;
        r[0] = 8 * 1024 * 1024;
        r[1] = 8 * 1024 * 1024;
    }
    return 0;
}

int getrlimit64(int resource, void *rlim) {
    (void)resource;
    if (rlim) {
        uint64_t *r = (uint64_t *)rlim;
        r[0] = 8 * 1024 * 1024;
        r[1] = 8 * 1024 * 1024;
    }
    return 0;
}

int __isoc23_sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int res = vsscanf(str, fmt, ap);
    va_end(ap);
    return res;
}

char *__fgets_chk(char *s, size_t size, int n, FILE *stream) {
    (void)size;
    return fgets(s, n, stream);
}

int __vfprintf_chk(FILE *fp, int flag, const char *fmt, va_list ap) {
    (void)flag;
    return vfprintf(fp, fmt, ap);
}

int ftruncate64(int fd, off_t length) {
    (void)fd; (void)length;
    return 0;
}

int faccessat(int dirfd, const char *pathname, int mode, int flags) {
    (void)dirfd; (void)flags;
    return access(pathname, mode);
}

int fileno(FILE *stream) {
    if (!stream) return -1;
    return (int)stream->fd;
}

long sysconf(int name) {
    (void)name;
    return 4096;
}

char *canonicalize_file_name(const char *path) {
    if (!path) return NULL;
    return strdup(path);
}

unsigned long __isoc23_strtoul(const char *nptr, char **endptr, int base) {
    return strtoul(nptr, endptr, base);
}

long __isoc23_strtol(const char *nptr, char **endptr, int base) {
    return strtol(nptr, endptr, base);
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr; (void)prot; (void)flags; (void)fd; (void)offset;
    return malloc(length);
}

int munmap(void *addr, size_t length) {
    (void)length;
    free(addr);
    return 0;
}

off_t lseek64(int fd, off_t offset, int whence) {
    return lseek(fd, offset, whence);
}

int open64(const char *pathname, int flags, ...) {
    return open(pathname, flags);
}

int fsync(int fd) {
    (void)fd;
    return 0;
}

int statvfs64(const char *path, void *buf) {
    (void)path; (void)buf;
    return 0;
}

int __stat64_time64(const char *path, void *buf) {
    return stat64(path, (struct stat *)buf);
}

void *opendir(const char *name) {
    (void)name;
    return NULL;
}

int closedir(void *dirp) {
    (void)dirp;
    return 0;
}

void *readdir64(void *dirp) {
    (void)dirp;
    return NULL;
}

int mkdir(const char *pathname, mode_t mode) {
    (void)pathname; (void)mode;
    return 0;
}

int sigemptyset(sigset_t *set) {
    if (set) *set = 0;
    return 0;
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    (void)signum; (void)act; (void)oldact;
    return 0;
}

int __clock_gettime64(int clk_id, void *tp) {
    (void)clk_id;
    if (tp) {
        long long *p = (long long *)tp;
        p[0] = (long long)time(NULL);
        p[1] = 0;
    }
    return 0;
}

int __gettimeofday64(struct timeval *tv, void *tz) {
    (void)tz;
    return gettimeofday(tv, NULL);
}

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags) {
    (void)flags;
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i < buflen; i++) {
        p[i] = (uint8_t)(rand() & 0xFF);
    }
    return (ssize_t)buflen;
}

long long __time64(void *tloc) {
    long long t = (long long)time(NULL);
    if (tloc) *(long long *)tloc = t;
    return t;
}

struct tm *__localtime64(const void *timer) {
    time_t t = timer ? (time_t)*(const long long *)timer : time(NULL);
    return localtime(&t);
}

struct tm *gmtime_r(const time_t *timep, struct tm *result) {
    if (!result) return NULL;
    struct tm *t = gmtime(timep);
    if (!t) return NULL;
    *result = *t;
    return result;
}

struct tm *localtime_r(const time_t *timep, struct tm *result) {
    if (!result) return NULL;
    struct tm *t = localtime(timep);
    if (!t) return NULL;
    *result = *t;
    return result;
}

int mkstemp(char *tmpl) {
    if (!tmpl) {
        errno = EINVAL;
        return -1;
    }
    return open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
}

