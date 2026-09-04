#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <file_system/ipo_fs.h>
#include <syscall.h>
#include <memory/kmalloc.h>
#include <system/timer.h>

int errno = 0;

static FILE _stdin_obj  = {0, 0, 0, 0, 0};
static FILE _stdout_obj = {1, 0, 0, 0, 0};
static FILE _stderr_obj = {2, 0, 0, 0, 0};

FILE *stdin  = &_stdin_obj;
FILE *stdout = &_stdout_obj;
FILE *stderr = &_stderr_obj;

#define MAX_SHIM_FDS 64
typedef struct {
    int in_use;
    int ipo_fd;
    uint32_t offset;
    char path[128];
} shim_fd_t;

static shim_fd_t shim_fds[MAX_SHIM_FDS];

static void init_shim_fds(void) {
    static int init = 0;
    if (init) return;
    init = 1;
    for (int i = 0; i < MAX_SHIM_FDS; i++) {
        shim_fds[i].in_use = 0;
        shim_fds[i].ipo_fd = -1;
        shim_fds[i].offset = 0;
        shim_fds[i].path[0] = '\0';
    }
    shim_fds[0].in_use = 1; /* stdin */
    shim_fds[1].in_use = 1; /* stdout */
    shim_fds[2].in_use = 1; /* stderr */
}

/* -------------------------------------------------------------
 * Low-level POSIX I/O shims
 * ------------------------------------------------------------- */

int open(const char *pathname, int flags, ...) {
    init_shim_fds();
    if (!pathname) {
        errno = EINVAL;
        return -1;
    }

    if (flags & O_CREAT) {
        /* If file does not exist, create it */
        struct ipo_inode st;
        if (ipo_stat(pathname, &st) != 0) {
            ipo_create(pathname, IPO_INODE_TYPE_FILE);
        }
    }

    int ipo_fd = ipo_open(pathname);
    if (ipo_fd < 0) {
        errno = ENOENT;
        return -1;
    }

    for (int i = 3; i < MAX_SHIM_FDS; i++) {
        if (!shim_fds[i].in_use) {
            shim_fds[i].in_use = 1;
            shim_fds[i].ipo_fd = ipo_fd;
            shim_fds[i].offset = 0;
            strncpy(shim_fds[i].path, pathname, sizeof(shim_fds[i].path) - 1);
            shim_fds[i].path[sizeof(shim_fds[i].path) - 1] = '\0';
            return i;
        }
    }

    ipo_close(ipo_fd);
    errno = ENOMEM;
    return -1;
}

int close(int fd) {
    init_shim_fds();
    if (fd < 0 || fd >= MAX_SHIM_FDS || !shim_fds[fd].in_use) {
        errno = EINVAL;
        return -1;
    }
    if (fd >= 3) {
        ipo_close(shim_fds[fd].ipo_fd);
        shim_fds[fd].in_use = 0;
        shim_fds[fd].ipo_fd = -1;
    }
    return 0;
}

int read(int fd, void *buf, size_t count) {
    init_shim_fds();
    if (fd < 0 || fd >= MAX_SHIM_FDS || !shim_fds[fd].in_use) {
        errno = EINVAL;
        return -1;
    }
    if (fd == 0) {
        return 0;
    }
    if (fd < 3) {
        return 0;
    }

    int n = ipo_read(shim_fds[fd].ipo_fd, buf, (uint32_t)count, shim_fds[fd].offset);
    if (n > 0) {
        shim_fds[fd].offset += (uint32_t)n;
        return n;
    }
    return 0;
}

int write(int fd, const void *buf, size_t count) {
    init_shim_fds();
    if (fd < 0 || fd >= MAX_SHIM_FDS || !shim_fds[fd].in_use) {
        errno = EINVAL;
        return -1;
    }
    if (fd == 1 || fd == 2) {
        const char *s = (const char *)buf;
        for (size_t i = 0; i < count; i++) {
            putchar(s[i]);
        }
        return (int)count;
    }
    if (fd == 0) return -1;

    int n = ipo_write(shim_fds[fd].ipo_fd, buf, (uint32_t)count, shim_fds[fd].offset);
    if (n > 0) {
        shim_fds[fd].offset += (uint32_t)n;
        return n;
    }
    return -1;
}

