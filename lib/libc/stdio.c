#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>

static FILE **file_pool = NULL;
static size_t file_pool_count = 0;
static size_t file_pool_cap = 0;

static FILE std_streams[3] = {
    { .fd = 0, .eof = 0, .error = 0, .ungotten = 0, .has_ungot = 0 },
    { .fd = 1, .eof = 0, .error = 0, .ungotten = 0, .has_ungot = 0 },
    { .fd = 2, .eof = 0, .error = 0, .ungotten = 0, .has_ungot = 0 }
};

FILE *stdin = &std_streams[0];
FILE *stdout = &std_streams[1];
FILE *stderr = &std_streams[2];

FILE *fopen(const char *pathname, const char *mode) {
    if (!pathname || !mode) {
        errno = EINVAL;
        return NULL;
    }

    int flags = 0;
    if (strchr(mode, 'w')) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        if (strchr(mode, '+')) flags = O_RDWR | O_CREAT | O_TRUNC;
    } else if (strchr(mode, 'a')) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        if (strchr(mode, '+')) flags = O_RDWR | O_CREAT | O_APPEND;
    } else { // 'r'
        flags = O_RDONLY;
        if (strchr(mode, '+')) flags = O_RDWR;
    }

    int fd = open(pathname, flags, 0666);
    if (fd < 0) return NULL;

    return fdopen(fd, mode);
}

FILE *fdopen(int fd, const char *mode) {
    (void)mode;
    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }

    FILE *f = (FILE *)calloc(1, sizeof(FILE));
    if (!f) {
        errno = ENOMEM;
        return NULL;
    }
    f->fd = fd;
    f->eof = 0;
    f->error = 0;
    f->ungotten = 0;
    f->has_ungot = 0;

    if (file_pool_count >= file_pool_cap) {
        size_t new_cap = (file_pool_cap == 0) ? 16 : file_pool_cap * 2;
        FILE **new_pool = (FILE **)realloc(file_pool, new_cap * sizeof(FILE *));
        if (!new_pool) {
            free(f);
            errno = ENOMEM;
            return NULL;
        }
        file_pool = new_pool;
        file_pool_cap = new_cap;
    }

    file_pool[file_pool_count++] = f;
    return f;
}

FILE *freopen(const char *pathname, const char *mode, FILE *stream) {
    if (stream) {
        fclose(stream);
    }
    return fopen(pathname, mode);
}

int fclose(FILE *stream) {
    if (!stream) {
        errno = EBADF;
        return EOF;
    }

    int res = close(stream->fd);
    for (size_t i = 0; i < file_pool_count; i++) {
        if (file_pool[i] == stream) {
            file_pool[i] = file_pool[file_pool_count - 1];
            file_pool_count--;
            free(stream);
            break;
        }
    }
    return (res < 0) ? EOF : 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || size == 0 || nmemb == 0 || !stream) return 0;
    size_t total = size * nmemb;
    size_t bytes_read = 0;
    char *out = (char *)ptr;

    if (stream->has_ungot) {
        *out++ = (char)stream->ungotten;
        stream->has_ungot = 0;
        bytes_read++;
    }

    while (bytes_read < total) {
        ssize_t n = read(stream->fd, out, total - bytes_read);
        if (n < 0) {
            stream->error = 1;
            break;
        }
        if (n == 0) {
            stream->eof = 1;
            break;
        }
        bytes_read += n;
        out += n;
    }

    return bytes_read / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || size == 0 || nmemb == 0 || !stream) return 0;
    size_t total = size * nmemb;
    size_t bytes_written = 0;
    const char *in = (const char *)ptr;

    while (bytes_written < total) {
        ssize_t n = write(stream->fd, in, total - bytes_written);
        if (n <= 0) {
            stream->error = 1;
            break;
        }
        bytes_written += n;
        in += n;
    }

    return bytes_written / size;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream) {
        errno = EBADF;
        return -1;
    }
    stream->has_ungot = 0;
    off_t res = lseek(stream->fd, offset, whence);
    if (res == (off_t)-1) {
        stream->error = 1;
        return -1;
    }
    stream->eof = 0;
    return 0;
}

long ftell(FILE *stream) {
    if (!stream) {
        errno = EBADF;
        return -1;
    }
    off_t pos = lseek(stream->fd, 0, SEEK_CUR);
    if (pos == (off_t)-1) return -1;
    if (stream->has_ungot) pos--;
    return (long)pos;
}

int fseeko(FILE *stream, off_t offset, int whence) {
    return fseek(stream, (long)offset, whence);
}

off_t ftello(FILE *stream) {
    return (off_t)ftell(stream);
}

