#include <stdio.h>

typedef struct {
    char *buf;
    size_t max_write;
    size_t pos;
} snp_ctx_t;

static void snp_emit(void *ctx, char ch) {
    snp_ctx_t *s = (snp_ctx_t *)ctx;
    if (s->buf && s->pos < s->max_write) {
        s->buf[s->pos] = ch;
    }
    s->pos++;
}

__attribute__((weak))
int vsnprintf(char *buf, size_t size, const char *format, va_list args) {
    snp_ctx_t ctx = {
        .buf = buf,
        .max_write = (buf && size > 0) ? (size - 1u) : 0u,
        .pos = 0
    };
    int total = stdio_vformat(snp_emit, &ctx, format, args);
    if (buf && size > 0) {
        size_t nul_pos = (ctx.pos < size) ? ctx.pos : (size - 1u);
        buf[nul_pos] = '\0';
    }
    return total;
}

int snprintf(char *buf, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vsnprintf(buf, size, format, args);
    va_end(args);
    return ret;
}
