#include <system/rtc.h>
#include <ioport.h>
#include <kernel/irq.h>
#include <kernel/smp.h>
#include <memory/kmalloc.h>
#include <string.h>

#define CMOS_ADDR_PORT        0x70u
#define CMOS_DATA_PORT        0x71u

#define RTC_REG_SECONDS       0x00u
#define RTC_REG_MINUTES       0x02u
#define RTC_REG_HOURS         0x04u
#define RTC_REG_WEEKDAY       0x06u
#define RTC_REG_DAY           0x07u
#define RTC_REG_MONTH         0x08u
#define RTC_REG_YEAR          0x09u
#define RTC_REG_STATUS_A      0x0Au
#define RTC_REG_STATUS_B      0x0Bu
#define RTC_REG_STATUS_C      0x0Cu
#define RTC_REG_STATUS_D      0x0Du
#define RTC_REG_DEFAULT_CENTURY 0x32u

#define RTC_A_UIP             0x80u
#define RTC_A_DV_MASK         0x70u
#define RTC_A_DV_32KHZ        0x20u
#define RTC_A_RATE_1024HZ     0x06u

#define RTC_B_DSE             0x01u
#define RTC_B_24H             0x02u
#define RTC_B_DM_BIN          0x04u
#define RTC_B_UIE             0x10u
#define RTC_B_AIE             0x20u
#define RTC_B_PIE             0x40u
#define RTC_B_SET             0x80u

#define RTC_C_UF              0x10u
#define RTC_C_PF              0x40u

#define RTC_D_VRT             0x80u

#define RTC_UIP_POLL_LIMIT    100000u
#define RTC_SNAPSHOT_RETRIES  16u
#define RTC_BIG_BASE          1000000000u

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} rtc_strbuf_t;

typedef struct {
    uint8_t sec;
    uint8_t min;
    uint8_t hr;
    uint8_t dow;
    uint8_t day;
    uint8_t mon;
    uint8_t yr;
    uint8_t cen;
    uint8_t reg_a;
    uint8_t reg_b;
    uint8_t reg_c;
    uint8_t reg_d;
} rtc_raw_hw_t;

typedef struct {
    uint32_t year;
    uint32_t month;
    uint32_t day;
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
} rtc_hw_fields_t;

typedef enum {
    RTC_MM_MONTH = 0,
    RTC_MM_MINUTE = 1
} rtc_mm_kind_t;

/* Hardware registers and IRQ8 counters protected solely by s_rtc_hw_lock. */
static spinlock_t s_rtc_hw_lock = {0};
/* Subsystem cache, format strings, and state protected by s_rtc_state_lock. */
static spinlock_t s_rtc_state_lock = {0};

static volatile bool s_rtc_initialized = false;
static volatile bool s_rtc_irq8_registered = false;
static volatile bool s_rtc_uf_dirty = false;
static volatile bool s_rtc_uses_subsecond = false;
static volatile uint32_t s_rtc_irq8_total = 0u;
static volatile uint32_t s_rtc_irq8_pf = 0u;
static volatile uint32_t s_rtc_irq8_uf = 0u;
static volatile uint32_t s_rtc_pi_ticks = 0u;
static volatile uint32_t s_rtc_pf_in_cur_sec = 0u;
static volatile uint32_t s_rtc_pf_per_sec = 1024u;
static volatile uint32_t s_rtc_generation = 1u;
static volatile uint8_t s_rtc_last_reg_c = 0u;

static uint8_t s_century_reg = RTC_REG_DEFAULT_CENTURY;
static bool s_century_from_fadt = false;

static bool s_cached_valid = false;
static rtc_hw_fields_t s_cached_hw = {0};
static rtc_raw_hw_t s_last_raw = {0};
static rtc_status_t s_last_error = RTC_ERR_NOT_READY;
static bool s_epoch_offset_active = false;
static rtc_bigint_t s_epoch_offset_sec = {0};

static char *s_time_format = NULL;
static char *s_date_format = NULL;

static char *rtc_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = (char *)kmalloc(len + 1u);
    if (!copy) return NULL;
    memcpy(copy, s, len + 1u);
    return copy;
}

static const char *rtc_memchr(const char *s, char c, size_t n) {
    if (!s) return NULL;
    for (size_t i = 0u; i < n; i++) {
        if (s[i] == c) return s + i;
    }
    return NULL;
}