FILE *popen(const char *command, const char *type) {
    (void)command;
    (void)type;
    return NULL;
}

int pclose(FILE *stream) {
    (void)stream;
    return 0;
}

void rewind(FILE *stream) {
    if (stream) {
        fseek(stream, 0, SEEK_SET);
        stream->error = 0;
    }
}

int fgetc(FILE *stream) {
    if (!stream) return EOF;
    if (stream->has_ungot) {
        stream->has_ungot = 0;
        return stream->ungotten;
    }
    unsigned char ch;
    ssize_t n = read(stream->fd, &ch, 1);
    if (n <= 0) {
        if (n == 0) stream->eof = 1;
        else stream->error = 1;
        return EOF;
    }
    return (int)ch;
}

int getc(FILE *stream) {
    return fgetc(stream);
}

int fputc(int c, FILE *stream) {
    if (!stream) return EOF;
    unsigned char ch = (unsigned char)c;
    ssize_t n = write(stream->fd, &ch, 1);
    if (n != 1) {
        stream->error = 1;
        return EOF;
    }
    return (int)ch;
}

int putc(int c, FILE *stream) {
    return fputc(c, stream);
}

int putchar(int c) {
    return fputc(c, stdout);
}

int getchar(void) {
    return fgetc(stdin);
}

int ungetc(int c, FILE *stream) {
    if (c == EOF || !stream || stream->has_ungot) return EOF;
    stream->ungotten = (unsigned char)c;
    stream->has_ungot = 1;
    stream->eof = 0;
    return stream->ungotten;
}

char *fgets(char *s, int size, FILE *stream) {
    if (!s || size <= 0 || !stream) return NULL;
    int idx = 0;
    while (idx < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (idx == 0) return NULL;
            break;
        }
        s[idx++] = (char)c;
        if (c == '\n') break;
    }
    s[idx] = '\0';
    return s;
}

int fputs(const char *s, FILE *stream) {
    if (!s || !stream) return EOF;
    size_t len = strlen(s);
    if (fwrite(s, 1, len, stream) != len) return EOF;
    return 0;
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    if (putchar('\n') == EOF) return EOF;
    return 0;
}

int feof(FILE *stream) {
    return stream ? stream->eof : 0;
}

int ferror(FILE *stream) {
    return stream ? stream->error : 0;
}

void clearerr(FILE *stream) {
    if (stream) {
        stream->eof = 0;
        stream->error = 0;
    }
}

int fflush(FILE *stream) {
    (void)stream;
    return 0;
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    (void)stream; (void)buf; (void)mode; (void)size;
    return 0;
}

static int tmp_counter = 0;
static char tmpnam_buf[L_tmpnam];

char *tmpnam(char *s) {
    char *out = s ? s : tmpnam_buf;
    snprintf(out, L_tmpnam, "/tmp/tmp_%d.tmp", ++tmp_counter);
    return out;
}

FILE *tmpfile(void) {
    char name[L_tmpnam];
    tmpnam(name);
    return fopen(name, "w+");
}

int remove(const char *pathname) {
    return unlink(pathname);
}

int rename(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath;
    errno = ENOSYS;
    return -1;
}

void perror(const char *s) {
    if (s && *s) {
        fputs(s, stderr);
        fputs(": ", stderr);
    }
    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}

/* Printf implementations */

static void int_to_str(long long val, int base, int uppercase, char *buf, int *len) {
    char digits[] = "0123456789abcdef0123456789ABCDEF";
    char *d = uppercase ? &digits[16] : &digits[0];
    char temp[64];
    int tlen = 0;
    unsigned long long uval;

    if (val < 0 && base == 10) {
        uval = (unsigned long long)(-val);
    } else {
        uval = (unsigned long long)val;
    }

    if (uval == 0) {
        temp[tlen++] = '0';
    } else {
        while (uval > 0) {
            temp[tlen++] = d[uval % base];
            uval /= base;
        }
    }

    *len = tlen;
    for (int i = 0; i < tlen; i++) {
        buf[i] = temp[tlen - 1 - i];
    }
    buf[tlen] = '\0';
}