long lseek(int fd, long offset, int whence) {
    init_shim_fds();
    if (fd < 0 || fd >= MAX_SHIM_FDS || !shim_fds[fd].in_use) {
        errno = EINVAL;
        return -1;
    }
    if (fd < 3) return 0;

    if (whence == SEEK_SET) {
        shim_fds[fd].offset = (uint32_t)offset;
    } else if (whence == SEEK_CUR) {
        shim_fds[fd].offset += (uint32_t)offset;
    } else if (whence == SEEK_END) {
        struct ipo_inode st;
        if (ipo_stat(shim_fds[fd].path, &st) == 0) {
            shim_fds[fd].offset = (uint32_t)(st.size + offset);
        }
    } else {
        errno = EINVAL;
        return -1;
    }
    return (long)shim_fds[fd].offset;
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

int remove(const char *pathname) {
    return unlink(pathname);
}

char *getcwd(char *buf, size_t size) {
    if (size < 2) return NULL;
    buf[0] = '/';
    buf[1] = '\0';
    return buf;
}

int stat(const char *pathname, struct stat *statbuf) {
    if (!pathname || !statbuf) {
        errno = EINVAL;
        return -1;
    }
    struct ipo_inode st;
    if (ipo_stat(pathname, &st) == 0) {
        statbuf->st_size = (uint32_t)st.size;
        statbuf->st_mode = (st.mode & IPO_INODE_TYPE_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
        return 0;
    }
    errno = ENOENT;
    return -1;
}

int fstat(int fd, struct stat *statbuf) {
    init_shim_fds();
    if (fd < 0 || fd >= MAX_SHIM_FDS || !shim_fds[fd].in_use) {
        errno = EINVAL;
        return -1;
    }
    return stat(shim_fds[fd].path, statbuf);
}

/* -------------------------------------------------------------
 * stdio shims (FILE *)
 * ------------------------------------------------------------- */

FILE *fopen(const char *pathname, const char *mode) {
    int flags = O_RDONLY;
    if (mode[0] == 'w') {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        /* Truncate file by deleting and recreating */
        ipo_delete(pathname);
        ipo_create(pathname, IPO_INODE_TYPE_FILE);
    } else if (mode[0] == 'a') {
        flags = O_WRONLY | O_CREAT | O_APPEND;
    } else if (strchr(mode, '+')) {
        flags = O_RDWR | O_CREAT;
    }

    int fd = open(pathname, flags);
    if (fd < 0) return NULL;

    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    f->fd = fd;
    f->eof = 0;
    f->error = 0;
    f->ungotten = 0;
    f->has_ungot = 0;
    return f;
}

FILE *fdopen(int fd, const char *mode) {
    (void)mode;
    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) return NULL;
    f->fd = fd;
    f->eof = 0;
    f->error = 0;
    f->ungotten = 0;
    f->has_ungot = 0;
    return f;
}

int fclose(FILE *stream) {
    if (!stream) return EOF;
    if (stream == stdin || stream == stdout || stream == stderr) return 0;
    int res = close(stream->fd);
    free(stream);
    return res;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || size == 0 || nmemb == 0 || !stream) return 0;
    size_t total = size * nmemb;
    size_t done = 0;

    if (stream->has_ungot) {
        ((char *)ptr)[0] = (char)stream->ungotten;
        stream->has_ungot = 0;
        done = 1;
    }

    if (done < total) {
        int r = read(stream->fd, (char *)ptr + done, total - done);
        if (r > 0) done += (size_t)r;
        else stream->eof = 1;
    }
    return done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || size == 0 || nmemb == 0 || !stream) return 0;
    size_t total = size * nmemb;
    int w = write(stream->fd, ptr, total);
    if (w > 0) return (size_t)w / size;
    stream->error = 1;
    return 0;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream) return -1;
    stream->has_ungot = 0;
    stream->eof = 0;
    return (lseek(stream->fd, offset, whence) < 0) ? -1 : 0;
}

