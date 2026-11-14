#include <stdio.h>
#include <stdint.h>
#include <string.h>

static void fmt_emit(void (*emit)(void *, char), void *ctx, char ch, int *count) {
    emit(ctx, ch);
    if (count) (*count)++;
}

static void fmt_repeat(void (*emit)(void *, char), void *ctx, char ch, int n, int *count) {
    for (int i = 0; i < n; i++) {
        fmt_emit(emit, ctx, ch, count);
    }
}

static int fmt_u64_len(uint64_t val, uint32_t base) {
    int len = 1;
    while (val >= base) {
        val /= base;
        len++;
    }
    return len;
}

static void fmt_u64_emit(void (*emit)(void *, char), void *ctx,
                         uint64_t val, uint32_t base, int upper, int *count) {
    if (val >= base) {
        fmt_u64_emit(emit, ctx, val / base, base, upper, count);
    }
    uint32_t d = (uint32_t)(val % base);
    char ch = (d < 10u) ? (char)('0' + d) : (char)((upper ? 'A' : 'a') + (d - 10u));
    fmt_emit(emit, ctx, ch, count);
}

static void fmt_float(void (*emit)(void *, char), void *ctx, double val,
                      int left_align, int zero_pad, int plus_sign, int space_sign,
                      int width, int precision, int *count) {
    uint64_t bits = 0;
    memcpy(&bits, &val, sizeof(bits));

    int is_neg = (bits >> 63) != 0;
    uint64_t exp_bits = (bits >> 52) & 0x7FFULL;
    uint64_t frac_bits = bits & 0x000FFFFFFFFFFFFFULL;
    char sign_ch = is_neg ? '-' : (plus_sign ? '+' : (space_sign ? ' ' : '\0'));

    if (exp_bits == 0x7FFULL) {
        const char *tag = (frac_bits != 0) ? "nan" : "inf";
        int total_len = 3 + (sign_ch ? 1 : 0);
        int pad = (width > total_len) ? (width - total_len) : 0;
        if (!left_align) fmt_repeat(emit, ctx, ' ', pad, count);
        if (sign_ch) fmt_emit(emit, ctx, sign_ch, count);
        for (int i = 0; i < 3; i++) fmt_emit(emit, ctx, tag[i], count);
        if (left_align) fmt_repeat(emit, ctx, ' ', pad, count);
        return;
    }

    if (val < 0.0) val = -val;
    int prec = (precision < 0) ? 6 : (precision > 9 ? 9 : precision);

    if (val >= 18446744073709550000.0) {
        /* Fallback scientific representation to keep (uint64_t) conversion in bounds */
        int exp10 = 0;
        while (val >= 10.0) {
            val /= 10.0;
            exp10++;
        }
        uint64_t scale = 1ULL;
        for (int i = 0; i < prec; i++) scale *= 10ULL;
        uint64_t scaled = (uint64_t)((val - (uint64_t)val) * (double)scale + 0.5);
        uint64_t lead = (uint64_t)val;
        if (scaled >= scale) {
            lead++;
            scaled = 0ULL;
        }
        if (sign_ch) fmt_emit(emit, ctx, sign_ch, count);
        fmt_u64_emit(emit, ctx, lead, 10u, 0, count);
        if (prec > 0) {
            fmt_emit(emit, ctx, '.', count);
            uint64_t div = scale / 10ULL;
            for (int i = 0; i < prec; i++) {
                fmt_emit(emit, ctx, (char)('0' + ((scaled / div) % 10ULL)), count);
                div /= 10ULL;
            }
        }
        fmt_emit(emit, ctx, 'e', count);
        fmt_emit(emit, ctx, '+', count);
        if (exp10 < 10) fmt_emit(emit, ctx, '0', count);
        fmt_u64_emit(emit, ctx, (uint64_t)exp10, 10u, 0, count);
        return;
    }

    uint64_t scale = 1ULL;
    for (int i = 0; i < prec; i++) scale *= 10ULL;

    uint64_t whole = (uint64_t)val;
    double fraction = val - (double)whole;
    uint64_t scaled = (prec > 0) ? (uint64_t)(fraction * (double)scale + 0.5) : 0ULL;
    if (prec > 0 && scaled >= scale) {
        whole++;
        scaled -= scale;
    } else if (prec == 0 && fraction >= 0.5) {
        whole++;
    }

    int total_len = (sign_ch ? 1 : 0) + fmt_u64_len(whole, 10u) + (prec > 0 ? (1 + prec) : 0);
    int pad = (width > total_len) ? (width - total_len) : 0;

    if (!left_align && !zero_pad) fmt_repeat(emit, ctx, ' ', pad, count);
    if (sign_ch) fmt_emit(emit, ctx, sign_ch, count);
    if (!left_align && zero_pad) fmt_repeat(emit, ctx, '0', pad, count);

    fmt_u64_emit(emit, ctx, whole, 10u, 0, count);
    if (prec > 0) {
        fmt_emit(emit, ctx, '.', count);
        uint64_t div = scale / 10ULL;
        for (int i = 0; i < prec; i++) {
            fmt_emit(emit, ctx, (char)('0' + ((scaled / div) % 10ULL)), count);
            div /= 10ULL;
        }
    }
    if (left_align) fmt_repeat(emit, ctx, ' ', pad, count);
}

