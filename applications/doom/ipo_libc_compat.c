/**
 * ipo_libc_compat.c - Minimal Libc compatibility layer for DOOM on IPO_OS
 *
 * Provides:
 * - Proper vsnprintf/snprintf with full precision padding (e.g. "%.3d" -> "033")
 * - Environment variable array stub
 * - Safe exit handler with text mode restoration
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <syscall.h>

extern void I_ShutdownGraphics(void);

/* Default environment array for libc getenv() */
static char s_home_env[] = "HOME=/";
static char s_wad_env[] = "DOOMWADDIR=";
static char *s_default_environ[] = {
    s_home_env,
    s_wad_env,
    NULL
};
char **environ = s_default_environ;

char *sndserver_filename = "./sndserver";
int mb_used = 6;

void setbuf(FILE *stream, char *buf) {
    (void)stream;
    (void)buf;
}

void exit(int status) {
    I_ShutdownGraphics();
    if (status != 0) {
        printf("\n[DOOM] exit(%d) called!\n", status);
    }
    ipo_exit(status);
    for (;;) {}
}

static void ull_to_str(unsigned long long val, int base, int uppercase, char *buf, int *len) {
    char digits_lower[] = "0123456789abcdef";
    char digits_upper[] = "0123456789ABCDEF";
    const char *digits = uppercase ? digits_upper : digits_lower;
    char tmp[65];
    int pos = 0;
    int i;

    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        *len = 1;
        return;
    }

    while (val > 0) {
        tmp[pos++] = digits[val % base];
        val /= base;
    }

    for (i = 0; i < pos; i++) {
        buf[i] = tmp[pos - 1 - i];
    }
    buf[pos] = '\0';
    *len = pos;
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    size_t written = 0;
    const char *p = format;

    #define PUTC(c) do { \
        if (str && written + 1 < size) { \
            str[written] = (c); \
        } \
        written++; \
    } while (0)

    while (*p) {
        int left_align = 0;
        int plus_sign = 0;
        int space_sign = 0;
        int zero_pad = 0;
        int hash = 0;
        int width = 0;
        int prec = -1;
        int is_long = 0;
        int is_long_long = 0;
        char spec;
        int i;

        if (*p != '%') {
            PUTC(*p++);
            continue;
        }
        p++; /* Skip '%' */

        /* Flags */
        while (1) {
            if (*p == '-') left_align = 1;
            else if (*p == '+') plus_sign = 1;
            else if (*p == ' ') space_sign = 1;
            else if (*p == '0') zero_pad = 1;
            else if (*p == '#') hash = 1;
            else break;
            p++;
        }
        if (left_align) zero_pad = 0;

        /* Field width */
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left_align = 1;
                width = -width;
            }
            p++;
        } else {
            while (isdigit((unsigned char)*p)) {
                width = width * 10 + (*p++ - '0');
            }
        }

        /* Precision */
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                prec = va_arg(ap, int);
                p++;
            } else {
                while (isdigit((unsigned char)*p)) {
                    prec = prec * 10 + (*p++ - '0');
                }
            }
        }

        /* Length modifiers */
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
        } else if (*p == 'h') {
            p++;
            if (*p == 'h') p++;
        }

        spec = *p++;
        if (!spec) break;

        if (spec == '%') {
            PUTC('%');
            continue;
        }

        if (spec == 's') {
            const char *s = va_arg(ap, const char *);
            int slen;
            int pad;
            if (!s) s = "(null)";
            slen = (int)strlen(s);
            if (prec >= 0 && prec < slen) slen = prec;
            pad = (width > slen) ? width - slen : 0;
            if (!left_align) {
                for (i = 0; i < pad; i++) PUTC(' ');
            }
            for (i = 0; i < slen; i++) PUTC(s[i]);
            if (left_align) {
                for (i = 0; i < pad; i++) PUTC(' ');
            }
            continue;
        }

        if (spec == 'c') {
            char c = (char)va_arg(ap, int);
            int pad = (width > 1) ? width - 1 : 0;
            if (!left_align) {
                for (i = 0; i < pad; i++) PUTC(' ');
            }
            PUTC(c);
            if (left_align) {
                for (i = 0; i < pad; i++) PUTC(' ');
            }
            continue;
        }

        /* Numeric specs */
        if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'x' || spec == 'X' || spec == 'p') {
            char num_buf[65];
            int num_len = 0;
            char sign_char = 0;
            unsigned long long uval;
            int zeros = 0;
            int total_len;
            int spaces;

            if (spec == 'p') {
                uintptr_t ptr = (uintptr_t)va_arg(ap, void *);
                uval = (unsigned long long)ptr;
                ull_to_str(uval, 16, 0, num_buf, &num_len);
                PUTC('0'); PUTC('x');
                for (i = 0; i < num_len; i++) PUTC(num_buf[i]);
                continue;
            }

            if (spec == 'd' || spec == 'i') {
                long long sval = is_long_long ? va_arg(ap, long long) :
                                 (is_long ? va_arg(ap, long) : va_arg(ap, int));
                if (sval < 0) {
                    sign_char = '-';
                    uval = (unsigned long long)(-sval);
                } else {
                    if (plus_sign) sign_char = '+';
                    else if (space_sign) sign_char = ' ';
                    uval = (unsigned long long)sval;
                }
                ull_to_str(uval, 10, 0, num_buf, &num_len);
            } else {
                int base = (spec == 'u') ? 10 : 16;
                uval = is_long_long ? va_arg(ap, unsigned long long) :
                       (is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
                ull_to_str(uval, base, (spec == 'X'), num_buf, &num_len);
            }

            /* If precision is given for integers, zero-padding takes precedence over '0' flag */
            if (prec >= 0) {
                zero_pad = 0;
                if (prec > num_len) zeros = prec - num_len;
                if (prec == 0 && uval == 0) num_len = 0;
            }

            total_len = num_len + zeros + (sign_char ? 1 : 0);
            spaces = (width > total_len) ? width - total_len : 0;

            if (!left_align && !zero_pad) {
                for (i = 0; i < spaces; i++) PUTC(' ');
            }
            if (sign_char) PUTC(sign_char);
            if (!left_align && zero_pad) {
                for (i = 0; i < spaces; i++) PUTC('0');
            }
            for (i = 0; i < zeros; i++) PUTC('0');
            for (i = 0; i < num_len; i++) PUTC(num_buf[i]);
            if (left_align) {
                for (i = 0; i < spaces; i++) PUTC(' ');
            }
            continue;
        }

        /* Unknown specifier fallback */
        PUTC(spec);
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