long ftell(FILE *stream) {
    if (!stream) return -1;
    return lseek(stream->fd, 0, SEEK_CUR);
}

int fgetc(FILE *stream) {
    if (!stream) return EOF;
    if (stream->has_ungot) {
        stream->has_ungot = 0;
        return stream->ungotten;
    }
    char c = 0;
    if (read(stream->fd, &c, 1) == 1) {
        return (unsigned char)c;
    }
    stream->eof = 1;
    return EOF;
}

int getc(FILE *stream) {
    return fgetc(stream);
}

int fputc(int c, FILE *stream) {
    if (!stream) return EOF;
    char ch = (char)c;
    if (write(stream->fd, &ch, 1) == 1) {
        return (unsigned char)c;
    }
    stream->error = 1;
    return EOF;
}

int putc(int c, FILE *stream) {
    return fputc(c, stream);
}

int ungetc(int c, FILE *stream) {
    if (!stream || c == EOF) return EOF;
    stream->ungotten = (unsigned char)c;
    stream->has_ungot = 1;
    stream->eof = 0;
    return (unsigned char)c;
}

char *fgets(char *s, int size, FILE *stream) {
    if (!s || size <= 0 || !stream) return NULL;
    int idx = 0;
    while (idx < size - 1) {
        int ch = fgetc(stream);
        if (ch == EOF) {
            if (idx == 0) return NULL;
            break;
        }
        s[idx++] = (char)ch;
        if (ch == '\n') break;
    }
    s[idx] = '\0';
    return s;
}

int fputs(const char *s, FILE *stream) {
    if (!s || !stream) return EOF;
    size_t len = strlen(s);
    return (write(stream->fd, s, len) == (int)len) ? 0 : EOF;
}

int puts(const char *s) {
    int res = fputs(s, stdout);
    putchar('\n');
    return res;
}

int feof(FILE *stream) {
    return stream ? stream->eof : 1;
}

int ferror(FILE *stream) {
    return stream ? stream->error : 1;
}

int fflush(FILE *stream) {
    (void)stream;
    return 0;
}

/* -------------------------------------------------------------
 * Formatted output
 * ------------------------------------------------------------- */

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    if (!str || size == 0) return 0;
    size_t written = 0;
    const char *p = format;

    while (*p && written < size - 1) {
        if (*p != '%') {
            str[written++] = *p++;
            continue;
        }
        p++;
        if (*p == '%') {
            str[written++] = '%';
            p++;
        } else if (*p == 's') {
            p++;
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && written < size - 1) {
                str[written++] = *s++;
            }
        } else if (*p == 'c') {
            p++;
            char c = (char)va_arg(ap, int);
            str[written++] = c;
        } else if (*p == 'd' || *p == 'i') {
            p++;
            int v = va_arg(ap, int);
            char num_buf[32];
            int n_idx = 0;
            if (v == 0) {
                num_buf[n_idx++] = '0';
            } else {
                unsigned int uv = (v < 0) ? (unsigned int)(-v) : (unsigned int)v;
                if (v < 0) str[written++] = '-';
                char tmp[32];
                int t = 0;
                while (uv > 0) {
                    tmp[t++] = '0' + (uv % 10);
                    uv /= 10;
                }
                while (t > 0 && written < size - 1) {
                    str[written++] = tmp[--t];
                }
            }
            if (n_idx > 0 && written < size - 1) str[written++] = '0';
        } else if (*p == 'u') {
            p++;
            unsigned int v = va_arg(ap, unsigned int);
            char tmp[32];
            int t = 0;
            if (v == 0) tmp[t++] = '0';
            while (v > 0) {
                tmp[t++] = '0' + (v % 10);
                v /= 10;
            }
            while (t > 0 && written < size - 1) {
                str[written++] = tmp[--t];
            }
        } else if (*p == 'x' || *p == 'X' || *p == 'p') {
            p++;
            unsigned int v = va_arg(ap, unsigned int);
            char tmp[32];
            int t = 0;
            if (v == 0) tmp[t++] = '0';
            while (v > 0) {
                int d = v % 16;
                tmp[t++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
                v /= 16;
            }
            while (t > 0 && written < size - 1) {
                str[written++] = tmp[--t];
            }
        } else {
            str[written++] = *p++;
        }
    }
    str[written] = '\0';
    return (int)written;
}