static bool rtc_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool rtc_is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static char rtc_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static bool rtc_strieq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (rtc_tolower(*a) != rtc_tolower(*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static uint8_t acpi_checksum(const uint8_t *p, uint32_t len) {
    uint8_t sum = 0u;
    for (uint32_t i = 0u; i < len; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum;
}

static const uint8_t *acpi_scan_rsdp_range(uintptr_t start, uintptr_t end) {
    for (uintptr_t addr = start; addr + 20u <= end; addr += 16u) {
        const uint8_t *p = (const uint8_t *)addr;
        if (memcmp(p, "RSD PTR ", 8u) == 0 && acpi_checksum(p, 20u) == 0u) {
            return p;
        }
    }
    return NULL;
}

static uint8_t rtc_discover_fadt_century_reg(bool *out_from_fadt) {
    if (out_from_fadt) *out_from_fadt = false;

    const uint8_t *rsdp = acpi_scan_rsdp_range(0x9F000u, 0xA0000u);
    if (!rsdp) rsdp = acpi_scan_rsdp_range(0xE0000u, 0x100000u);
    if (!rsdp) return RTC_REG_DEFAULT_CENTURY;

    uint32_t rsdt_addr = 0u;
    memcpy(&rsdt_addr, rsdp + 16u, sizeof(rsdt_addr));
    if (rsdt_addr == 0u) return RTC_REG_DEFAULT_CENTURY;

    const uint8_t *rsdt = (const uint8_t *)(uintptr_t)rsdt_addr;
    if (memcmp(rsdt, "RSDT", 4u) != 0) return RTC_REG_DEFAULT_CENTURY;

    uint32_t rsdt_len = 0u;
    memcpy(&rsdt_len, rsdt + 4u, sizeof(rsdt_len));
    if (rsdt_len < 36u || rsdt_len > 0x10000u || acpi_checksum(rsdt, rsdt_len) != 0u) {
        return RTC_REG_DEFAULT_CENTURY;
    }

    uint32_t count = (rsdt_len - 36u) / 4u;
    for (uint32_t i = 0u; i < count; i++) {
        uint32_t tbl_addr = 0u;
        memcpy(&tbl_addr, rsdt + 36u + i * 4u, sizeof(tbl_addr));
        if (tbl_addr == 0u) continue;
        const uint8_t *tbl = (const uint8_t *)(uintptr_t)tbl_addr;
        if (memcmp(tbl, "FACP", 4u) != 0) continue;

        uint32_t tbl_len = 0u;
        memcpy(&tbl_len, tbl + 4u, sizeof(tbl_len));
        if (tbl_len >= 109u && tbl_len <= 0x10000u && acpi_checksum(tbl, tbl_len) == 0u) {
            uint8_t century_idx = tbl[108u];
            if (century_idx != 0u) {
                if (out_from_fadt) *out_from_fadt = true;
                return century_idx;
            }
        }
        break;
    }

    return RTC_REG_DEFAULT_CENTURY;
}

/* ======================== Unsigned Arbitrary-Precision ======================== */

static void bu_init(rtc_biguint_t *b) {
    b->limbs = NULL;
    b->len = 0u;
    b->cap = 0u;
}

static void bu_free(rtc_biguint_t *b) {
    if (b && b->limbs) {
        kfree(b->limbs);
        b->limbs = NULL;
    }
    if (b) {
        b->len = 0u;
        b->cap = 0u;
    }
}

static void bu_trim(rtc_biguint_t *b) {
    while (b->len > 0u && b->limbs[b->len - 1u] == 0u) {
        b->len--;
    }
}

static bool bu_ensure_cap(rtc_biguint_t *b, size_t need) {
    if (need <= b->cap) return true;
    size_t old_cap = b->cap;
    if (!kgrow_array((void **)&b->limbs, &b->cap, need, sizeof(uint32_t), 4u)) {
        return false;
    }
    if (b->cap > old_cap) {
        memset(b->limbs + old_cap, 0, (b->cap - old_cap) * sizeof(uint32_t));
    }
    return true;
}

static bool bu_is_zero(const rtc_biguint_t *b) {
    return !b || b->len == 0u;
}

static bool bu_set_u64(rtc_biguint_t *b, uint64_t val) {
    if (val == 0u) {
        b->len = 0u;
        return true;
    }
    uint32_t parts[3];
    size_t count = 0u;
    while (val > 0u && count < 3u) {
        parts[count++] = (uint32_t)(val % RTC_BIG_BASE);
        val /= RTC_BIG_BASE;
    }
    if (!bu_ensure_cap(b, count)) return false;
    for (size_t i = 0u; i < count; i++) b->limbs[i] = parts[i];
    b->len = count;
    return true;
}

static bool bu_copy(rtc_biguint_t *dst, const rtc_biguint_t *src) {
    if (dst == src) return true;
    if (src->len == 0u) {
        dst->len = 0u;
        return true;
    }
    if (!bu_ensure_cap(dst, src->len)) return false;
    memcpy(dst->limbs, src->limbs, src->len * sizeof(uint32_t));
    dst->len = src->len;
    return true;
}

static int bu_cmp(const rtc_biguint_t *a, const rtc_biguint_t *b) {
    size_t la = a ? a->len : 0u;
    size_t lb = b ? b->len : 0u;
    if (la < lb) return -1;
    if (la > lb) return 1;
    for (size_t i = la; i > 0u; i--) {
        uint32_t va = a->limbs[i - 1u];
        uint32_t vb = b->limbs[i - 1u];
        if (va < vb) return -1;
        if (va > vb) return 1;
    }
    return 0;
}

static bool bu_fits_u64(const rtc_biguint_t *a, uint64_t *out_val) {
    uint64_t acc = 0u;
    for (size_t i = a->len; i > 0u; i--) {
        if (acc > (UINT64_MAX - a->limbs[i - 1u]) / RTC_BIG_BASE) {
            return false;
        }
        acc = acc * RTC_BIG_BASE + a->limbs[i - 1u];
    }
    if (out_val) *out_val = acc;
    return true;
}

static bool bu_mul_add_u32(rtc_biguint_t *b, uint32_t mul, uint32_t add) {
    if (b->len == 0u) {
        return bu_set_u64(b, add);
    }
    /* Reserve room for carry limb up front so OOM leaves b intact. */
    if (!bu_ensure_cap(b, b->len + 1u)) return false;

    uint64_t carry = add;
    for (size_t i = 0u; i < b->len; i++) {
        uint64_t prod = (uint64_t)b->limbs[i] * (uint64_t)mul + carry;
        b->limbs[i] = (uint32_t)(prod % RTC_BIG_BASE);
        carry = prod / RTC_BIG_BASE;
    }
    if (carry > 0u) {
        b->limbs[b->len++] = (uint32_t)(carry % RTC_BIG_BASE);
        carry /= RTC_BIG_BASE;
        if (carry > 0u) {
            if (!bu_ensure_cap(b, b->len + 1u)) return false;
            b->limbs[b->len++] = (uint32_t)carry;
        }
    }
    bu_trim(b);
    return true;
}

static bool bu_add_u64(rtc_biguint_t *b, uint64_t add) {
    if (add == 0u) return true;
    size_t max_len = (b->len > 3u ? b->len : 3u) + 1u;
    if (!bu_ensure_cap(b, max_len)) return false;

    uint64_t carry = add;
    size_t i = 0u;
    while (carry > 0u || i < b->len) {
        uint64_t cur = (i < b->len) ? b->limbs[i] : 0u;
        uint64_t sum = cur + carry;
        b->limbs[i] = (uint32_t)(sum % RTC_BIG_BASE);
        carry = sum / RTC_BIG_BASE;
        i++;
    }
    if (i > b->len) b->len = i;
    bu_trim(b);
    return true;
}

static bool bu_add_big(rtc_biguint_t *dst, const rtc_biguint_t *src) {
    if (!src || src->len == 0u) return true;
    if (dst == src) {
        return bu_mul_add_u32(dst, 2u, 0u);
    }
    size_t n = (dst->len > src->len) ? dst->len : src->len;
    if (!bu_ensure_cap(dst, n + 1u)) return false;

    uint64_t carry = 0u;
    for (size_t i = 0u; i < n; i++) {
        uint64_t av = (i < dst->len) ? dst->limbs[i] : 0u;
        uint64_t bv = (i < src->len) ? src->limbs[i] : 0u;
        uint64_t sum = av + bv + carry;
        dst->limbs[i] = (uint32_t)(sum % RTC_BIG_BASE);
        carry = sum / RTC_BIG_BASE;
    }
    dst->len = n;
    if (carry > 0u) {
        dst->limbs[dst->len++] = (uint32_t)carry;
    }
    bu_trim(dst);
    return true;
}

/* Computes dst = a - b assuming a >= b. */
static bool bu_sub_abs(rtc_biguint_t *dst, const rtc_biguint_t *a, const rtc_biguint_t *b) {
    if (bu_cmp(a, b) < 0) return false;
    if (dst != a && !bu_copy(dst, a)) return false;

    int64_t borrow = 0;
    for (size_t i = 0u; i < dst->len; i++) {
        int64_t av = (int64_t)dst->limbs[i];
        int64_t bv = (b && i < b->len) ? (int64_t)b->limbs[i] : 0;
        int64_t diff = av - bv - borrow;
        if (diff < 0) {
            diff += (int64_t)RTC_BIG_BASE;
            borrow = 1;
        } else {
            borrow = 0;
        }
        dst->limbs[i] = (uint32_t)diff;
    }
    bu_trim(dst);
    return true;
}

static uint32_t bu_divmod_u32(rtc_biguint_t *b, uint32_t divisor) {
    uint64_t rem = 0u;
    for (size_t i = b->len; i > 0u; i--) {
        uint64_t cur = rem * RTC_BIG_BASE + b->limbs[i - 1u];
        b->limbs[i - 1u] = (uint32_t)(cur / divisor);
        rem = cur % divisor;
    }
    bu_trim(b);
    return (uint32_t)rem;
}

static bool bu_from_digits(rtc_biguint_t *b, const char *s, size_t len) {
    b->len = 0u;
    if (!s || len == 0u) return false;
    for (size_t i = 0u; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    size_t pos = 0u;
    while (pos < len && s[pos] == '0') pos++;
    if (pos == len) return true;

    for (size_t i = pos; i < len; ) {
        size_t chunk = len - i;
        if (chunk > 9u) chunk = 9u;
        uint32_t mul = 1u;
        uint32_t val = 0u;
        for (size_t k = 0u; k < chunk; k++) {
            mul *= 10u;
            val = val * 10u + (uint32_t)(s[i + k] - '0');
        }
        if (!bu_mul_add_u32(b, mul, val)) return false;
        i += chunk;
    }
    return true;
}

static char *bu_to_dec_pad_alloc(const rtc_biguint_t *b, size_t min_digits) {
    size_t digits = 1u;
    if (!bu_is_zero(b)) {
        digits = (b->len - 1u) * 9u;
        uint32_t top = b->limbs[b->len - 1u];
        while (top > 0u) {
            digits++;
            top /= 10u;
        }
    }
    size_t out_digits = (digits > min_digits) ? digits : min_digits;
    char *buf = (char *)kmalloc(out_digits + 1u);
    if (!buf) return NULL;
    memset(buf, '0', out_digits);
    buf[out_digits] = '\0';

    if (!bu_is_zero(b)) {
        size_t pos = out_digits;
        for (size_t i = 0u; i < b->len && pos > 0u; i++) {
            uint32_t val = b->limbs[i];
            size_t count = (i + 1u < b->len) ? 9u : 0u;
            if (count > 0u) {
                for (size_t k = 0u; k < count && pos > 0u; k++) {
                    buf[--pos] = (char)('0' + (val % 10u));
                    val /= 10u;
                }
            } else {
                while (val > 0u && pos > 0u) {
                    buf[--pos] = (char)('0' + (val % 10u));
                    val /= 10u;
                }
            }
        }
    }
    return buf;
}

/* ======================== Signed Arbitrary-Precision ========================= */

void rtc_bigint_init(rtc_bigint_t *v) {
    if (!v) return;
    v->negative = false;
    bu_init(&v->mag);
}

void rtc_bigint_free(rtc_bigint_t *v) {
    if (!v) return;
    v->negative = false;
    bu_free(&v->mag);
}

static void bi_normalize(rtc_bigint_t *v) {
    bu_trim(&v->mag);
    if (bu_is_zero(&v->mag)) {
        v->negative = false;
    }
}

static bool bi_set_i64(rtc_bigint_t *v, int64_t val) {
    if (val < 0) {
        v->negative = true;
        uint64_t mag = (val == INT64_MIN)
            ? ((uint64_t)INT64_MAX + 1ULL)
            : (uint64_t)(-val);
        if (!bu_set_u64(&v->mag, mag)) return false;
    } else {
        v->negative = false;
        if (!bu_set_u64(&v->mag, (uint64_t)val)) return false;
    }
    bi_normalize(v);
    return true;
}

static bool bi_copy(rtc_bigint_t *dst, const rtc_bigint_t *src) {
    if (dst == src) return true;
    dst->negative = src->negative;
    if (!bu_copy(&dst->mag, &src->mag)) return false;
    bi_normalize(dst);
    return true;
}

bool rtc_bigint_from_dec(rtc_bigint_t *out, const char *s) {
    if (!out || !s) return false;
    while (rtc_is_space(*s)) s++;

    bool neg = false;
    if (*s == '-') {
        neg = true;
        s++;
    } else if (*s == '+') {
        s++;
    }

    const char *digits_start = s;
    while (*s >= '0' && *s <= '9') s++;
    size_t digits_len = (size_t)(s - digits_start);
    while (rtc_is_space(*s)) s++;
    if (digits_len == 0u || *s != '\0') return false;

    if (!bu_from_digits(&out->mag, digits_start, digits_len)) return false;
    out->negative = neg;
    bi_normalize(out);
    return true;
}

char *rtc_bigint_to_dec_alloc(const rtc_bigint_t *v) {
    if (!v) return NULL;
    char *mag_str = bu_to_dec_pad_alloc(&v->mag, 1u);
    if (!mag_str) return NULL;
    if (!v->negative || bu_is_zero(&v->mag)) return mag_str;

    size_t mlen = strlen(mag_str);
    char *out = (char *)kmalloc(mlen + 2u);
    if (!out) {
        kfree(mag_str);
        return NULL;
    }
    out[0] = '-';
    memcpy(out + 1u, mag_str, mlen + 1u);
    kfree(mag_str);
    return out;
}

static bool bi_add_signed(rtc_bigint_t *dst, const rtc_bigint_t *a, const rtc_bigint_t *b) {
    rtc_bigint_t tmp;
    rtc_bigint_init(&tmp);

    bool ok = false;
    if (a->negative == b->negative) {
        tmp.negative = a->negative;
        ok = bu_copy(&tmp.mag, &a->mag) && bu_add_big(&tmp.mag, &b->mag);
    } else {
        int c = bu_cmp(&a->mag, &b->mag);
        if (c == 0) {
            tmp.negative = false;
            ok = true;
        } else if (c > 0) {
            tmp.negative = a->negative;
            ok = bu_sub_abs(&tmp.mag, &a->mag, &b->mag);
        } else {
            tmp.negative = b->negative;
            ok = bu_sub_abs(&tmp.mag, &b->mag, &a->mag);
        }
    }

    if (ok) {
        bi_normalize(&tmp);
        ok = bi_copy(dst, &tmp);
    }
    rtc_bigint_free(&tmp);
    return ok;
}

static bool bi_mul_u32(rtc_bigint_t *v, uint32_t factor) {
    if (!bu_mul_add_u32(&v->mag, factor, 0u)) return false;
    bi_normalize(v);
    return true;
}

static bool bi_add_i64(rtc_bigint_t *v, int64_t delta) {
    rtc_bigint_t d;
    rtc_bigint_init(&d);
    bool ok = bi_set_i64(&d, delta) && bi_add_signed(v, v, &d);
    rtc_bigint_free(&d);
    return ok;
}

/*
 * Euclidean floor division by positive divisor:
 * q = floor(v / divisor), 0 <= rem < divisor, v == q * divisor + rem.
 */
static bool bi_divmod_floor_u32(rtc_bigint_t *v, uint32_t divisor, uint32_t *out_rem) {
    if (divisor == 0u) return false;
    bool was_neg = v->negative && !bu_is_zero(&v->mag);
    uint32_t rem = bu_divmod_u32(&v->mag, divisor);

    if (was_neg && rem != 0u) {
        if (!bu_add_u64(&v->mag, 1ULL)) return false;
        rem = divisor - rem;
    }
    v->negative = was_neg && !bu_is_zero(&v->mag);
    if (out_rem) *out_rem = rem;
    return true;
}

static uint32_t bi_mod_u32(const rtc_bigint_t *v, uint32_t divisor) {
    rtc_bigint_t tmp;
    rtc_bigint_init(&tmp);
    uint32_t rem = 0u;
    if (bi_copy(&tmp, v)) {
        (void)bi_divmod_floor_u32(&tmp, divisor, &rem);
    }
    rtc_bigint_free(&tmp);
    return rem;
}

/* ========================= Dynamic String Builder ========================== */

static void sb_init(rtc_strbuf_t *sb) {
    sb->data = NULL;
    sb->len = 0u;
    sb->cap = 0u;
    sb->failed = false;
}

static void sb_append_n(rtc_strbuf_t *sb, const char *s, size_t n) {
    if (sb->failed || n == 0u) return;
    size_t need = sb->len + n + 1u;
    if (need > sb->cap) {
        if (!kgrow_array((void **)&sb->data, &sb->cap, need, sizeof(char), 32u)) {
            sb->failed = true;
            return;
        }
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_append_ch(rtc_strbuf_t *sb, char c) {
    sb_append_n(sb, &c, 1u);
}

static void sb_append_str(rtc_strbuf_t *sb, const char *s) {
    if (!s) return;
    sb_append_n(sb, s, strlen(s));
}

static void sb_append_uint(rtc_strbuf_t *sb, uint32_t val, size_t width, bool zero_pad) {
    char tmp[16];
    size_t pos = 0u;
    if (val == 0u) {
        tmp[pos++] = '0';
    } else {
        while (val > 0u && pos < sizeof(tmp)) {
            tmp[pos++] = (char)('0' + (val % 10u));
            val /= 10u;
        }
    }
    while (pos < width && pos < sizeof(tmp)) {
        tmp[pos++] = zero_pad ? '0' : ' ';
    }
    while (pos > 0u) {
        sb_append_ch(sb, tmp[--pos]);
    }
}

static void sb_append_bigint_pad(rtc_strbuf_t *sb, const rtc_bigint_t *v, size_t min_digits) {
    char *mag_str = bu_to_dec_pad_alloc(&v->mag, min_digits);
    if (!mag_str) {
        sb->failed = true;
        return;
    }
    if (v->negative && !bu_is_zero(&v->mag)) {
        sb_append_ch(sb, '-');
    }
    sb_append_str(sb, mag_str);
    kfree(mag_str);
}

static char *sb_finish(rtc_strbuf_t *sb, size_t *out_len) {
    if (sb->failed) {
        if (sb->data) kfree(sb->data);
        sb->data = NULL;
        sb->len = 0u;
        sb->cap = 0u;
        return NULL;
    }
    if (!sb->data) {
        sb->data = (char *)kmalloc(1u);
        if (!sb->data) return NULL;
        sb->data[0] = '\0';
        sb->len = 0u;
        sb->cap = 1u;
    }
    if (out_len) *out_len = sb->len;
    return sb->data;
}

/* ======================== Gregorian Calendar Engine ========================= */

void rtc_datetime_init(rtc_datetime_t *dt) {
    if (!dt) return;
    dt->negative_year = false;
    bu_init(&dt->year_mag);
    dt->month = 1u;
    dt->day = 1u;
    dt->hour = 0u;
    dt->minute = 0u;
    dt->second = 0u;
    dt->ms = 0u;
}

void rtc_datetime_free(rtc_datetime_t *dt) {
    if (!dt) return;
    bu_free(&dt->year_mag);
    dt->negative_year = false;
}

static bool rtc_datetime_copy(rtc_datetime_t *dst, const rtc_datetime_t *src) {
    if (!dst || !src) return false;
    if (dst == src) return true;
    dst->negative_year = src->negative_year;
    if (!bu_copy(&dst->year_mag, &src->year_mag)) return false;
    dst->month = src->month;
    dst->day = src->day;
    dst->hour = src->hour;
    dst->minute = src->minute;
    dst->second = src->second;
    dst->ms = src->ms;
    return true;
}

static bool cal_is_leap_year_bigint(const rtc_bigint_t *y) {
    if (bi_mod_u32(y, 4u) != 0u) return false;
    if (bi_mod_u32(y, 100u) != 0u) return true;
    return bi_mod_u32(y, 400u) == 0u;
}

static bool cal_is_leap_year_u32(uint32_t y) {
    if ((y % 4u) != 0u) return false;
    if ((y % 100u) != 0u) return true;
    return (y % 400u) == 0u;
}

static uint32_t cal_days_in_month(bool leap, uint32_t month) {
    switch (month) {
        case 1u: case 3u: case 5u: case 7u: case 8u: case 10u: case 12u:
            return 31u;
        case 4u: case 6u: case 9u: case 11u:
            return 30u;
        case 2u:
            return leap ? 29u : 28u;
        default:
            return 0u;
    }
}

static bool cal_validate_datetime(const rtc_datetime_t *dt) {
    if (!dt) return false;
    if (dt->month < 1u || dt->month > 12u) return false;
    if (dt->hour > 23u || dt->minute > 59u || dt->second > 59u || dt->ms > 999u) {
        return false;
    }
    rtc_bigint_t y;
    rtc_bigint_init(&y);
    y.negative = dt->negative_year && !bu_is_zero(&dt->year_mag);
    if (!bu_copy(&y.mag, &dt->year_mag)) {
        rtc_bigint_free(&y);
        return false;
    }
    bool leap = cal_is_leap_year_bigint(&y);
    rtc_bigint_free(&y);

    uint32_t max_d = cal_days_in_month(leap, dt->month);
    return (dt->day >= 1u && dt->day <= max_d);
}

/* Converts validated date/time into signed total days since 0000-01-01. */
static bool cal_date_to_days_bigint(bool neg_year, const rtc_biguint_t *year_mag,
                                    uint32_t month, uint32_t day, rtc_bigint_t *out_days) {
    if (month < 1u || month > 12u || day < 1u || day > 31u) return false;

    rtc_bigint_t y;
    rtc_bigint_init(&y);
    y.negative = neg_year && !bu_is_zero(year_mag);
    if (!bu_copy(&y.mag, year_mag)) {
        rtc_bigint_free(&y);
        return false;
    }
    if (month <= 2u && !bi_add_i64(&y, -1LL)) {
        rtc_bigint_free(&y);
        return false;
    }

    uint32_t yoe = 0u;
    if (!bi_divmod_floor_u32(&y, 400u, &yoe)) {
        rtc_bigint_free(&y);
        return false;
    }

    uint32_t m_adj = (month > 2u) ? (month - 3u) : (month + 9u);
    uint32_t doy = (153u * m_adj + 2u) / 5u + day - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;

    /* Shift from 0000-03-01 back to 0000-01-01 (+60 days because year 0 is leap). */
    bool ok = bi_mul_u32(&y, 146097u) &&
              bi_add_i64(&y, (int64_t)doe + 60LL) &&
              bi_copy(out_days, &y);
    rtc_bigint_free(&y);
    return ok;
}

static bool cal_days_to_date_bigint(const rtc_bigint_t *in_days, rtc_datetime_t *dt) {
    rtc_bigint_t era;
    rtc_bigint_init(&era);
    if (!bi_copy(&era, in_days) || !bi_add_i64(&era, -60LL)) {
        rtc_bigint_free(&era);
        return false;
    }

    uint32_t doe = 0u;
    if (!bi_divmod_floor_u32(&era, 146097u, &doe)) {
        rtc_bigint_free(&era);
        return false;
    }

    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint32_t mp = (5u * doy + 2u) / 153u;
    uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;
    uint32_t m = (mp < 10u) ? (mp + 3u) : (mp - 9u);

    bool ok = bi_mul_u32(&era, 400u) &&
              bi_add_i64(&era, (int64_t)yoe + (m <= 2u ? 1LL : 0LL)) &&
              bu_copy(&dt->year_mag, &era.mag);
    if (ok) {
        dt->negative_year = era.negative && !bu_is_zero(&dt->year_mag);
        dt->month = m;
        dt->day = d;
    }
    rtc_bigint_free(&era);
    return ok;
}

static bool cal_datetime_to_totals(const rtc_datetime_t *dt,
                                   rtc_bigint_t *out_days,
                                   rtc_bigint_t *out_hours,
                                   rtc_bigint_t *out_mins,
                                   rtc_bigint_t *out_secs,
                                   rtc_bigint_t *out_ms) {
    rtc_bigint_t acc;
    rtc_bigint_init(&acc);
    if (!cal_date_to_days_bigint(dt->negative_year, &dt->year_mag, dt->month, dt->day, &acc)) {
        rtc_bigint_free(&acc);
        return false;
    }
    if (out_days && !bi_copy(out_days, &acc)) goto fail;

    if (!bi_mul_u32(&acc, 24u) || !bi_add_i64(&acc, (int64_t)dt->hour)) goto fail;
    if (out_hours && !bi_copy(out_hours, &acc)) goto fail;

    if (!bi_mul_u32(&acc, 60u) || !bi_add_i64(&acc, (int64_t)dt->minute)) goto fail;
    if (out_mins && !bi_copy(out_mins, &acc)) goto fail;

    if (!bi_mul_u32(&acc, 60u) || !bi_add_i64(&acc, (int64_t)dt->second)) goto fail;
    if (out_secs && !bi_copy(out_secs, &acc)) goto fail;

    if (!bi_mul_u32(&acc, 1000u) || !bi_add_i64(&acc, (int64_t)dt->ms)) goto fail;
    if (out_ms && !bi_copy(out_ms, &acc)) goto fail;

    rtc_bigint_free(&acc);
    return true;

fail:
    rtc_bigint_free(&acc);
    return false;
}

static bool cal_total_ms_to_datetime(const rtc_bigint_t *in_ms, rtc_datetime_t *out_dt) {
    rtc_bigint_t q;
    rtc_bigint_init(&q);
    uint32_t ms = 0u, s = 0u, m = 0u, h = 0u;
    if (!bi_copy(&q, in_ms)) goto fail;
    if (!bi_divmod_floor_u32(&q, 1000u, &ms)) goto fail;
    if (!bi_divmod_floor_u32(&q, 60u, &s)) goto fail;
    if (!bi_divmod_floor_u32(&q, 60u, &m)) goto fail;
    if (!bi_divmod_floor_u32(&q, 24u, &h)) goto fail;
    if (!cal_days_to_date_bigint(&q, out_dt)) goto fail;

    out_dt->hour = h;
    out_dt->minute = m;
    out_dt->second = s;
    out_dt->ms = ms;
    rtc_bigint_free(&q);
    return cal_validate_datetime(out_dt);

fail:
    rtc_bigint_free(&q);
    return false;
}

static bool cal_total_sec_to_datetime(const rtc_bigint_t *in_sec, uint32_t ms, rtc_datetime_t *out_dt) {
    rtc_bigint_t q;
    rtc_bigint_init(&q);
    uint32_t s = 0u, m = 0u, h = 0u;
    if (!bi_copy(&q, in_sec)) goto fail;
    if (!bi_divmod_floor_u32(&q, 60u, &s)) goto fail;
    if (!bi_divmod_floor_u32(&q, 60u, &m)) goto fail;
    if (!bi_divmod_floor_u32(&q, 24u, &h)) goto fail;
    if (!cal_days_to_date_bigint(&q, out_dt)) goto fail;

    out_dt->hour = h;
    out_dt->minute = m;
    out_dt->second = s;
    out_dt->ms = (ms < 1000u) ? ms : 0u;
    rtc_bigint_free(&q);
    return cal_validate_datetime(out_dt);

fail:
    rtc_bigint_free(&q);
    return false;
}

static uint32_t cal_weekday_1_to_7(uint32_t year, uint32_t month, uint32_t day) {
    /* Return 1=Sunday..7=Saturday matching MC146818 Day-of-Week register. */
    static const uint8_t t[] = {0u, 3u, 2u, 5u, 0u, 3u, 5u, 1u, 4u, 6u, 2u, 4u};
    uint32_t y = year - (month < 3u ? 1u : 0u);
    uint32_t w = (y + y / 4u - y / 100u + y / 400u + t[month - 1u] + day) % 7u;
    return w + 1u;
}

/* ======================== MC146818 CMOS Hardware I/O ======================== */

static inline uint8_t cmos_read_unlocked(uint8_t reg) {
    outb((uint16_t)CMOS_ADDR_PORT, (uint8_t)(reg & 0x7Fu));
    io_wait();
    return inb((uint16_t)CMOS_DATA_PORT);
}

static inline void cmos_write_unlocked(uint8_t reg, uint8_t val) {
    outb((uint16_t)CMOS_ADDR_PORT, (uint8_t)(reg & 0x7Fu));
    io_wait();
    outb((uint16_t)CMOS_DATA_PORT, val);
}

static bool cmos_bcd_valid(uint8_t v) {
    return ((v & 0x0Fu) <= 9u) && (((v >> 4) & 0x0Fu) <= 9u);
}

static uint8_t cmos_bcd_to_bin(uint8_t v) {
    return (uint8_t)(((v >> 4) * 10u) + (v & 0x0Fu));
}

static uint8_t cmos_bin_to_bcd(uint8_t v) {
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

static bool cmos_decode_field(uint8_t raw, bool bcd_mode, uint32_t *out_val) {
    if (bcd_mode) {
        if (!cmos_bcd_valid(raw)) return false;
        *out_val = cmos_bcd_to_bin(raw);
    } else {
        *out_val = raw;
    }
    return true;
}

static rtc_status_t rtc_cmos_read_snapshot_hw(rtc_raw_hw_t *out_raw, rtc_hw_fields_t *out_fields) {
    {
        uint64_t f0 = spin_lock_irqsave(&s_rtc_hw_lock);
        uint8_t ra0 = cmos_read_unlocked(RTC_REG_STATUS_A);
        uint8_t rb0 = cmos_read_unlocked(RTC_REG_STATUS_B);
        uint8_t rd0 = cmos_read_unlocked(RTC_REG_STATUS_D);
        spin_unlock_irqrestore(&s_rtc_hw_lock, f0);
        if (ra0 == 0xFFu && rb0 == 0xFFu && rd0 == 0xFFu) {
            return RTC_ERR_HW_UNAVAILABLE;
        }
    }

    rtc_raw_hw_t first, second;
    uint8_t cen_reg = s_century_reg;
    bool got_consistent = false;

    for (uint32_t attempt = 0u; attempt < RTC_SNAPSHOT_RETRIES; attempt++) {
        uint32_t spin = 0u;
        bool uip_ok = false;
        while (spin < RTC_UIP_POLL_LIMIT) {
            uint64_t f = spin_lock_irqsave(&s_rtc_hw_lock);
            uint8_t ra = cmos_read_unlocked(RTC_REG_STATUS_A);
            spin_unlock_irqrestore(&s_rtc_hw_lock, f);
            if ((ra & RTC_A_UIP) == 0u) {
                uip_ok = true;
                break;
            }
            spin++;
            io_wait();
        }
        if (!uip_ok) {
            return RTC_ERR_HW_TIMEOUT;
        }

        uint64_t f = spin_lock_irqsave(&s_rtc_hw_lock);
        uint8_t ra1 = cmos_read_unlocked(RTC_REG_STATUS_A);
        if ((ra1 & RTC_A_UIP) != 0u) {
            spin_unlock_irqrestore(&s_rtc_hw_lock, f);
            continue;
        }
        first.sec = cmos_read_unlocked(RTC_REG_SECONDS);
        first.min = cmos_read_unlocked(RTC_REG_MINUTES);
        first.hr  = cmos_read_unlocked(RTC_REG_HOURS);
        first.dow = cmos_read_unlocked(RTC_REG_WEEKDAY);
        first.day = cmos_read_unlocked(RTC_REG_DAY);
        first.mon = cmos_read_unlocked(RTC_REG_MONTH);
        first.yr  = cmos_read_unlocked(RTC_REG_YEAR);
        first.cen = (cen_reg != 0u) ? cmos_read_unlocked(cen_reg) : 0u;
        first.reg_a = ra1;
        first.reg_b = cmos_read_unlocked(RTC_REG_STATUS_B);
        first.reg_c = s_rtc_last_reg_c;
        first.reg_d = cmos_read_unlocked(RTC_REG_STATUS_D);

        uint8_t ra2 = cmos_read_unlocked(RTC_REG_STATUS_A);
        if ((ra2 & RTC_A_UIP) != 0u) {
            spin_unlock_irqrestore(&s_rtc_hw_lock, f);
            continue;
        }
        second.sec = cmos_read_unlocked(RTC_REG_SECONDS);
        second.min = cmos_read_unlocked(RTC_REG_MINUTES);
        second.hr  = cmos_read_unlocked(RTC_REG_HOURS);
        second.dow = cmos_read_unlocked(RTC_REG_WEEKDAY);
        second.day = cmos_read_unlocked(RTC_REG_DAY);
        second.mon = cmos_read_unlocked(RTC_REG_MONTH);
        second.yr  = cmos_read_unlocked(RTC_REG_YEAR);
        second.cen = (cen_reg != 0u) ? cmos_read_unlocked(cen_reg) : 0u;
        second.reg_b = cmos_read_unlocked(RTC_REG_STATUS_B);
        second.reg_d = cmos_read_unlocked(RTC_REG_STATUS_D);
        uint8_t ra3 = cmos_read_unlocked(RTC_REG_STATUS_A);
        spin_unlock_irqrestore(&s_rtc_hw_lock, f);

        if ((ra3 & RTC_A_UIP) != 0u) continue;
        if (first.sec == second.sec &&
            first.min == second.min &&
            first.hr  == second.hr  &&
            first.day == second.day &&
            first.mon == second.mon &&
            first.yr  == second.yr  &&
            first.cen == second.cen &&
            first.reg_b == second.reg_b &&
            first.reg_d == second.reg_d) {
            got_consistent = true;
            break;
        }
    }

    if (!got_consistent) {
        return RTC_ERR_HW_TIMEOUT;
    }

    if (out_raw) *out_raw = first;

    /* Detect floating bus / disconnected RTC or dead battery. */
    if (first.reg_d == 0xFFu && first.reg_b == 0xFFu && first.sec == 0xFFu) {
        return RTC_ERR_HW_UNAVAILABLE;
    }
    if ((first.reg_d & RTC_D_VRT) == 0u) {
        return RTC_ERR_HW_UNAVAILABLE;
    }

    bool bcd_mode = (first.reg_b & RTC_B_DM_BIN) == 0u;
    bool h24_mode = (first.reg_b & RTC_B_24H) != 0u;

    uint32_t sec = 0u, min = 0u, hr = 0u, day = 0u, mon = 0u, yr2 = 0u;
    if (!cmos_decode_field(first.sec, bcd_mode, &sec) ||
        !cmos_decode_field(first.min, bcd_mode, &min) ||
        !cmos_decode_field(first.day, bcd_mode, &day) ||
        !cmos_decode_field(first.mon, bcd_mode, &mon) ||
        !cmos_decode_field(first.yr,  bcd_mode, &yr2)) {
        return RTC_ERR_INVALID_DATETIME;
    }

    if (h24_mode) {
        if (!cmos_decode_field(first.hr, bcd_mode, &hr)) return RTC_ERR_INVALID_DATETIME;
    } else {
        bool pm = (first.hr & 0x80u) != 0u;
        uint32_t h12 = 0u;
        if (!cmos_decode_field((uint8_t)(first.hr & 0x7Fu), bcd_mode, &h12)) {
            return RTC_ERR_INVALID_DATETIME;
        }
        if (h12 < 1u || h12 > 12u) return RTC_ERR_INVALID_DATETIME;
        if (h12 == 12u) {
            hr = pm ? 12u : 0u;
        } else {
            hr = pm ? (h12 + 12u) : h12;
        }
    }

    if (sec > 59u || min > 59u || hr > 23u || mon < 1u || mon > 12u || yr2 > 99u) {
        return RTC_ERR_INVALID_DATETIME;
    }

    uint32_t century = 0u;
    bool have_century = false;
    if (cen_reg != 0u && first.cen != 0xFFu) {
        uint32_t cen_dec = 0u;
        if (cmos_decode_field(first.cen, bcd_mode, &cen_dec) && cen_dec <= 99u) {
            if (s_century_from_fadt || cen_dec >= 19u) {
                century = cen_dec;
                have_century = true;
            }
        }
    }
    if (!have_century) {
        /* Fallback only if platform omits a valid century register. */
        century = (yr2 >= 70u) ? 19u : 20u;
    }
    uint32_t full_year = century * 100u + yr2;

    uint32_t max_day = cal_days_in_month(cal_is_leap_year_u32(full_year), mon);
    if (day < 1u || day > max_day) {
        return RTC_ERR_INVALID_DATETIME;
    }

    if (out_fields) {
        out_fields->year = full_year;
        out_fields->month = mon;
        out_fields->day = day;
        out_fields->hour = hr;
        out_fields->minute = min;
        out_fields->second = sec;
    }
    return RTC_OK;
}

static int64_t rtc_hw_fields_to_sec_i64(const rtc_hw_fields_t *f) {
    int64_t y = (int64_t)f->year - (f->month <= 2u ? 1LL : 0LL);
    int64_t era = (y >= 0LL ? y : y - 399LL) / 400LL;
    uint32_t yoe = (uint32_t)(y - era * 400LL);
    uint32_t m_adj = (f->month > 2u) ? (f->month - 3u) : (f->month + 9u);
    uint32_t doy = (153u * m_adj + 2u) / 5u + f->day - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    int64_t days = era * 146097LL + (int64_t)doe + 60LL;
    return days * 86400LL +
           (int64_t)f->hour * 3600LL +
           (int64_t)f->minute * 60LL +
           (int64_t)f->second;
}

static rtc_status_t rtc_cmos_program_raw_hw(const rtc_hw_fields_t *fields, uint32_t ms,
                                            rtc_raw_hw_t *out_raw,
                                            rtc_hw_fields_t *out_fields) {
    uint32_t century = fields->year / 100u;
    uint32_t yr2 = fields->year % 100u;
    uint32_t dow = cal_weekday_1_to_7(fields->year, fields->month, fields->day);

    uint64_t f = spin_lock_irqsave(&s_rtc_hw_lock);
    uint8_t reg_d = cmos_read_unlocked(RTC_REG_STATUS_D);
    uint8_t reg_b = cmos_read_unlocked(RTC_REG_STATUS_B);
    if (reg_d == 0xFFu && reg_b == 0xFFu) {
        spin_unlock_irqrestore(&s_rtc_hw_lock, f);
        return RTC_ERR_HW_UNAVAILABLE;
    }

    bool bcd_mode = (reg_b & RTC_B_DM_BIN) == 0u;
    bool h24_mode = (reg_b & RTC_B_24H) != 0u;

    uint8_t w_sec = bcd_mode ? cmos_bin_to_bcd((uint8_t)fields->second) : (uint8_t)fields->second;
    uint8_t w_min = bcd_mode ? cmos_bin_to_bcd((uint8_t)fields->minute) : (uint8_t)fields->minute;
    uint8_t w_day = bcd_mode ? cmos_bin_to_bcd((uint8_t)fields->day)    : (uint8_t)fields->day;
    uint8_t w_mon = bcd_mode ? cmos_bin_to_bcd((uint8_t)fields->month)  : (uint8_t)fields->month;
    uint8_t w_yr  = bcd_mode ? cmos_bin_to_bcd((uint8_t)yr2)            : (uint8_t)yr2;
    uint8_t w_cen = bcd_mode ? cmos_bin_to_bcd((uint8_t)century)        : (uint8_t)century;
    uint8_t w_dow = bcd_mode ? cmos_bin_to_bcd((uint8_t)dow)            : (uint8_t)dow;

    uint8_t w_hr = 0u;
    if (h24_mode) {
        w_hr = bcd_mode ? cmos_bin_to_bcd((uint8_t)fields->hour) : (uint8_t)fields->hour;
    } else {
        bool pm = (fields->hour >= 12u);
        uint32_t h12 = fields->hour % 12u;
        if (h12 == 0u) h12 = 12u;
        w_hr = bcd_mode ? cmos_bin_to_bcd((uint8_t)h12) : (uint8_t)h12;
        if (pm) w_hr = (uint8_t)(w_hr | 0x80u);
    }

    /* Hold internal CMOS update cycle while all fields are written atomically. */
    cmos_write_unlocked(RTC_REG_STATUS_B, (uint8_t)((reg_b & 0x07u) | RTC_B_SET));
    cmos_write_unlocked(RTC_REG_SECONDS, w_sec);
    cmos_write_unlocked(RTC_REG_MINUTES, w_min);
    cmos_write_unlocked(RTC_REG_HOURS, w_hr);
    cmos_write_unlocked(RTC_REG_WEEKDAY, w_dow);
    cmos_write_unlocked(RTC_REG_DAY, w_day);
    cmos_write_unlocked(RTC_REG_MONTH, w_mon);
    cmos_write_unlocked(RTC_REG_YEAR, w_yr);
    if (s_century_reg != 0u) {
        cmos_write_unlocked(s_century_reg, w_cen);
    }

    uint8_t active_b = (uint8_t)((reg_b & (RTC_B_DSE | RTC_B_24H | RTC_B_DM_BIN)) |
                                 RTC_B_PIE | RTC_B_UIE);
    cmos_write_unlocked(RTC_REG_STATUS_B, active_b);
    s_rtc_last_reg_c = cmos_read_unlocked(RTC_REG_STATUS_C);
    uint32_t div = s_rtc_pf_per_sec ? s_rtc_pf_per_sec : 1024u;
    s_rtc_pi_ticks = (ms < 1000u) ? ((ms * div) / 1000u) : 0u;
    s_rtc_uf_dirty = true;
    s_rtc_generation++;
    spin_unlock_irqrestore(&s_rtc_hw_lock, f);

    return rtc_cmos_read_snapshot_hw(out_raw, out_fields);
}

static rtc_status_t rtc_cmos_write_hw(const rtc_hw_fields_t *fields, uint32_t ms) {
    if (!fields) return RTC_ERR_INVALID_ARG;

    if (fields->year > 9999u || fields->month < 1u || fields->month > 12u ||
        fields->hour > 23u || fields->minute > 59u || fields->second > 59u) {
        return RTC_ERR_INVALID_DATETIME;
    }
    uint32_t max_day = cal_days_in_month(cal_is_leap_year_u32(fields->year), fields->month);
    if (fields->day < 1u || fields->day > max_day) {
        return RTC_ERR_INVALID_DATETIME;
    }
    if (s_century_reg == 0u && (fields->year < 1900u || fields->year > 2099u)) {
        return RTC_ERR_HW_RANGE;
    }

    /* Snapshot live CMOS registers + ms before programming so out-of-range writes can roll back cleanly. */
    rtc_raw_hw_t prev_raw;
    rtc_hw_fields_t prev_fields;
    rtc_status_t prev_st = rtc_cmos_read_snapshot_hw(&prev_raw, &prev_fields);
    if (prev_st == RTC_ERR_HW_UNAVAILABLE || prev_st == RTC_ERR_HW_TIMEOUT) {
        return prev_st;
    }
    bool have_prev = (prev_st == RTC_OK);

    uint64_t hf = spin_lock_irqsave(&s_rtc_hw_lock);
    uint32_t prev_ticks = s_rtc_pi_ticks;
    uint32_t prev_rate = s_rtc_pf_per_sec;
    spin_unlock_irqrestore(&s_rtc_hw_lock, hf);
    uint32_t prev_ms = (prev_rate > 0u) ? ((prev_ticks * 1000u) / prev_rate) : 0u;
    if (prev_ms > 999u) prev_ms = 999u;

    if (!have_prev) {
        uint64_t sf = spin_lock_irqsave(&s_rtc_state_lock);
        if (s_cached_valid) {
            prev_fields = s_cached_hw;
            prev_raw = s_last_raw;
            have_prev = true;
        }
        spin_unlock_irqrestore(&s_rtc_state_lock, sf);
    }

    rtc_raw_hw_t verify_raw;
    rtc_hw_fields_t verify_fields;
    rtc_status_t st = rtc_cmos_program_raw_hw(fields, ms, &verify_raw, &verify_fields);
    if (st == RTC_ERR_HW_UNAVAILABLE || st == RTC_ERR_HW_TIMEOUT) {
        return st;
    }

    /* Reject targets that hardware CMOS or an emulated RTC silently wraps or alters. */
    bool mismatch = (st != RTC_OK);
    if (!mismatch) {
        int64_t target_sec = rtc_hw_fields_to_sec_i64(fields);
        int64_t actual_sec = rtc_hw_fields_to_sec_i64(&verify_fields);
        int64_t delta = actual_sec - target_sec;
        if (delta < -1LL || delta > 2LL) {
            mismatch = true;
        }
    }

    if (mismatch) {
        if (have_prev) {
            rtc_raw_hw_t rest_raw;
            rtc_hw_fields_t rest_fields;
            if (rtc_cmos_program_raw_hw(&prev_fields, prev_ms, &rest_raw, &rest_fields) == RTC_OK) {
                uint64_t sf = spin_lock_irqsave(&s_rtc_state_lock);
                s_last_raw = rest_raw;
                s_cached_hw = rest_fields;
                s_cached_valid = true;
                s_last_error = RTC_OK;
                spin_unlock_irqrestore(&s_rtc_state_lock, sf);
            }
        }
        return RTC_ERR_HW_RANGE;
    }

    uint64_t sf = spin_lock_irqsave(&s_rtc_state_lock);
    s_last_raw = verify_raw;
    s_cached_hw = verify_fields;
    s_cached_valid = true;
    s_last_error = RTC_OK;
    spin_unlock_irqrestore(&s_rtc_state_lock, sf);
    return RTC_OK;
}

/*
 * IRQ8 interrupt service handler: runs with IF=0 after PIC EOI.
 * Only clears Register C and increments crystal counters; no heap, console, or UI work.
 */
static void rtc_irq8_handler(uint8_t irq, void *ctx) {
    (void)irq;
    (void)ctx;

    uint64_t f = spin_lock_irqsave(&s_rtc_hw_lock);
    uint8_t reg_c = cmos_read_unlocked(RTC_REG_STATUS_C);
    s_rtc_last_reg_c = reg_c;

    s_rtc_irq8_total++;
    if ((reg_c & RTC_C_PF) != 0u) {
        s_rtc_irq8_pf++;
        s_rtc_pf_in_cur_sec++;
        uint32_t cap = s_rtc_pf_per_sec ? s_rtc_pf_per_sec : 1024u;
        if (s_rtc_pi_ticks < cap) {
            s_rtc_pi_ticks++;
        }
        if (s_rtc_uses_subsecond) {
            s_rtc_generation++;
        }
    }
    if ((reg_c & RTC_C_UF) != 0u) {
        s_rtc_irq8_uf++;
        if (s_rtc_irq8_uf >= 2u && s_rtc_pf_in_cur_sec >= 16u && s_rtc_pf_in_cur_sec <= 2048u) {
            s_rtc_pf_per_sec = s_rtc_pf_in_cur_sec;
        }
        s_rtc_pf_in_cur_sec = 0u;
        s_rtc_pi_ticks = 0u;
        s_rtc_uf_dirty = true;
        s_rtc_generation++;
    }
    spin_unlock_irqrestore(&s_rtc_hw_lock, f);
}

static bool rtc_sync_from_cmos_locked_helper(bool force_read, uint32_t *out_ms, bool *out_subsec) {
    uint64_t hf = spin_lock_irqsave(&s_rtc_hw_lock);
    bool dirty = s_rtc_uf_dirty;
    uint32_t uf_before = s_rtc_irq8_uf;
    uint32_t ticks = s_rtc_pi_ticks;
    uint32_t pf_rate = s_rtc_pf_per_sec;
    uint32_t pf = s_rtc_irq8_pf;
    bool no_uf_yet = (uf_before == 0u);
    if (dirty) s_rtc_uf_dirty = false;
    spin_unlock_irqrestore(&s_rtc_hw_lock, hf);

    bool need_read = force_read || dirty || no_uf_yet;
    if (!need_read) {
        uint64_t sf = spin_lock_irqsave(&s_rtc_state_lock);
        need_read = !s_cached_valid;
        spin_unlock_irqrestore(&s_rtc_state_lock, sf);
    }

    rtc_raw_hw_t raw;
    rtc_hw_fields_t fields;
    rtc_status_t hw_st = RTC_OK;
    if (need_read) {
        for (uint32_t pass = 0u; pass < 3u; pass++) {
            hf = spin_lock_irqsave(&s_rtc_hw_lock);
            uf_before = s_rtc_irq8_uf;
            s_rtc_uf_dirty = false;
            spin_unlock_irqrestore(&s_rtc_hw_lock, hf);

            hw_st = rtc_cmos_read_snapshot_hw(&raw, &fields);

            hf = spin_lock_irqsave(&s_rtc_hw_lock);
            uint32_t uf_after = s_rtc_irq8_uf;
            ticks = s_rtc_pi_ticks;
            pf_rate = s_rtc_pf_per_sec;
            pf = s_rtc_irq8_pf;
            no_uf_yet = (uf_after == 0u);
            spin_unlock_irqrestore(&s_rtc_hw_lock, hf);

            if (hw_st != RTC_OK || uf_after == uf_before) break;
        }
    }

    uint64_t sf = spin_lock_irqsave(&s_rtc_state_lock);
    if (need_read) {
        if (hw_st == RTC_OK) {
            bool changed = !s_cached_valid ||
                fields.second != s_cached_hw.second ||
                fields.minute != s_cached_hw.minute ||
                fields.hour   != s_cached_hw.hour   ||
                fields.day    != s_cached_hw.day    ||
                fields.month  != s_cached_hw.month  ||
                fields.year   != s_cached_hw.year;
            s_last_raw = raw;
            s_cached_hw = fields;
            s_cached_valid = true;
            s_last_error = RTC_OK;
            if (changed) {
                s_rtc_generation++;
                if (no_uf_yet) {
                    uint64_t hf2 = spin_lock_irqsave(&s_rtc_hw_lock);
                    s_rtc_pi_ticks = 0u;
                    spin_unlock_irqrestore(&s_rtc_hw_lock, hf2);
                    ticks = 0u;
                }
            }
        } else {
            s_cached_valid = false;
            s_last_error = hw_st;
        }
    }

    bool stopped = (s_last_raw.reg_b & RTC_B_SET) != 0u ||
                   (s_last_raw.reg_a & RTC_A_DV_MASK) == 0u ||
                   (s_last_raw.reg_a & RTC_A_DV_MASK) >= 0x60u;
    bool subsec_ok = s_cached_valid && !stopped && s_rtc_irq8_registered &&
                     (pf > 0u) && ((s_last_raw.reg_b & RTC_B_PIE) != 0u);

    if (out_subsec) *out_subsec = subsec_ok;
    if (out_ms) {
        if (subsec_ok) {
            uint32_t div = (pf_rate > 0u) ? pf_rate : 1024u;
            uint32_t ms = (ticks * 1000u) / div;
            *out_ms = (ms <= 999u) ? ms : 999u;
        } else {
            *out_ms = 0u;
        }
    }
    bool ok = s_cached_valid;
    spin_unlock_irqrestore(&s_rtc_state_lock, sf);
    return ok;
}

/* ======================== Token Format Compiler & Engine ==================== */

static bool fmt_str_contains_subsecond(const char *fmt) {
    if (!fmt) return false;
    bool in_quote = false;
    for (size_t i = 0u; fmt[i] != '\0'; i++) {
        if (fmt[i] == '\'') {
            in_quote = !in_quote;
            continue;
        }
        if (in_quote) continue;
        if (strncmp(fmt + i, "Ms", 2u) == 0) return true;
    }
    return false;
}

static bool fmt_is_date_token_char(char c) {
    return c == 'Y' || c == 'D';
}

static int fmt_probe_mm_side(const char *fmt, size_t len, size_t pos, bool forward, bool stop_on_space) {
    /* Return +1 for time neighbor, -1 for date neighbor, 0 if undecided. */
    bool in_quote = false;
    if (!forward) {
        for (size_t i = pos; i > 0u; i--) {
            char c = fmt[i - 1u];
            if (c == '\'') { in_quote = !in_quote; continue; }
            if (in_quote) continue;
            if (stop_on_space && rtc_is_space(c)) break;
            if (c == ':') return 1;
            if (c == 'H' || c == 'h' || c == 'S') return 1;
            if (c == 'Y' || c == 'D') return -1;
            if (c == 's' && i >= 2u && fmt[i - 2u] == 'M') return 1;
        }
    } else {
        for (size_t i = pos + 2u; i < len; i++) {
            char c = fmt[i];
            if (c == '\'') { in_quote = !in_quote; continue; }
            if (in_quote) continue;
            if (stop_on_space && rtc_is_space(c)) break;
            if (c == ':') return 1;
            if (c == 'H' || c == 'h' || c == 'S') return 1;
            if (fmt_is_date_token_char(c)) return -1;
            if (c == 'M' && i + 1u < len && fmt[i + 1u] == 's') return 1;
        }
    }
    return 0;
}

static rtc_mm_kind_t fmt_classify_mm(const char *fmt, size_t pos) {
    size_t len = strlen(fmt);
    if (pos > 0u && fmt[pos - 1u] == ':') return RTC_MM_MINUTE;
    if (pos + 2u < len && fmt[pos + 2u] == ':') return RTC_MM_MINUTE;

    /* First classify within the immediate delimiter-separated token group. */
    int side = fmt_probe_mm_side(fmt, len, pos, false, true);
    if (side != 0) return (side > 0) ? RTC_MM_MINUTE : RTC_MM_MONTH;
    side = fmt_probe_mm_side(fmt, len, pos, true, true);
    if (side != 0) return (side > 0) ? RTC_MM_MINUTE : RTC_MM_MONTH;

    /* Fall back to scanning across whitespace if still ambiguous. */
    side = fmt_probe_mm_side(fmt, len, pos, false, false);
    if (side != 0) return (side > 0) ? RTC_MM_MINUTE : RTC_MM_MONTH;
    side = fmt_probe_mm_side(fmt, len, pos, true, false);
    if (side != 0) return (side > 0) ? RTC_MM_MINUTE : RTC_MM_MONTH;

    if ((pos > 0u && (fmt[pos - 1u] == '-' || fmt[pos - 1u] == '/' || fmt[pos - 1u] == '.')) ||
        (pos + 2u < len && (fmt[pos + 2u] == '-' || fmt[pos + 2u] == '/' || fmt[pos + 2u] == '.'))) {
        return RTC_MM_MONTH;
    }

    return RTC_MM_MINUTE;
}

static size_t fmt_match_token(const char *p) {
    if (strncmp(p, "YYYY", 4u) == 0) return 4u;
    if (strncmp(p, "MsMs", 4u) == 0) return 4u;
    if (strncmp(p, "TMS",  3u) == 0) return 3u;
    if (strncmp(p, "YY",   2u) == 0) return 2u;
    if (strncmp(p, "MO",   2u) == 0) return 2u;
    if (strncmp(p, "Mo",   2u) == 0) return 2u;
    if (strncmp(p, "MM",   2u) == 0) return 2u;
    if (strncmp(p, "DD",   2u) == 0) return 2u;
    if (strncmp(p, "HH",   2u) == 0) return 2u;
    if (strncmp(p, "hh",   2u) == 0) return 2u;
    if (strncmp(p, "SS",   2u) == 0) return 2u;
    if (strncmp(p, "Ms",   2u) == 0) return 2u;
    if (strncmp(p, "AM",   2u) == 0) return 2u;
    if (strncmp(p, "PM",   2u) == 0) return 2u;
    if (strncmp(p, "TT",   2u) == 0) return 2u;
    if (strncmp(p, "TS",   2u) == 0) return 2u;
    if (strncmp(p, "TM",   2u) == 0) return 2u;
    if (strncmp(p, "TH",   2u) == 0) return 2u;
    if (strncmp(p, "TD",   2u) == 0) return 2u;
    if (p[0] == 'Y' || p[0] == 'M' || p[0] == 'D' ||
        p[0] == 'H' || p[0] == 'h' || p[0] == 'S') {
        return 1u;
    }
    return 0u;
}

static rtc_status_t fmt_validate_syntax(const char *fmt) {
    if (!fmt) return RTC_ERR_INVALID_FORMAT;
    size_t len = strlen(fmt);
    if (len == 0u || len > RTC_FORMAT_MAX_LEN) return RTC_ERR_INVALID_FORMAT;

    bool has_nonspace = false;
    bool in_quote = false;
    for (size_t i = 0u; i < len; ) {
        char c = fmt[i];
        if (!rtc_is_space(c)) has_nonspace = true;
        if (c == '\'') {
            if (in_quote && i + 1u < len && fmt[i + 1u] == '\'') {
                i += 2u;
                continue;
            }
            in_quote = !in_quote;
            i++;
            continue;
        }
        if (in_quote) {
            i++;
            continue;
        }
        if (c == '%') {
            return RTC_ERR_INVALID_FORMAT;
        }
        if (rtc_is_alpha(c)) {
            size_t tlen = fmt_match_token(fmt + i);
            if (tlen == 0u) return RTC_ERR_INVALID_FORMAT;
            i += tlen;
            continue;
        }
        i++;
    }

    if (in_quote || !has_nonspace) return RTC_ERR_INVALID_FORMAT;
    return RTC_OK;
}

static char *rtc_format_datetime_internal(const rtc_datetime_t *dt, const char *fmt,
                                          bool require_hw_subsec, bool subsec_avail,
                                          size_t *out_len, rtc_status_t *out_err) {
    rtc_status_t syn = fmt_validate_syntax(fmt);
    if (syn != RTC_OK) {
        if (out_err) *out_err = syn;
        return NULL;
    }
    if (!cal_validate_datetime(dt)) {
        if (out_err) *out_err = RTC_ERR_INVALID_DATETIME;
        return NULL;
    }

    bool need_totals = (strstr(fmt, "TMS") != NULL || strstr(fmt, "TS") != NULL ||
                        strstr(fmt, "TM")  != NULL || strstr(fmt, "TH") != NULL ||
                        strstr(fmt, "TD")  != NULL);

    rtc_bigint_t t_days, t_hours, t_mins, t_secs, t_ms, year_bi;
    rtc_bigint_init(&t_days);
    rtc_bigint_init(&t_hours);
    rtc_bigint_init(&t_mins);
    rtc_bigint_init(&t_secs);
    rtc_bigint_init(&t_ms);
    rtc_bigint_init(&year_bi);

    year_bi.negative = dt->negative_year && !bu_is_zero(&dt->year_mag);
    if (!bu_copy(&year_bi.mag, &dt->year_mag)) {
        if (out_err) *out_err = RTC_ERR_NOMEM;
        goto cleanup;
    }

    if (need_totals && !cal_datetime_to_totals(dt, &t_days, &t_hours, &t_mins, &t_secs, &t_ms)) {
        if (out_err) *out_err = RTC_ERR_NOMEM;
        goto cleanup;
    }

    rtc_strbuf_t sb;
    sb_init(&sb);

    size_t len = strlen(fmt);
    bool in_quote = false;
    for (size_t i = 0u; i < len && !sb.failed; ) {
        char c = fmt[i];
        if (c == '\'') {
            if (in_quote && i + 1u < len && fmt[i + 1u] == '\'') {
                sb_append_ch(&sb, '\'');
                i += 2u;
                continue;
            }
            in_quote = !in_quote;
            i++;
            continue;
        }
        if (in_quote) {
            sb_append_ch(&sb, c);
            i++;
            continue;
        }
        if (!rtc_is_alpha(c)) {
            sb_append_ch(&sb, c);
            i++;
            continue;
        }

        const char *p = fmt + i;
        size_t tlen = fmt_match_token(p);
        if (tlen == 4u && strncmp(p, "YYYY", 4u) == 0) {
            sb_append_bigint_pad(&sb, &year_bi, 4u);
        } else if (tlen == 4u && strncmp(p, "MsMs", 4u) == 0) {
            if (require_hw_subsec && !subsec_avail) {
                sb.failed = true;
                if (out_err) *out_err = RTC_ERR_NO_SUBSECOND;
                break;
            }
            sb_append_uint(&sb, dt->ms, 3u, true);
        } else if (tlen == 3u && strncmp(p, "TMS", 3u) == 0) {
            sb_append_bigint_pad(&sb, &t_ms, 1u);
        } else if (tlen == 2u && strncmp(p, "YY", 2u) == 0) {
            uint32_t yy = bi_mod_u32(&year_bi, 100u);
            sb_append_uint(&sb, yy, 2u, true);
        } else if (tlen == 2u && (strncmp(p, "MO", 2u) == 0 || strncmp(p, "Mo", 2u) == 0)) {
            sb_append_uint(&sb, dt->month, 2u, true);
        } else if (tlen == 2u && strncmp(p, "MM", 2u) == 0) {
            if (fmt_classify_mm(fmt, i) == RTC_MM_MINUTE) {
                sb_append_uint(&sb, dt->minute, 2u, true);
            } else {
                sb_append_uint(&sb, dt->month, 2u, true);
            }
        } else if (tlen == 2u && strncmp(p, "DD", 2u) == 0) {
            sb_append_uint(&sb, dt->day, 2u, true);
        } else if (tlen == 2u && strncmp(p, "HH", 2u) == 0) {
            sb_append_uint(&sb, dt->hour, 2u, true);
        } else if (tlen == 2u && strncmp(p, "hh", 2u) == 0) {
            uint32_t h12 = dt->hour % 12u;
            if (h12 == 0u) h12 = 12u;
            sb_append_uint(&sb, h12, 2u, true);
        } else if (tlen == 2u && strncmp(p, "SS", 2u) == 0) {
            sb_append_uint(&sb, dt->second, 2u, true);
        } else if (tlen == 2u && strncmp(p, "Ms", 2u) == 0) {
            if (require_hw_subsec && !subsec_avail) {
                sb.failed = true;
                if (out_err) *out_err = RTC_ERR_NO_SUBSECOND;
                break;
            }
            sb_append_uint(&sb, dt->ms, 1u, false);
        } else if (tlen == 2u && (strncmp(p, "AM", 2u) == 0 ||
                                  strncmp(p, "PM", 2u) == 0 ||
                                  strncmp(p, "TT", 2u) == 0)) {
            sb_append_str(&sb, (dt->hour >= 12u) ? "PM" : "AM");
        } else if (tlen == 2u && strncmp(p, "TS", 2u) == 0) {
            sb_append_bigint_pad(&sb, &t_secs, 1u);
        } else if (tlen == 2u && strncmp(p, "TM", 2u) == 0) {
            sb_append_bigint_pad(&sb, &t_mins, 1u);
        } else if (tlen == 2u && strncmp(p, "TH", 2u) == 0) {
            sb_append_bigint_pad(&sb, &t_hours, 1u);
        } else if (tlen == 2u && strncmp(p, "TD", 2u) == 0) {
            sb_append_bigint_pad(&sb, &t_days, 1u);
        } else if (tlen == 1u && p[0] == 'Y') {
            sb_append_bigint_pad(&sb, &year_bi, 1u);
        } else if (tlen == 1u && p[0] == 'M') {
            sb_append_uint(&sb, dt->month, 1u, false);
        } else if (tlen == 1u && p[0] == 'D') {
            sb_append_uint(&sb, dt->day, 1u, false);
        } else if (tlen == 1u && p[0] == 'H') {
            sb_append_uint(&sb, dt->hour, 1u, false);
        } else if (tlen == 1u && p[0] == 'h') {
            uint32_t h12 = dt->hour % 12u;
            if (h12 == 0u) h12 = 12u;
            sb_append_uint(&sb, h12, 1u, false);
        } else if (tlen == 1u && p[0] == 'S') {
            sb_append_uint(&sb, dt->second, 1u, false);
        } else {
            sb.failed = true;
            if (out_err) *out_err = RTC_ERR_INVALID_FORMAT;
            break;
        }
        i += tlen;
    }

    char *res = sb_finish(&sb, out_len);
    if (!res && out_err && *out_err == RTC_OK) {
        *out_err = RTC_ERR_NOMEM;
    } else if (res && out_err) {
        *out_err = RTC_OK;
    }

    rtc_bigint_free(&t_days);
    rtc_bigint_free(&t_hours);
    rtc_bigint_free(&t_mins);
    rtc_bigint_free(&t_secs);
    rtc_bigint_free(&t_ms);
    rtc_bigint_free(&year_bi);
    return res;

cleanup:
    rtc_bigint_free(&t_days);
    rtc_bigint_free(&t_hours);
    rtc_bigint_free(&t_mins);
    rtc_bigint_free(&t_secs);
    rtc_bigint_free(&t_ms);
    rtc_bigint_free(&year_bi);
    return NULL;
}

/* ======================== Spec Parsing Helpers ============================= */

static bool parse_u32_range(const char *s, size_t n, uint32_t min_v, uint32_t max_v, uint32_t *out) {
    if (!s || n == 0u || n > 9u) return false;
    uint32_t v = 0u;
    for (size_t i = 0u; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10u + (uint32_t)(s[i] - '0');
    }
    if (v < min_v || v > max_v) return false;
    *out = v;
    return true;
}

static bool parse_date_token(const char *s, size_t len, bool *out_neg,
                             rtc_biguint_t *out_y, uint32_t *out_m, uint32_t *out_d) {
    if (!s || len == 0u) return false;

    /* Check YYYY-MM-DD (or signed -Y-MM-DD) */
    const char *p = s;
    const char *end = s + len;
    if (rtc_memchr(p, '-', len) != NULL) {
        bool neg = false;
        if (*p == '-') {
            neg = true;
            p++;
        } else if (*p == '+') {
            p++;
        }
        const char *d1 = (const char *)rtc_memchr(p, '-', (size_t)(end - p));
        if (!d1 || d1 == p) return false;
        const char *d2 = (const char *)rtc_memchr(d1 + 1u, '-', (size_t)(end - (d1 + 1u)));
        if (!d2 || d2 == d1 + 1u || d2 + 1u >= end) return false;
        if (rtc_memchr(d2 + 1u, '-', (size_t)(end - (d2 + 1u))) != NULL) return false;

        if (!bu_from_digits(out_y, p, (size_t)(d1 - p))) return false;
        *out_neg = neg && !bu_is_zero(out_y);
        return parse_u32_range(d1 + 1u, (size_t)(d2 - (d1 + 1u)), 1u, 12u, out_m) &&
               parse_u32_range(d2 + 1u, (size_t)(end - (d2 + 1u)), 1u, 31u, out_d);
    }

    /* Check DD.MM.YYYY */
    if (rtc_memchr(p, '.', len) != NULL) {
        const char *p1 = (const char *)rtc_memchr(p, '.', len);
        if (!p1 || p1 == p) return false;
        const char *p2 = (const char *)rtc_memchr(p1 + 1u, '.', (size_t)(end - (p1 + 1u)));
        if (!p2 || p2 == p1 + 1u || p2 + 1u >= end) return false;
        if (!parse_u32_range(p, (size_t)(p1 - p), 1u, 31u, out_d)) return false;
        if (!parse_u32_range(p1 + 1u, (size_t)(p2 - (p1 + 1u)), 1u, 12u, out_m)) return false;
        bool neg = false;
        const char *ys = p2 + 1u;
        if (*ys == '-') { neg = true; ys++; }
        if (ys >= end || !bu_from_digits(out_y, ys, (size_t)(end - ys))) return false;
        *out_neg = neg && !bu_is_zero(out_y);
        return true;
    }

    /* Check MM/DD/YYYY */
    if (rtc_memchr(p, '/', len) != NULL) {
        const char *s1 = (const char *)rtc_memchr(p, '/', len);
        if (!s1 || s1 == p) return false;
        const char *s2 = (const char *)rtc_memchr(s1 + 1u, '/', (size_t)(end - (s1 + 1u)));
        if (!s2 || s2 == s1 + 1u || s2 + 1u >= end) return false;
        if (!parse_u32_range(p, (size_t)(s1 - p), 1u, 12u, out_m)) return false;
        if (!parse_u32_range(s1 + 1u, (size_t)(s2 - (s1 + 1u)), 1u, 31u, out_d)) return false;
        bool neg = false;
        const char *ys = s2 + 1u;
        if (*ys == '-') { neg = true; ys++; }
        if (ys >= end || !bu_from_digits(out_y, ys, (size_t)(end - ys))) return false;
        *out_neg = neg && !bu_is_zero(out_y);
        return true;
    }

    return false;
}

static bool parse_time_token(const char *s, size_t len, uint32_t *out_h,
                             uint32_t *out_m, uint32_t *out_s, uint32_t *out_ms) {
    if (!s || len == 0u) return false;
    const char *end = s + len;

    bool has_ampm = false;
    bool is_pm = false;
    while (end > s && rtc_is_space(end[-1])) end--;
    if ((size_t)(end - s) >= 2u) {
        if ((end[-2] == 'A' || end[-2] == 'a') && (end[-1] == 'M' || end[-1] == 'm')) {
            has_ampm = true;
            is_pm = false;
            end -= 2u;
        } else if ((end[-2] == 'P' || end[-2] == 'p') && (end[-1] == 'M' || end[-1] == 'm')) {
            has_ampm = true;
            is_pm = true;
            end -= 2u;
        }
        while (end > s && rtc_is_space(end[-1])) end--;
    }

    /* Extract optional .ms or final :ms field. */
    uint32_t parts[4] = {0u, 0u, 0u, 0u};
    size_t nparts = 0u;
    const char *p = s;
    const char *dot = (const char *)rtc_memchr(s, '.', (size_t)(end - s));
    const char *colon_end = dot ? dot : end;

    while (p < colon_end && nparts < 4u) {
        const char *c = (const char *)rtc_memchr(p, ':', (size_t)(colon_end - p));
        const char *seg_end = c ? c : colon_end;
        uint32_t val = 0u;
        uint32_t lim = (nparts == 0u) ? 23u : ((nparts < 3u) ? 59u : 999u);
        if (!parse_u32_range(p, (size_t)(seg_end - p), 0u, lim, &val)) return false;
        parts[nparts++] = val;
        if (!c) break;
        p = c + 1u;
        if (p == colon_end) return false;
    }

    if (nparts < 2u || nparts > (dot ? 3u : 4u)) return false;
    if (dot) {
        if (dot + 1u >= end) return false;
        if (!parse_u32_range(dot + 1u, (size_t)(end - (dot + 1u)), 0u, 999u, &parts[3])) {
            return false;
        }
    }

    uint32_t h = parts[0];
    if (has_ampm) {
        if (h < 1u || h > 12u) return false;
        if (h == 12u) h = is_pm ? 12u : 0u;
        else if (is_pm) h += 12u;
    }

    *out_h = h;
    *out_m = parts[1];
    *out_s = (nparts >= 3u) ? parts[2] : 0u;
    *out_ms = (dot || nparts == 4u) ? parts[3] : 0u;
    return true;
}

static bool parse_datetime_spec_against_base(const char *spec, uint32_t kind,
                                             const rtc_datetime_t *base,
                                             rtc_datetime_t *out_dt,
                                             rtc_status_t *out_err) {
    if (!spec) {
        if (out_err) *out_err = RTC_ERR_INVALID_ARG;
        return false;
    }
    while (rtc_is_space(*spec)) spec++;
    size_t len = strlen(spec);
    while (len > 0u && rtc_is_space(spec[len - 1u])) len--;
    if (len == 0u) {
        if (out_err) *out_err = RTC_ERR_INVALID_ARG;
        return false;
    }

    if (kind == RTC_SET_MS || kind == RTC_SET_SEC || kind == RTC_SET_DAYS) {
        char *copy = (char *)kmalloc(len + 1u);
        if (!copy) {
            if (out_err) *out_err = RTC_ERR_NOMEM;
            return false;
        }
        memcpy(copy, spec, len);
        copy[len] = '\0';

        rtc_bigint_t v;
        rtc_bigint_init(&v);
        bool parsed = rtc_bigint_from_dec(&v, copy);
        kfree(copy);
        if (!parsed) {
            rtc_bigint_free(&v);
            if (out_err) *out_err = RTC_ERR_INVALID_DATETIME;
            return false;
        }

        bool ok = false;
        if (kind == RTC_SET_MS) {
            ok = cal_total_ms_to_datetime(&v, out_dt);
        } else if (kind == RTC_SET_SEC) {
            ok = cal_total_sec_to_datetime(&v, 0u, out_dt);
        } else {
            ok = cal_days_to_date_bigint(&v, out_dt);
            if (ok && base) {
                out_dt->hour = base->hour;
                out_dt->minute = base->minute;
                out_dt->second = base->second;
                out_dt->ms = base->ms;
            }
        }
        rtc_bigint_free(&v);
        if (!ok && out_err) *out_err = RTC_ERR_INVALID_DATETIME;
        return ok;
    }

    /* AUTO mode: try date + time, date only, time only, or raw seconds integer. */
    if (base && !rtc_datetime_copy(out_dt, base)) {
        if (out_err) *out_err = RTC_ERR_NOMEM;
        return false;
    }

    /* Split on space / 'T' before looking for time part (keep trailing AM/PM attached to time). */
    const char *sep = NULL;
    for (size_t i = 0u; i < len; i++) {
        if ((spec[i] == ' ' || spec[i] == '\t' || spec[i] == 'T') &&
            i > 0u && i + 1u < len &&
            (spec[i - 1u] >= '0' && spec[i - 1u] <= '9') &&
            (spec[i + 1u] >= '0' && spec[i + 1u] <= '9')) {
            sep = spec + i;
            break;
        }
    }

    if (sep) {
        size_t dlen = (size_t)(sep - spec);
        const char *ts = sep + 1u;
        while (*ts && rtc_is_space(*ts)) ts++;
        size_t tlen = (size_t)((spec + len) - ts);

        bool neg_y = false;
        rtc_biguint_t y_mag;
        bu_init(&y_mag);
        uint32_t m = 0u, d = 0u, hh = 0u, mm = 0u, ss = 0u, ms = 0u;
        bool ok = parse_date_token(spec, dlen, &neg_y, &y_mag, &m, &d) &&
                  parse_time_token(ts, tlen, &hh, &mm, &ss, &ms) &&
                  bu_copy(&out_dt->year_mag, &y_mag);
        bu_free(&y_mag);
        if (!ok) {
            if (out_err) *out_err = RTC_ERR_INVALID_DATETIME;
            return false;
        }
        out_dt->negative_year = neg_y;
        out_dt->month = m;
        out_dt->day = d;
        out_dt->hour = hh;
        out_dt->minute = mm;
        out_dt->second = ss;
        out_dt->ms = ms;
        if (!cal_validate_datetime(out_dt)) {
            if (out_err) *out_err = RTC_ERR_INVALID_DATETIME;
            return false;
        }
        return true;
    }

    /* Check single date token */
    if (rtc_memchr(spec, '.', len) || rtc_memchr(spec, '/', len) ||
        (len > 1u && rtc_memchr(spec + 1u, '-', len - 1u))) {
        bool neg_y = false;
        rtc_biguint_t y_mag;
        bu_init(&y_mag);
        uint32_t m = 0u, d = 0u;
        bool ok = parse_date_token(spec, len, &neg_y, &y_mag, &m, &d) &&
                  bu_copy(&out_dt->year_mag, &y_mag);
        bu_free(&y_mag);
        if (!ok) {
            if (out_err) *out_err = RTC_ERR_INVALID_DATETIME;
            return false;
        }
        out_dt->negative_year = neg_y;
        out_dt->month = m;
        out_dt->day = d;
        if (!cal_validate_datetime(out_dt)) {
            if (out_err) *out_err = RTC_ERR_INVALID_DATETIME;
            return false;
        }
        return true;
    }

    /* Check single time token */
    if (rtc_memchr(spec, ':', len)) {
        uint32_t hh = 0u, mm = 0u, ss = 0u, ms = 0u;
        if (!parse_time_token(spec, len, &hh, &mm, &ss, &ms)) {
            if (out_err) *out_err = RTC_ERR_INVALID_DATETIME;
            return false;
        }
        out_dt->hour = hh;
        out_dt->minute = mm;
        out_dt->second = ss;
        out_dt->ms = ms;
        if (!cal_validate_datetime(out_dt)) {
            if (out_err) *out_err = RTC_ERR_INVALID_DATETIME;
            return false;
        }
        return true;
    }

    /* Fallback: plain signed integer treated as seconds from 0000-01-01 */
    return parse_datetime_spec_against_base(spec, RTC_SET_SEC, base, out_dt, out_err);
}

static bool cal_add_unit(const rtc_datetime_t *base, const char *unit,
                         const char *amount_dec, rtc_datetime_t *out_dt,
                         rtc_status_t *out_err) {
    if (!base || !unit || !amount_dec || !out_dt) {
        if (out_err) *out_err = RTC_ERR_INVALID_ARG;
        return false;
    }

    rtc_bigint_t amt;
    rtc_bigint_init(&amt);
    if (!rtc_bigint_from_dec(&amt, amount_dec)) {
        rtc_bigint_free(&amt);
        if (out_err) *out_err = RTC_ERR_INVALID_ARG;
        return false;
    }

    if (rtc_strieq(unit, "y") || rtc_strieq(unit, "yr") || rtc_strieq(unit, "yrs") ||
        rtc_strieq(unit, "year") || rtc_strieq(unit, "years")) {
        if (!rtc_datetime_copy(out_dt, base)) goto oom;
        rtc_bigint_t y;
        rtc_bigint_init(&y);
        y.negative = base->negative_year && !bu_is_zero(&base->year_mag);
        if (!bu_copy(&y.mag, &base->year_mag) || !bi_add_signed(&y, &y, &amt) ||
            !bu_copy(&out_dt->year_mag, &y.mag)) {
            rtc_bigint_free(&y);
            goto oom;
        }
        out_dt->negative_year = y.negative && !bu_is_zero(&out_dt->year_mag);
        uint32_t max_d = cal_days_in_month(cal_is_leap_year_bigint(&y), out_dt->month);
        if (out_dt->day > max_d) out_dt->day = max_d;
        rtc_bigint_free(&y);
        rtc_bigint_free(&amt);
        return true;
    }

    if (rtc_strieq(unit, "mo") || rtc_strieq(unit, "mon") ||
        rtc_strieq(unit, "month") || rtc_strieq(unit, "months")) {
        if (!rtc_datetime_copy(out_dt, base)) goto oom;
        rtc_bigint_t total_m;
        rtc_bigint_init(&total_m);
        total_m.negative = base->negative_year && !bu_is_zero(&base->year_mag);
        if (!bu_copy(&total_m.mag, &base->year_mag) ||
            !bi_mul_u32(&total_m, 12u) ||
            !bi_add_i64(&total_m, (int64_t)base->month - 1LL) ||
            !bi_add_signed(&total_m, &total_m, &amt)) {
            rtc_bigint_free(&total_m);
            goto oom;
        }
        uint32_t m_rem = 0u;
        if (!bi_divmod_floor_u32(&total_m, 12u, &m_rem) ||
            !bu_copy(&out_dt->year_mag, &total_m.mag)) {
            rtc_bigint_free(&total_m);
            goto oom;
        }
        out_dt->negative_year = total_m.negative && !bu_is_zero(&out_dt->year_mag);
        out_dt->month = m_rem + 1u;
        uint32_t max_d = cal_days_in_month(cal_is_leap_year_bigint(&total_m), out_dt->month);
        if (out_dt->day > max_d) out_dt->day = max_d;
        rtc_bigint_free(&total_m);
        rtc_bigint_free(&amt);
        return true;
    }

    uint32_t ms_factor = 0u;
    if (rtc_strieq(unit, "ms") || rtc_strieq(unit, "msec") || rtc_strieq(unit, "millisecond") || rtc_strieq(unit, "milliseconds")) {
        ms_factor = 1u;
    } else if (rtc_strieq(unit, "s") || rtc_strieq(unit, "sec") || rtc_strieq(unit, "secs") || rtc_strieq(unit, "second") || rtc_strieq(unit, "seconds")) {
        ms_factor = 1000u;
    } else if (rtc_strieq(unit, "m") || rtc_strieq(unit, "min") || rtc_strieq(unit, "mins") || rtc_strieq(unit, "minute") || rtc_strieq(unit, "minutes")) {
        ms_factor = 60u * 1000u;
    } else if (rtc_strieq(unit, "h") || rtc_strieq(unit, "hr") || rtc_strieq(unit, "hrs") || rtc_strieq(unit, "hour") || rtc_strieq(unit, "hours")) {
        ms_factor = 3600u * 1000u;
    } else if (rtc_strieq(unit, "d") || rtc_strieq(unit, "day") || rtc_strieq(unit, "days")) {
        ms_factor = 86400u * 1000u;
    } else if (rtc_strieq(unit, "w") || rtc_strieq(unit, "wk") || rtc_strieq(unit, "wks") || rtc_strieq(unit, "week") || rtc_strieq(unit, "weeks")) {
        ms_factor = 7u * 86400u * 1000u;
    } else {
        rtc_bigint_free(&amt);
        if (out_err) *out_err = RTC_ERR_INVALID_ARG;
        return false;
    }

    rtc_bigint_t total_ms;
    rtc_bigint_init(&total_ms);
    if (!cal_datetime_to_totals(base, NULL, NULL, NULL, NULL, &total_ms) ||
        !bi_mul_u32(&amt, ms_factor) ||
        !bi_add_signed(&total_ms, &total_ms, &amt) ||
        !cal_total_ms_to_datetime(&total_ms, out_dt)) {
        rtc_bigint_free(&total_ms);
        goto oom;
    }
    rtc_bigint_free(&total_ms);
    rtc_bigint_free(&amt);
    return true;

oom:
    rtc_bigint_free(&amt);
    if (out_err) *out_err = RTC_ERR_NOMEM;
    return false;
}

/* ======================== Public Subsystem API ============================= */

void rtc_init(void) {
    rtc_bigint_t old_offset;
    rtc_bigint_init(&old_offset);

    uint64_t sf = spin_lock_irqsave(&s_rtc_state_lock);
    if (!s_time_format) s_time_format = rtc_strdup("HH:MM:SS");
    if (!s_date_format) s_date_format = rtc_strdup("YYYY-MM-DD");
    s_rtc_uses_subsecond = fmt_str_contains_subsecond(s_time_format);
    old_offset = s_epoch_offset_sec;
    rtc_bigint_init(&s_epoch_offset_sec);
    s_epoch_offset_active = false;
    spin_unlock_irqrestore(&s_rtc_state_lock, sf);

    rtc_bigint_free(&old_offset);

    s_century_reg = rtc_discover_fadt_century_reg(&s_century_from_fadt);

    uint64_t hf = spin_lock_irqsave(&s_rtc_hw_lock);
    uint8_t reg_d = cmos_read_unlocked(RTC_REG_STATUS_D);
    uint8_t reg_a = cmos_read_unlocked(RTC_REG_STATUS_A);
    uint8_t reg_b = cmos_read_unlocked(RTC_REG_STATUS_B);

    if (!(reg_d == 0xFFu && reg_b == 0xFFu)) {
        /* Select standard 32.768 kHz divider and 1024 Hz periodic crystal tick. */
        cmos_write_unlocked(RTC_REG_STATUS_A, (uint8_t)(RTC_A_DV_32KHZ | RTC_A_RATE_1024HZ));
        uint8_t next_b = (uint8_t)((reg_b & (RTC_B_DSE | RTC_B_24H | RTC_B_DM_BIN)) |
                                   RTC_B_PIE | RTC_B_UIE);
        cmos_write_unlocked(RTC_REG_STATUS_B, next_b);
        s_rtc_last_reg_c = cmos_read_unlocked(RTC_REG_STATUS_C);
        (void)reg_a;
    }
    s_rtc_pi_ticks = 0u;
    s_rtc_uf_dirty = true;
    s_rtc_initialized = true;
    spin_unlock_irqrestore(&s_rtc_hw_lock, hf);

    uint32_t irq_slot = irq_register_handler(8u, rtc_irq8_handler, NULL, 0u);
    s_rtc_irq8_registered = (irq_slot != 0xFFFFFFFFu);

    /* Clear any interrupt flag latched between programming Reg B and unmasking IRQ8. */
    hf = spin_lock_irqsave(&s_rtc_hw_lock);
    s_rtc_last_reg_c = cmos_read_unlocked(RTC_REG_STATUS_C);
    spin_unlock_irqrestore(&s_rtc_hw_lock, hf);

    (void)rtc_sync_from_cmos_locked_helper(true, NULL, NULL);
}

void rtc_poll(void) {
    if (!s_rtc_initialized) return;
    (void)rtc_sync_from_cmos_locked_helper(false, NULL, NULL);
}

bool rtc_resync_hw(void) {
    if (!s_rtc_initialized) return false;
    return rtc_sync_from_cmos_locked_helper(true, NULL, NULL);
}

bool rtc_is_ready(void) {
    rtc_poll();
    uint64_t sf = spin_lock_irqsave(&s_rtc_state_lock);
    bool ready = s_rtc_initialized && s_cached_valid && (s_last_error == RTC_OK);
    spin_unlock_irqrestore(&s_rtc_state_lock, sf);
    return ready;
}

bool rtc_irq8_active(void) {
    uint64_t hf = spin_lock_irqsave(&s_rtc_hw_lock);
    bool active = s_rtc_irq8_registered && (s_rtc_irq8_total > 0u);
    spin_unlock_irqrestore(&s_rtc_hw_lock, hf);
    return active;
}

bool rtc_has_subsecond(void) {
    bool subsec = false;
    (void)rtc_sync_from_cmos_locked_helper(false, NULL, &subsec);
    return subsec;
}

uint32_t rtc_generation(void) {
    return s_rtc_generation;
}

rtc_status_t rtc_last_error(void) {
    uint64_t sf = spin_lock_irqsave(&s_rtc_state_lock);
    rtc_status_t err = s_last_error;
    spin_unlock_irqrestore(&s_rtc_state_lock, sf);
    return err;
}

const char *rtc_status_str(rtc_status_t st) {
    switch (st) {
        case RTC_OK:                   return "OK";
        case RTC_ERR_NOT_READY:        return "RTC not initialized";
        case RTC_ERR_BUFFER_TOO_SMALL: return "Output buffer too small";
        case RTC_ERR_INVALID_ARG:      return "Invalid argument";
        case RTC_ERR_INVALID_FORMAT:   return "Invalid RTC format string";
        case RTC_ERR_INVALID_DATETIME: return "Invalid CMOS or input date/time";
        case RTC_ERR_HW_UNAVAILABLE:   return "Hardware RTC unavailable or VRT battery invalid";
        case RTC_ERR_HW_TIMEOUT:       return "Hardware CMOS RTC UIP timeout";
        case RTC_ERR_HW_RANGE:         return "Date outside MC146818 hardware CMOS range";
        case RTC_ERR_NO_SUBSECOND:     return "Subsecond precision (Ms) unavailable without active IRQ8 PIE";
        case RTC_ERR_NOMEM:            return "Out of memory";
        default:                       return "Unknown RTC error";
    }
}

void rtc_get_info(rtc_info_t *out_info) {
    if (!out_info) return;
    rtc_poll();

    uint64_t hf = spin_lock_irqsave(&s_rtc_hw_lock);
    uint32_t total = s_rtc_irq8_total;
    uint32_t pf = s_rtc_irq8_pf;
    uint32_t uf = s_rtc_irq8_uf;
    uint32_t ticks = s_rtc_pi_ticks;
    uint8_t last_c = s_rtc_last_reg_c;
    spin_unlock_irqrestore(&s_rtc_hw_lock, hf);

    uint64_t sf = spin_lock_irqsave(&s_rtc_state_lock);
    memset(out_info, 0, sizeof(*out_info));
    out_info->initialized = s_rtc_initialized;
    out_info->hw_ready = s_cached_valid && (s_last_error == RTC_OK);
    out_info->reg_a = s_last_raw.reg_a;
    out_info->reg_b = s_last_raw.reg_b;
    out_info->reg_c = last_c;
    out_info->reg_d = s_last_raw.reg_d;
    out_info->hw_stopped = (s_last_raw.reg_b & RTC_B_SET) != 0u ||
                           (s_last_raw.reg_a & RTC_A_DV_MASK) == 0u ||
                           (s_last_raw.reg_a & RTC_A_DV_MASK) >= 0x60u;
    out_info->battery_ok = (s_last_raw.reg_d & RTC_D_VRT) != 0u && s_last_raw.reg_d != 0xFFu;
    out_info->bcd_mode = (s_last_raw.reg_b & RTC_B_DM_BIN) == 0u;
    out_info->hour24_mode = (s_last_raw.reg_b & RTC_B_24H) != 0u;
    out_info->century_from_fadt = s_century_from_fadt;
    out_info->century_reg = s_century_reg;
    out_info->irq8_registered = s_rtc_irq8_registered;
    out_info->irq8_active = s_rtc_irq8_registered && total > 0u;
    out_info->subsecond_available = out_info->hw_ready && !out_info->hw_stopped &&
                                    out_info->irq8_active && pf > 0u &&
                                    ((s_last_raw.reg_b & RTC_B_PIE) != 0u);
    out_info->irq8_total_count = total;
    out_info->irq8_pf_count = pf;
    out_info->irq8_uf_count = uf;
    out_info->pi_ticks_in_sec = ticks;
    out_info->generation = s_rtc_generation;
    out_info->last_error = s_last_error;
    out_info->epoch_offset_active = s_epoch_offset_active;
    out_info->hw_year = s_cached_hw.year;
    out_info->hw_month = (uint8_t)s_cached_hw.month;
    out_info->hw_day = (uint8_t)s_cached_hw.day;
    out_info->hw_hour = (uint8_t)s_cached_hw.hour;
    out_info->hw_minute = (uint8_t)s_cached_hw.minute;
    out_info->hw_second = (uint8_t)s_cached_hw.second;
    spin_unlock_irqrestore(&s_rtc_state_lock, sf);
}

static bool rtc_snapshot_state_with_offset(bool sync_ok,
                                           rtc_hw_fields_t *out_hw,
                                           bool *out_have_off,
                                           rtc_bigint_t *out_off,
                                           rtc_status_t *out_err) {
    size_t want_limbs = 0u;
    for (;;) {
        if (want_limbs > 0u && !bu_ensure_cap(&out_off->mag, want_limbs)) {
            if (out_err) *out_err = RTC_ERR_NOMEM;
            return false;
        }
        uint64_t sf = spin_lock_irqsave(&s_rtc_state_lock);
        if (!sync_ok || !s_cached_valid) {
            rtc_status_t err = s_last_error;
            spin_unlock_irqrestore(&s_rtc_state_lock, sf);
            if (out_err) *out_err = err;
            return false;
        }
        *out_hw = s_cached_hw;
        if (!s_epoch_offset_active || bu_is_zero(&s_epoch_offset_sec.mag)) {
            *out_have_off = false;
            out_off->negative = false;
            out_off->mag.len = 0u;
            spin_unlock_irqrestore(&s_rtc_state_lock, sf);
            return true;
        }
        size_t need = s_epoch_offset_sec.mag.len;
        if (out_off->mag.cap >= need) {
            memcpy(out_off->mag.limbs, s_epoch_offset_sec.mag.limbs, need * sizeof(uint32_t));
            out_off->mag.len = need;
            out_off->negative = s_epoch_offset_sec.negative;
            *out_have_off = true;
            spin_unlock_irqrestore(&s_rtc_state_lock, sf);
            return true;
        }
        want_limbs = need;
        spin_unlock_irqrestore(&s_rtc_state_lock, sf);
    }
}

bool rtc_read_hw_datetime(rtc_datetime_t *out_dt, rtc_status_t *out_err) {
    if (!out_dt) {
        if (out_err) *out_err = RTC_ERR_INVALID_ARG;
        return false;
    }
    rtc_datetime_init(out_dt);

    if (!s_rtc_initialized) {
        if (out_err) *out_err = RTC_ERR_NOT_READY;
        return false;
    }

    uint32_t ms = 0u;
    bool ok = rtc_sync_from_cmos_locked_helper(false, &ms, NULL);

    rtc_hw_fields_t hw;
    bool have_offset = false;
    rtc_bigint_t offset_copy;
    rtc_bigint_init(&offset_copy);
    if (!rtc_snapshot_state_with_offset(ok, &hw, &have_offset, &offset_copy, out_err)) {
        rtc_bigint_free(&offset_copy);
        return false;
    }

    out_dt->negative_year = false;
    if (!bu_set_u64(&out_dt->year_mag, hw.year)) {
        rtc_bigint_free(&offset_copy);
        rtc_datetime_free(out_dt);
        if (out_err) *out_err = RTC_ERR_NOMEM;
        return false;
    }
    out_dt->month = hw.month;
    out_dt->day = hw.day;
    out_dt->hour = hw.hour;
    out_dt->minute = hw.minute;
    out_dt->second = hw.second;
    out_dt->ms = ms;

    if (!cal_validate_datetime(out_dt)) {
        rtc_bigint_free(&offset_copy);
        rtc_datetime_free(out_dt);
        if (out_err) *out_err = RTC_ERR_INVALID_DATETIME;
        return false;
    }

    if (have_offset) {
        rtc_bigint_t hw_sec, logical_sec;
        rtc_bigint_init(&hw_sec);
        rtc_bigint_init(&logical_sec);
        rtc_datetime_t logical_dt;
        rtc_datetime_init(&logical_dt);

        bool conv_ok = cal_datetime_to_totals(out_dt, NULL, NULL, NULL, &hw_sec, NULL) &&
                       bi_add_signed(&logical_sec, &hw_sec, &offset_copy) &&
                       cal_total_sec_to_datetime(&logical_sec, ms, &logical_dt);
        rtc_bigint_free(&hw_sec);
        rtc_bigint_free(&logical_sec);
        rtc_bigint_free(&offset_copy);

        if (!conv_ok) {
            rtc_datetime_free(&logical_dt);
            rtc_datetime_free(out_dt);
            if (out_err) *out_err = RTC_ERR_NOMEM;
            return false;
        }
        rtc_datetime_free(out_dt);
        *out_dt = logical_dt;
    } else {
        rtc_bigint_free(&offset_copy);
    }

    if (out_err) *out_err = RTC_OK;
    return true;
}

bool rtc_set_time_format(const char *fmt) {
    if (fmt_validate_syntax(fmt) != RTC_OK) return false;
    char *copy = rtc_strdup(fmt);
    if (!copy) return false;

    uint64_t sf = spin_lock_irqsave(&s_rtc_state_lock);
    char *old = s_time_format;
    s_time_format = copy;
    s_rtc_uses_subsecond = fmt_str_contains_subsecond(copy);
    s_rtc_generation++;
    spin_unlock_irqrestore(&s_rtc_state_lock, sf);

    if (old) kfree(old);
    return true;
}

char *rtc_get_time_format_alloc(void) {
    uint64_t sf = spin_lock_irqsave(&s_rtc_state_lock);
    const char *src = s_time_format ? s_time_format : "HH:MM:SS";
    char *copy = rtc_strdup(src);
    spin_unlock_irqrestore(&s_rtc_state_lock, sf);
    return copy;
}

bool rtc_set_date_format(const char *fmt) {
    if (fmt_validate_syntax(fmt) != RTC_OK) return false;
    char *copy = rtc_strdup(fmt);
    if (!copy) return false;

    uint64_t sf = spin_lock_irqsave(&s_rtc_state_lock);
    char *old = s_date_format;
    s_date_format = copy;
    s_rtc_generation++;
    spin_unlock_irqrestore(&s_rtc_state_lock, sf);

    if (old) kfree(old);
    return true;
}

char *rtc_get_date_format_alloc(void) {
    uint64_t sf = spin_lock_irqsave(&s_rtc_state_lock);
    const char *src = s_date_format ? s_date_format : "YYYY-MM-DD";
    char *copy = rtc_strdup(src);
    spin_unlock_irqrestore(&s_rtc_state_lock, sf);
    return copy;
}

bool rtc_set_format(const char *fmt) {
    return rtc_set_time_format(fmt);
}

char *rtc_get_format_alloc(void) {
    return rtc_get_time_format_alloc();
}

bool rtc_time_format_uses_subsecond(void) {
    return s_rtc_uses_subsecond;
}

bool rtc_format_uses_subsecond(const char *fmt) {
    return fmt_str_contains_subsecond(fmt);
}

char *rtc_format_alloc(const char *fmt, size_t *out_len, rtc_status_t *out_err) {
    const char *use_fmt = fmt;
    char *owned_fmt = NULL;
    if (!use_fmt) {
        owned_fmt = rtc_get_time_format_alloc();
        if (!owned_fmt) {
            if (out_err) *out_err = RTC_ERR_NOMEM;
            return NULL;
        }
        use_fmt = owned_fmt;
    }

    rtc_datetime_t dt;
    rtc_datetime_init(&dt);
    rtc_status_t st = RTC_OK;
    char *res = NULL;
    if (rtc_read_hw_datetime(&dt, &st)) {
        bool subsec = rtc_has_subsecond();
        res = rtc_format_datetime_internal(&dt, use_fmt, true, subsec, out_len, &st);
    }
    rtc_datetime_free(&dt);
    if (owned_fmt) kfree(owned_fmt);
    if (out_err) *out_err = st;
    return res;
}

char *rtc_format_time_alloc(size_t *out_len, rtc_status_t *out_err) {
    char *tfmt = rtc_get_time_format_alloc();
    if (!tfmt) {
        if (out_err) *out_err = RTC_ERR_NOMEM;
        return NULL;
    }
    char *res = rtc_format_alloc(tfmt, out_len, out_err);
    kfree(tfmt);
    return res;
}

char *rtc_format_date_alloc(size_t *out_len, rtc_status_t *out_err) {
    char *dfmt = rtc_get_date_format_alloc();
    if (!dfmt) {
        if (out_err) *out_err = RTC_ERR_NOMEM;
        return NULL;
    }
    char *res = rtc_format_alloc(dfmt, out_len, out_err);
    kfree(dfmt);
    return res;
}

char *rtc_format_now_alloc(size_t *out_len) {
    return rtc_format_time_alloc(out_len, NULL);
}

static int rtc_copy_str_into_caller_buf(char *str, size_t len, rtc_status_t err,
                                        char *out_buf, size_t cap) {
    if (!str) {
        return (err < 0) ? (int)err : (int)RTC_ERR_NOMEM;
    }
    if (!out_buf && cap == 0u) {
        kfree(str);
        return (int)len;
    }
    if (!out_buf || cap < len + 1u) {
        if (out_buf && cap > 0u) out_buf[0] = '\0';
        kfree(str);
        return (int)RTC_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(out_buf, str, len + 1u);
    kfree(str);
    return (int)len;
}

int rtc_format_buf(const char *fmt, char *out_buf, size_t cap) {
    size_t len = 0u;
    rtc_status_t err = RTC_OK;
    char *s = rtc_format_alloc(fmt, &len, &err);
    return rtc_copy_str_into_caller_buf(s, len, err, out_buf, cap);
}

int rtc_get_time_format_buf(char *out_buf, size_t cap) {
    char *s = rtc_get_time_format_alloc();
    size_t len = s ? strlen(s) : 0u;
    return rtc_copy_str_into_caller_buf(s, len, RTC_ERR_NOMEM, out_buf, cap);
}

int rtc_get_date_format_buf(char *out_buf, size_t cap) {
    char *s = rtc_get_date_format_alloc();
    size_t len = s ? strlen(s) : 0u;
    return rtc_copy_str_into_caller_buf(s, len, RTC_ERR_NOMEM, out_buf, cap);
}

char *rtc_get_total_ms_alloc(rtc_status_t *out_err) {
    return rtc_format_alloc("TMS", NULL, out_err);
}

char *rtc_get_total_sec_alloc(rtc_status_t *out_err) {
    return rtc_format_alloc("TS", NULL, out_err);
}

char *rtc_get_total_days_alloc(rtc_status_t *out_err) {
    return rtc_format_alloc("TD", NULL, out_err);
}

char *rtc_get_year_alloc(rtc_status_t *out_err) {
    return rtc_format_alloc("Y", NULL, out_err);
}

void rtc_get_seconds_parts(uint64_t *out_sec_low, uint32_t *out_ms) {
    rtc_datetime_t dt;
    rtc_datetime_init(&dt);
    if (!rtc_read_hw_datetime(&dt, NULL)) {
        if (out_sec_low) *out_sec_low = 0ULL;
        if (out_ms) *out_ms = 0u;
        return;
    }

    rtc_bigint_t t_secs;
    rtc_bigint_init(&t_secs);
    uint64_t sec_val = 0ULL;
    if (cal_datetime_to_totals(&dt, NULL, NULL, NULL, &t_secs, NULL)) {
        (void)bu_fits_u64(&t_secs.mag, &sec_val);
    }
    if (out_sec_low) *out_sec_low = sec_val;
    if (out_ms) *out_ms = dt.ms;
    rtc_bigint_free(&t_secs);
    rtc_datetime_free(&dt);
}

static void rtc_swap_epoch_offset_locked(bool active, rtc_bigint_t *new_offset,
                                         rtc_bigint_t *old_out) {
    *old_out = s_epoch_offset_sec;
    if (active && new_offset && !bu_is_zero(&new_offset->mag)) {
        s_epoch_offset_sec = *new_offset;
        s_epoch_offset_active = true;
        rtc_bigint_init(new_offset);
    } else {
        rtc_bigint_init(&s_epoch_offset_sec);
        s_epoch_offset_active = false;
    }
    s_rtc_generation++;
}

static rtc_status_t rtc_commit_datetime_to_clock(const rtc_datetime_t *dt) {
    if (!cal_validate_datetime(dt)) return RTC_ERR_INVALID_DATETIME;

    uint64_t year64 = 0ULL;
    bool fits_cmos_year = !dt->negative_year &&
                          bu_fits_u64(&dt->year_mag, &year64) &&
                          year64 <= 9999ULL &&
                          !(s_century_reg == 0u && (year64 < 1900ULL || year64 > 2099ULL));

    if (fits_cmos_year) {
        rtc_hw_fields_t hw = {
            .year = (uint32_t)year64,
            .month = dt->month,
            .day = dt->day,
            .hour = dt->hour,
            .minute = dt->minute,
            .second = dt->second
        };
        rtc_status_t st = rtc_cmos_write_hw(&hw, dt->ms);
        if (st == RTC_OK) {
            rtc_bigint_t old_offset;
            rtc_bigint_init(&old_offset);
            uint64_t sf = spin_lock_irqsave(&s_rtc_state_lock);
            rtc_swap_epoch_offset_locked(false, NULL, &old_offset);
            spin_unlock_irqrestore(&s_rtc_state_lock, sf);
            rtc_bigint_free(&old_offset);
            return RTC_OK;
        }
        if (st != RTC_ERR_HW_RANGE) {
            return st;
        }
    }

    /*
     * Target date is outside hardware/emulated CMOS limits (negative year, >4 digits,
     * missing century register, or platform/QEMU 64-bit rtc range): keep hardware CMOS +
     * IRQ8 running on a valid hardware anchor and store a signed BigInt second offset.
     */
    rtc_raw_hw_t anchor_raw;
    rtc_hw_fields_t anchor_fields;
    rtc_status_t ast = rtc_cmos_read_snapshot_hw(&anchor_raw, &anchor_fields);
    if (ast == RTC_ERR_HW_UNAVAILABLE || ast == RTC_ERR_HW_TIMEOUT) {
        return ast;
    }
    if (ast != RTC_OK) {
        anchor_fields = (rtc_hw_fields_t){ 2000u, 1u, 1u, 0u, 0u, 0u };
    }

    rtc_raw_hw_t live_raw;
    rtc_hw_fields_t live_fields;
    rtc_status_t pst = rtc_cmos_program_raw_hw(&anchor_fields, dt->ms, &live_raw, &live_fields);
    if (pst == RTC_ERR_HW_UNAVAILABLE || pst == RTC_ERR_HW_TIMEOUT) {
        return pst;
    }

    bool anchor_ok = (pst == RTC_OK);
    if (anchor_ok) {
        int64_t delta = rtc_hw_fields_to_sec_i64(&live_fields) -
                        rtc_hw_fields_to_sec_i64(&anchor_fields);
        if (delta < -1LL || delta > 2LL) {
            anchor_ok = false;
        }
    }
    if (!anchor_ok) {
        rtc_hw_fields_t safe_anchor = { 2000u, 1u, 1u, 0u, 0u, 0u };
        pst = rtc_cmos_program_raw_hw(&safe_anchor, dt->ms, &live_raw, &live_fields);
        if (pst != RTC_OK) {
            return pst;
        }
        int64_t delta = rtc_hw_fields_to_sec_i64(&live_fields) -
                        rtc_hw_fields_to_sec_i64(&safe_anchor);
        if (delta < -1LL || delta > 2LL) {
            return RTC_ERR_HW_RANGE;
        }
    }

    rtc_datetime_t hw_dt;
    rtc_datetime_init(&hw_dt);
    bool ok = bu_set_u64(&hw_dt.year_mag, live_fields.year);
    hw_dt.negative_year = false;
    hw_dt.month = live_fields.month;
    hw_dt.day = live_fields.day;
    hw_dt.hour = live_fields.hour;
    hw_dt.minute = live_fields.minute;
    hw_dt.second = live_fields.second;
    hw_dt.ms = 0u;

    rtc_bigint_t target_sec, hw_sec, neg_hw_sec, new_offset, old_offset;
    rtc_bigint_init(&target_sec);
    rtc_bigint_init(&hw_sec);
    rtc_bigint_init(&neg_hw_sec);
    rtc_bigint_init(&new_offset);
    rtc_bigint_init(&old_offset);

    if (ok) {
        ok = cal_datetime_to_totals(dt, NULL, NULL, NULL, &target_sec, NULL) &&
             cal_datetime_to_totals(&hw_dt, NULL, NULL, NULL, &hw_sec, NULL) &&
             bi_copy(&neg_hw_sec, &hw_sec);
    }
    if (ok) {
        neg_hw_sec.negative = !neg_hw_sec.negative;
        bi_normalize(&neg_hw_sec);
        ok = bi_add_signed(&new_offset, &target_sec, &neg_hw_sec);
    }

    rtc_datetime_free(&hw_dt);
    rtc_bigint_free(&target_sec);
    rtc_bigint_free(&hw_sec);
    rtc_bigint_free(&neg_hw_sec);

    if (!ok) {
        rtc_bigint_free(&new_offset);
        return RTC_ERR_NOMEM;
    }

    uint64_t sf = spin_lock_irqsave(&s_rtc_state_lock);
    s_last_raw = live_raw;
    s_cached_hw = live_fields;
    s_cached_valid = true;
    s_last_error = RTC_OK;
    rtc_swap_epoch_offset_locked(true, &new_offset, &old_offset);
    spin_unlock_irqrestore(&s_rtc_state_lock, sf);

    rtc_bigint_free(&new_offset);
    rtc_bigint_free(&old_offset);
    return RTC_OK;
}

bool rtc_set_components(const char *year_dec, uint32_t month, uint32_t day,
                        uint32_t hour, uint32_t minute, uint32_t second,
                        uint32_t ms, rtc_status_t *out_err) {
    if (!s_rtc_initialized) {
        if (out_err) *out_err = RTC_ERR_NOT_READY;
        return false;
    }
    rtc_bigint_t y;
    rtc_bigint_init(&y);
    if (!rtc_bigint_from_dec(&y, year_dec)) {
        rtc_bigint_free(&y);
        if (out_err) *out_err = RTC_ERR_INVALID_ARG;
        return false;
    }

    rtc_datetime_t dt;
    rtc_datetime_init(&dt);
    dt.negative_year = y.negative;
    if (!bu_copy(&dt.year_mag, &y.mag)) {
        rtc_bigint_free(&y);
        rtc_datetime_free(&dt);
        if (out_err) *out_err = RTC_ERR_NOMEM;
        return false;
    }
    dt.month = month;
    dt.day = day;
    dt.hour = hour;
    dt.minute = minute;
    dt.second = second;
    dt.ms = ms;
    rtc_bigint_free(&y);

    rtc_status_t st = rtc_commit_datetime_to_clock(&dt);
    rtc_datetime_free(&dt);
    if (out_err) *out_err = st;
    return st == RTC_OK;
}

bool rtc_set_spec(const char *spec, uint32_t kind, rtc_status_t *out_err) {
    if (!s_rtc_initialized) {
        if (out_err) *out_err = RTC_ERR_NOT_READY;
        return false;
    }

    rtc_datetime_t base;
    rtc_datetime_init(&base);
    bool have_base = rtc_read_hw_datetime(&base, NULL);
    if (!have_base && (kind == RTC_SET_AUTO || kind == RTC_SET_DAYS)) {
        /* Allow full explicit YYYY-MM-DD HH:MM:SS even if initial CMOS date was invalid. */
        have_base = false;
    }

    rtc_datetime_t target;
    rtc_datetime_init(&target);
    rtc_status_t err = RTC_OK;
    bool ok = parse_datetime_spec_against_base(spec, kind, have_base ? &base : NULL, &target, &err);
    if (ok) {
        err = rtc_commit_datetime_to_clock(&target);
        ok = (err == RTC_OK);
    }

    rtc_datetime_free(&base);
    rtc_datetime_free(&target);
    if (out_err) *out_err = err;
    return ok;
}

bool rtc_add_unit_dec(const char *unit, const char *amount_dec, rtc_status_t *out_err) {
    if (!s_rtc_initialized) {
        if (out_err) *out_err = RTC_ERR_NOT_READY;
        return false;
    }

    rtc_datetime_t base, next;
    rtc_datetime_init(&base);
    rtc_datetime_init(&next);

    rtc_status_t err = RTC_OK;
    bool ok = rtc_read_hw_datetime(&base, &err) &&
              cal_add_unit(&base, unit, amount_dec, &next, &err);
    if (ok) {
        err = rtc_commit_datetime_to_clock(&next);
        ok = (err == RTC_OK);
    }

    rtc_datetime_free(&base);
    rtc_datetime_free(&next);
    if (out_err) *out_err = err;
    return ok;
}