static void float_to_str(double val, int prec, char *buf) {
    if (isnan(val)) {
        strcpy(buf, "nan");
        return;
    }
    if (isinf(val)) {
        if (val < 0) strcpy(buf, "-inf");
        else strcpy(buf, "inf");
        return;
    }

    if (prec < 0) prec = 6;
    if (val < 0) {
        *buf++ = '-';
        val = -val;
    }

    double int_part;
    double frac_part = modf(val, &int_part);

    long long ip = (long long)int_part;
    int len = 0;
    int_to_str(ip, 10, 0, buf, &len);
    buf += len;

    if (prec > 0) {
        *buf++ = '.';
        for (int i = 0; i < prec; i++) {
            frac_part *= 10.0;
            int digit = (int)frac_part;
            *buf++ = '0' + (digit % 10);
            frac_part -= digit;
        }
    }
    *buf = '\0';
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    size_t written = 0;

    void append_char(char c) {
        if (str && written + 1 < size) {
            str[written] = c;
        }
        written++;
    }

    void append_str(const char *s, int width, int prec, int left_align, char pad) {
        int len = (int)strlen(s);
        if (prec >= 0 && len > prec) len = prec;
        int pad_len = width > len ? width - len : 0;

        if (!left_align) {
            for (int i = 0; i < pad_len; i++) append_char(pad);
        }
        for (int i = 0; i < len; i++) append_char(s[i]);
        if (left_align) {
            for (int i = 0; i < pad_len; i++) append_char(' ');
        }
    }

    const char *p = format;
    while (*p) {
        if (*p != '%') {
            append_char(*p++);
            continue;
        }
        p++; // skip '%'

        int left_align = 0;
        int plus_sign = 0;
        int space_sign = 0;
        char pad_char = ' ';

        while (*p == '-' || *p == '+' || *p == ' ' || *p == '0' || *p == '#') {
            if (*p == '-') left_align = 1;
            else if (*p == '+') plus_sign = 1;
            else if (*p == ' ') space_sign = 1;
            else if (*p == '0') pad_char = '0';
            p++;
        }
        if (left_align) pad_char = ' ';

        int width = 0;
        if (*p == '*') {
            width = va_arg(ap, int);
            p++;
        } else {
            while (isdigit(*p)) {
                width = width * 10 + (*p - '0');
                p++;
            }
        }

        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                prec = va_arg(ap, int);
                p++;
            } else {
                while (isdigit(*p)) {
                    prec = prec * 10 + (*p - '0');
                    p++;
                }
            }
        }

        int is_long = 0;
        int is_long_long = 0;
        if (*p == 'l') {
            p++;
            if (*p == 'l') {
                is_long_long = 1;
                p++;
            } else {
                is_long = 1;
            }
        } else if (*p == 'z') {
            is_long = 1;
            p++;
        }

        char spec = *p++;
        char buf[128];
        int num_len = 0;

        switch (spec) {
            case 'd':
            case 'i': {
                long long val = is_long_long ? va_arg(ap, long long) :
                                (is_long ? va_arg(ap, long) : va_arg(ap, int));
                int neg = (val < 0);
                int_to_str(val, 10, 0, buf, &num_len);
                char sign_buf[2] = {0};
                if (neg) sign_buf[0] = '-';
                else if (plus_sign) sign_buf[0] = '+';
                else if (space_sign) sign_buf[0] = ' ';

                if (sign_buf[0]) {
                    if (width > 0) width--;
                    if (pad_char == '0') {
                        append_char(sign_buf[0]);
                        append_str(buf, width, prec, left_align, '0');
                    } else {
                        if (!left_align && width > num_len) {
                            for (int i = 0; i < width - num_len; i++) append_char(' ');
                            append_char(sign_buf[0]);
                            for (int i = 0; i < num_len; i++) append_char(buf[i]);
                        } else {
                            append_char(sign_buf[0]);
                            append_str(buf, width, prec, left_align, ' ');
                        }
                    }
                } else {
                    append_str(buf, width, prec, left_align, pad_char);
                }
                break;
            }
            case 'u': {
                unsigned long long val = is_long_long ? va_arg(ap, unsigned long long) :
                                         (is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
                int_to_str((long long)val, 10, 0, buf, &num_len);
                append_str(buf, width, prec, left_align, pad_char);
                break;
            }
            case 'x':
            case 'X': {
                unsigned long long val = is_long_long ? va_arg(ap, unsigned long long) :
                                         (is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
                int_to_str((long long)val, 16, (spec == 'X'), buf, &num_len);
                append_str(buf, width, prec, left_align, pad_char);
                break;
            }
            case 'o': {
                unsigned long long val = is_long_long ? va_arg(ap, unsigned long long) :
                                         (is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
                int_to_str((long long)val, 8, 0, buf, &num_len);
                append_str(buf, width, prec, left_align, pad_char);
                break;
            }
            case 'p': {
                void *ptr = va_arg(ap, void *);
                int_to_str((uintptr_t)ptr, 16, 0, buf, &num_len);
                char pbuf[64] = "0x";
                strcat(pbuf, buf);
                append_str(pbuf, width, prec, left_align, ' ');
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                append_str(s, width, prec, left_align, ' ');
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                char s[2] = { c, '\0' };
                append_str(s, width, -1, left_align, ' ');
                break;
            }
            case 'f':
            case 'g':
            case 'e': {
                double val = va_arg(ap, double);
                float_to_str(val, prec, buf);
                append_str(buf, width, -1, left_align, ' ');
                break;
            }
            case '%': {
                append_char('%');
                break;
            }
            default:
                append_char(spec);
                break;
        }
    }

    if (str && size > 0) {
        if (written < size) {
            str[written] = '\0';
        } else {
            str[size - 1] = '\0';
        }
    }
    return (int)written;
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vsnprintf(str, size, format, ap);
    va_end(ap);
    return res;
}

int sprintf(char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vsnprintf(str, (size_t)-1, format, ap);
    va_end(ap);
    return res;
}

int vsprintf(char *str, const char *format, va_list ap) {
    return vsnprintf(str, (size_t)-1, format, ap);
}

int vfprintf(FILE *stream, const char *format, va_list ap) {
    char buf[1024];
    va_list aq;
    va_copy(aq, ap);
    int len = vsnprintf(buf, sizeof(buf), format, aq);
    va_end(aq);

    if (len < (int)sizeof(buf)) {
        return (int)fwrite(buf, 1, len, stream);
    } else {
        char *dyn = (char *)malloc(len + 1);
        if (!dyn) return -1;
        vsnprintf(dyn, len + 1, format, ap);
        int written = (int)fwrite(dyn, 1, len, stream);
        free(dyn);
        return written;
    }
}

int fprintf(FILE *stream, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vfprintf(stream, format, ap);
    va_end(ap);
    return res;
}

int vprintf(const char *format, va_list ap) {
    return vfprintf(stdout, format, ap);
}

int printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vprintf(format, ap);
    va_end(ap);
    return res;
}