int vfprintf(FILE *stream, const char *format, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), format, ap);
    if (n > 0) {
        write(stream ? stream->fd : 1, buf, (size_t)n);
    }
    return n;
}

int fprintf(FILE *stream, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vfprintf(stream, format, ap);
    va_end(ap);
    return res;
}

int sprintf(char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vsnprintf(str, 65536, format, ap);
    va_end(ap);
    return res;
}

int vprintf(const char *format, va_list ap) {
    return vfprintf(stdout, format, ap);
}

int vsprintf(char *str, const char *format, va_list ap) {
    return vsnprintf(str, 65536, format, ap);
}

/* -------------------------------------------------------------
 * Memory allocation shims
 * ------------------------------------------------------------- */

typedef struct {
    size_t size;
    uint32_t magic;
    uint32_t is_free;
    uint32_t padding;
} shim_kmalloc_block_t;

void *malloc(size_t size) {
    return kmalloc(size);
}

void free(void *ptr) {
    kfree(ptr);
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size == 0) {
        free(ptr);
        return NULL;
    }

    shim_kmalloc_block_t *block = (shim_kmalloc_block_t *)((uint8_t *)ptr - sizeof(shim_kmalloc_block_t));
    size_t old_usable = (block->size > sizeof(shim_kmalloc_block_t)) ? (block->size - sizeof(shim_kmalloc_block_t)) : 0;
    if (old_usable >= new_size) {
        return ptr;
    }

    void *new_p = malloc(new_size);
    if (!new_p) return NULL;
    memcpy(new_p, ptr, old_usable);
    free(ptr);
    return new_p;
}

/* -------------------------------------------------------------
 * stdlib helpers (conversion, qsort, time)
 * ------------------------------------------------------------- */

int atoi(const char *nptr) {
    return (int)strtol(nptr, NULL, 10);
}

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }

    long val = 0;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        val = val * base + d;
        s++;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -val : val;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    return (unsigned long)strtol(nptr, endptr, base);
}

long long strtoll(const char *nptr, char **endptr, int base) {
    return (long long)strtol(nptr, endptr, base);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    return (unsigned long long)strtol(nptr, endptr, base);
}

double strtod(const char *nptr, char **endptr) {
    const char *s = nptr;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    double val = 0.0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10.0 + (*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            val += (*s - '0') * frac;
            frac *= 0.1;
            s++;
        }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int exp_neg = 0;
        if (*s == '-') { exp_neg = 1; s++; }
        else if (*s == '+') { s++; }
        int exp = 0;
        while (*s >= '0' && *s <= '9') {
            exp = exp * 10 + (*s - '0');
            s++;
        }
        double factor = 1.0;
        for (int i = 0; i < exp; i++) factor *= 10.0;
        if (exp_neg) val /= factor;
        else val *= factor;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -val : val;
}

float strtof(const char *nptr, char **endptr) {
    return (float)strtod(nptr, endptr);
}

long double strtold(const char *nptr, char **endptr) {
    return (long double)strtod(nptr, endptr);
}

double ldexp(double x, int exp) {
    while (exp > 0) { x *= 2.0; exp--; }
    while (exp < 0) { x /= 2.0; exp++; }
    return x;
}

