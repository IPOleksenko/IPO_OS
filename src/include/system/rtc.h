#ifndef SYSTEM_RTC_H
#define SYSTEM_RTC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define RTC_FORMAT_MAX_LEN   256u

#define RTC_SET_AUTO         0u
#define RTC_SET_MS           1u
#define RTC_SET_SEC          2u
#define RTC_SET_DAYS         3u

typedef enum {
    RTC_OK = 0,
    RTC_ERR_NOT_READY = -1,
    RTC_ERR_BUFFER_TOO_SMALL = -2,
    RTC_ERR_INVALID_ARG = -3,
    RTC_ERR_INVALID_FORMAT = -4,
    RTC_ERR_INVALID_DATETIME = -5,
    RTC_ERR_HW_UNAVAILABLE = -6,
    RTC_ERR_HW_TIMEOUT = -7,
    RTC_ERR_HW_RANGE = -8,
    RTC_ERR_NO_SUBSECOND = -9,
    RTC_ERR_NOMEM = -10
} rtc_status_t;

typedef struct {
    uint32_t *limbs; /* Base-10^9 limbs stored little-endian. */
    size_t len;
    size_t cap;
} rtc_biguint_t;

typedef struct {
    bool negative;
    rtc_biguint_t mag;
} rtc_bigint_t;

typedef struct {
    bool negative_year;
    rtc_biguint_t year_mag;
    uint32_t month;  /* 1..12 */
    uint32_t day;    /* 1..31 */
    uint32_t hour;   /* 0..23 */
    uint32_t minute; /* 0..59 */
    uint32_t second; /* 0..59 */
    uint32_t ms;     /* 0..999 */
} rtc_datetime_t;

typedef struct {
    bool initialized;
    bool hw_ready;
    bool hw_stopped;
    bool battery_ok;
    bool irq8_registered;
    bool irq8_active;
    bool subsecond_available;
    bool bcd_mode;
    bool hour24_mode;
    bool century_from_fadt;
    uint8_t century_reg;
    uint8_t reg_a;
    uint8_t reg_b;
    uint8_t reg_c;
    uint8_t reg_d;
    uint32_t irq8_total_count;
    uint32_t irq8_pf_count;
    uint32_t irq8_uf_count;
    uint32_t pi_ticks_in_sec;
    uint32_t generation;
    rtc_status_t last_error;
    bool epoch_offset_active;
    uint32_t hw_year;
    uint8_t hw_month;
    uint8_t hw_day;
    uint8_t hw_hour;
    uint8_t hw_minute;
    uint8_t hw_second;
} rtc_info_t;

/* Lifecycle, interrupt polling, and hardware state */
void rtc_init(void);
void rtc_poll(void);
bool rtc_resync_hw(void);
bool rtc_is_ready(void);
bool rtc_irq8_active(void);
bool rtc_has_subsecond(void);
uint32_t rtc_generation(void);
rtc_status_t rtc_last_error(void);
const char *rtc_status_str(rtc_status_t st);
void rtc_get_info(rtc_info_t *out_info);

/* Independent time and date format settings */
bool rtc_set_time_format(const char *fmt);
char *rtc_get_time_format_alloc(void);
bool rtc_set_date_format(const char *fmt);
char *rtc_get_date_format_alloc(void);
bool rtc_set_format(const char *fmt);
char *rtc_get_format_alloc(void);
bool rtc_time_format_uses_subsecond(void);
bool rtc_format_uses_subsecond(const char *fmt);

/* Heap-allocated formatters */
char *rtc_format_alloc(const char *fmt, size_t *out_len, rtc_status_t *out_err);
char *rtc_format_time_alloc(size_t *out_len, rtc_status_t *out_err);
char *rtc_format_date_alloc(size_t *out_len, rtc_status_t *out_err);
char *rtc_format_now_alloc(size_t *out_len);

/* Caller-buffer formatters: return >= 0 length, or negative rtc_status_t error */
int rtc_format_buf(const char *fmt, char *out_buf, size_t cap);
int rtc_get_time_format_buf(char *out_buf, size_t cap);
int rtc_get_date_format_buf(char *out_buf, size_t cap);

/* Hardware read helpers */
void rtc_datetime_init(rtc_datetime_t *dt);
void rtc_datetime_free(rtc_datetime_t *dt);
bool rtc_read_hw_datetime(rtc_datetime_t *out_dt, rtc_status_t *out_err);
char *rtc_get_total_ms_alloc(rtc_status_t *out_err);
char *rtc_get_total_sec_alloc(rtc_status_t *out_err);
char *rtc_get_total_days_alloc(rtc_status_t *out_err);
char *rtc_get_year_alloc(rtc_status_t *out_err);
void rtc_get_seconds_parts(uint64_t *out_sec_low, uint32_t *out_ms);

/* Setting date/time (writes CMOS directly when in range, else anchors BigInt epoch offset to CMOS) */
bool rtc_set_components(const char *year_dec, uint32_t month, uint32_t day,
                        uint32_t hour, uint32_t minute, uint32_t second,
                        uint32_t ms, rtc_status_t *out_err);
bool rtc_set_spec(const char *spec, uint32_t kind, rtc_status_t *out_err);
bool rtc_add_unit_dec(const char *unit, const char *amount_dec, rtc_status_t *out_err);

/* Signed arbitrary-precision integer helpers */
void rtc_bigint_init(rtc_bigint_t *v);
void rtc_bigint_free(rtc_bigint_t *v);
bool rtc_bigint_from_dec(rtc_bigint_t *out, const char *s);
char *rtc_bigint_to_dec_alloc(const rtc_bigint_t *v);

#endif