int stdio_vformat(void (*emit)(void *ctx, char ch), void *ctx, const char *format, va_list args) {
    if (!emit || !format) return 0;

    int count = 0;
    while (*format) {
        if (*format != '%' || !*(format + 1)) {
            fmt_emit(emit, ctx, *format, &count);
            format++;
            continue;
        }
        format++;

        int left_align = 0;
        int zero_pad = 0;
        int plus_sign = 0;
        int space_sign = 0;
        while (*format == '-' || *format == '0' || *format == '+' || *format == ' ') {
            if (*format == '-') left_align = 1;
            else if (*format == '0') zero_pad = 1;
            else if (*format == '+') plus_sign = 1;
            else if (*format == ' ') space_sign = 1;
            format++;
        }
        if (left_align) zero_pad = 0;
        if (plus_sign) space_sign = 0;

        int width = 0;
        while (*format >= '0' && *format <= '9') {
            width = width * 10 + (*format - '0');
            format++;
        }

        int precision = -1;
        if (*format == '.') {
            format++;
            precision = 0;
            while (*format >= '0' && *format <= '9') {
                precision = precision * 10 + (*format - '0');
                format++;
            }
        }

        int is_long = 0;
        int is_long_long = 0;
        if (*format == 'l' && *(format + 1) == 'l') {
            is_long_long = 1;
            format += 2;
        } else if (*format == 'l' || *format == 'z') {
            is_long = 1;
            format++;
        }

        if (!*format) break;

        switch (*format) {
            case 'i':
            case 'd': {
                int is_neg = 0;
                uint64_t abs_val = 0;
                if (is_long_long) {
                    long long val = va_arg(args, long long);
                    is_neg = (val < 0);
                    abs_val = is_neg ? (0ULL - (uint64_t)val) : (uint64_t)val;
                } else if (is_long) {
                    long val = va_arg(args, long);
                    is_neg = (val < 0);
                    abs_val = is_neg ? (0ULL - (uint64_t)(long long)val) : (uint64_t)(unsigned long)val;
                } else {
                    int val = va_arg(args, int);
                    is_neg = (val < 0);
                    abs_val = is_neg ? (0ULL - (uint64_t)(long long)val) : (uint64_t)(unsigned int)val;
                }

                char sign_ch = is_neg ? '-' : (plus_sign ? '+' : (space_sign ? ' ' : '\0'));
                int dlen = fmt_u64_len(abs_val, 10u);
                int total_len = dlen + (sign_ch ? 1 : 0);
                int pad = (width > total_len) ? (width - total_len) : 0;

                if (!left_align && !zero_pad) fmt_repeat(emit, ctx, ' ', pad, &count);
                if (sign_ch) fmt_emit(emit, ctx, sign_ch, &count);
                if (!left_align && zero_pad) fmt_repeat(emit, ctx, '0', pad, &count);
                fmt_u64_emit(emit, ctx, abs_val, 10u, 0, &count);
                if (left_align) fmt_repeat(emit, ctx, ' ', pad, &count);
                break;
            }

            case 'u':
            case 'x':
            case 'X': {
                uint64_t val = is_long_long
                                   ? va_arg(args, uint64_t)
                                   : (is_long ? (uint64_t)va_arg(args, unsigned long)
                                              : (uint64_t)va_arg(args, unsigned int));
                uint32_t base = (*format == 'u') ? 10u : 16u;
                int dlen = fmt_u64_len(val, base);
                int pad = (width > dlen) ? (width - dlen) : 0;
                char pad_ch = zero_pad ? '0' : ' ';

                if (!left_align) fmt_repeat(emit, ctx, pad_ch, pad, &count);
                fmt_u64_emit(emit, ctx, val, base, (*format == 'X'), &count);
                if (left_align) fmt_repeat(emit, ctx, ' ', pad, &count);
                break;
            }

            case 'f': {
                double val = va_arg(args, double);
                fmt_float(emit, ctx, val, left_align, zero_pad, plus_sign, space_sign,
                          width, precision, &count);
                break;
            }

            case 'p': {
                uint32_t val = (uint32_t)(uintptr_t)va_arg(args, void *);
                int total_len = 2 + fmt_u64_len((uint64_t)val, 16u);
                int pad = (width > total_len) ? (width - total_len) : 0;
                if (!left_align) fmt_repeat(emit, ctx, ' ', pad, &count);
                fmt_emit(emit, ctx, '0', &count);
                fmt_emit(emit, ctx, 'x', &count);
                fmt_u64_emit(emit, ctx, (uint64_t)val, 16u, 0, &count);
                if (left_align) fmt_repeat(emit, ctx, ' ', pad, &count);
                break;
            }

            case 'c': {
                char val = (char)va_arg(args, int);
                int pad = (width > 1) ? (width - 1) : 0;
                if (!left_align) fmt_repeat(emit, ctx, ' ', pad, &count);
                fmt_emit(emit, ctx, val, &count);
                if (left_align) fmt_repeat(emit, ctx, ' ', pad, &count);
                break;
            }

            case 's': {
                const char *str = va_arg(args, const char *);
                if (!str) str = "(null)";
                int len = 0;
                while (str[len] && (precision < 0 || len < precision)) len++;
                int pad = (width > len) ? (width - len) : 0;
                if (!left_align) fmt_repeat(emit, ctx, ' ', pad, &count);
                for (int i = 0; i < len; i++) fmt_emit(emit, ctx, str[i], &count);
                if (left_align) fmt_repeat(emit, ctx, ' ', pad, &count);
                break;
            }

            case '%': {
                int pad = (width > 1) ? (width - 1) : 0;
                if (!left_align) fmt_repeat(emit, ctx, ' ', pad, &count);
                fmt_emit(emit, ctx, '%', &count);
                if (left_align) fmt_repeat(emit, ctx, ' ', pad, &count);
                break;
            }

            default: {
                int pad = (width > 2) ? (width - 2) : 0;
                if (!left_align) fmt_repeat(emit, ctx, ' ', pad, &count);
                fmt_emit(emit, ctx, '%', &count);
                fmt_emit(emit, ctx, *format, &count);
                if (left_align) fmt_repeat(emit, ctx, ' ', pad, &count);
                break;
            }
        }
        format++;
    }
    return count;
}

static void printf_emit_putchar(void *ctx, char ch) {
    (void)ctx;
    putchar(ch);
}

int printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int count = stdio_vformat(printf_emit_putchar, NULL, format, args);
    va_end(args);
    return count;
}