long double ldexpl(long double x, int exp) {
    while (exp > 0) { x *= 2.0; exp--; }
    while (exp < 0) { x /= 2.0; exp++; }
    return x;
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    if (nmemb <= 1 || !base || size == 0) return;
    char *arr = (char *)base;
    for (size_t i = 0; i < nmemb - 1; i++) {
        for (size_t j = i + 1; j < nmemb; j++) {
            if (compar(arr + i * size, arr + j * size) > 0) {
                for (size_t k = 0; k < size; k++) {
                    char tmp = arr[i * size + k];
                    arr[i * size + k] = arr[j * size + k];
                    arr[j * size + k] = tmp;
                }
            }
        }
    }
}

char *realpath(const char *path, char *resolved_path) {
    if (!path) return NULL;
    if (!resolved_path) resolved_path = (char *)malloc(128);
    if (!resolved_path) return NULL;
    strncpy(resolved_path, path, 127);
    resolved_path[127] = '\0';
    return resolved_path;
}

char *getenv(const char *name) {
    (void)name;
    return NULL;
}

void exit(int status) {
    asm volatile ("mov $0xFFFF, %%eax\n int $0x80\n" : : "b"(status));
    while (1);
}

void abort(void) {
    exit(1);
}

char *strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *p = (char *)malloc(len + 1);
    if (p) {
        memcpy(p, s, len + 1);
    }
    return p;
}

char *strerror(int errnum) {
    (void)errnum;
    return "Unknown error";
}

time_t time(time_t *t) {
    time_t now = (time_t)(timer_millis() / 1000u);
    if (t) *t = now;
    return now;
}

struct tm *localtime(const time_t *timep) {
    static struct tm t;
    (void)timep;
    memset(&t, 0, sizeof(t));
    t.tm_year = 126; // 2026
    t.tm_mday = 1;
    return &t;
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (tv) {
        uint64_t ms = timer_millis();
        tv->tv_sec = (long)(ms / 1000u);
        tv->tv_usec = (long)((ms % 1000u) * 1000u);
    }
    return 0;
}

static char *empty_environ[] = {NULL};
char **environ = empty_environ;

FILE *freopen(const char *pathname, const char *mode, FILE *stream) {
    if (stream && stream != stdin && stream != stdout && stream != stderr) {
        close(stream->fd);
    }
    int flags = O_RDONLY;
    if (mode[0] == 'w') flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mode[0] == 'a') flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (strchr(mode, '+')) flags = O_RDWR | O_CREAT;

    int fd = open(pathname, flags);
    if (fd < 0) return NULL;
    if (!stream) {
        stream = (FILE *)malloc(sizeof(FILE));
        if (!stream) { close(fd); return NULL; }
    }
    stream->fd = fd;
    stream->eof = 0;
    stream->error = 0;
    stream->ungotten = 0;
    stream->has_ungot = 0;
    return stream;
}

char *strpbrk(const char *s, const char *accept) {
    if (!s || !accept) return NULL;
    while (*s) {
        for (const char *a = accept; *a; a++) {
            if (*s == *a) return (char *)s;
        }
        s++;
    }
    return NULL;
}

int execvp(const char *file, char *const argv[]) {
    (void)file; (void)argv;
    errno = ENOENT;
    return -1;
}

int mprotect(void *addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot;
    return 0;
}

int sigaction(int signum, const void *act, void *oldact) {
    (void)signum; (void)act; (void)oldact;
    return 0;
}

int sigemptyset(void *set) {
    (void)set;
    return 0;
}

int sigaddset(void *set, int signum) {
    (void)set; (void)signum;
    return 0;
}

int sigprocmask(int how, const void *set, void *oldset) {
    (void)how; (void)set; (void)oldset;
    return 0;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) d++;
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dest;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char uc = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == uc) return (void *)(p + i);
    }
    return NULL;
}