/* Scanf implementations */

int vsscanf(const char *str, const char *format, va_list ap) {
    int matched = 0;
    const char *s = str;
    const char *f = format;

    while (*f && *s) {
        if (isspace(*f)) {
            while (isspace(*f)) f++;
            while (isspace(*s)) s++;
            continue;
        }

        if (*f != '%') {
            if (*f != *s) break;
            f++;
            s++;
            continue;
        }

        f++; // Skip '%'
        if (*f == '%') {
            if (*s != '%') break;
            f++;
            s++;
            continue;
        }

        int is_long = 0;
        if (*f == 'l') {
            is_long = 1;
            f++;
        }

        char spec = *f++;
        while (isspace(*s) && spec != 'c') s++;

        switch (spec) {
            case 'd':
            case 'i': {
                char *end;
                long val = strtol(s, &end, (spec == 'i' ? 0 : 10));
                if (end == s) return matched;
                if (is_long) *va_arg(ap, long *) = val;
                else *va_arg(ap, int *) = (int)val;
                s = end;
                matched++;
                break;
            }
            case 'u': {
                char *end;
                unsigned long val = strtoul(s, &end, 10);
                if (end == s) return matched;
                if (is_long) *va_arg(ap, unsigned long *) = val;
                else *va_arg(ap, unsigned int *) = (unsigned int)val;
                s = end;
                matched++;
                break;
            }
            case 'x':
            case 'X': {
                char *end;
                unsigned long val = strtoul(s, &end, 16);
                if (end == s) return matched;
                if (is_long) *va_arg(ap, unsigned long *) = val;
                else *va_arg(ap, unsigned int *) = (unsigned int)val;
                s = end;
                matched++;
                break;
            }
            case 's': {
                char *out = va_arg(ap, char *);
                int idx = 0;
                while (*s && !isspace(*s)) {
                    out[idx++] = *s++;
                }
                out[idx] = '\0';
                if (idx > 0) matched++;
                break;
            }
            case 'c': {
                char *out = va_arg(ap, char *);
                *out = *s++;
                matched++;
                break;
            }
            case 'f': {
                char *end;
                double val = strtod(s, &end);
                if (end == s) return matched;
                if (is_long) *va_arg(ap, double *) = val;
                else *va_arg(ap, float *) = (float)val;
                s = end;
                matched++;
                break;
            }
            default:
                return matched;
        }
    }
    return matched;
}

int sscanf(const char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vsscanf(str, format, ap);
    va_end(ap);
    return res;
}

int fscanf(FILE *stream, const char *format, ...) {
    char line[1024];
    if (!fgets(line, sizeof(line), stream)) return EOF;
    va_list ap;
    va_start(ap, format);
    int res = vsscanf(line, format, ap);
    va_end(ap);
    return res;
}

int scanf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = fscanf(stdin, format, ap);
    va_end(ap);
    return res;
}
