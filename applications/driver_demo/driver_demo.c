/**
 * driver_demo - Real Hardware Driver Implementation for IPO_OS
 *
 * Demonstrates a real hardware device driver running in IPO_OS:
 * - Direct I/O communication with Motorola MC146818 CMOS RTC (ports 0x70 / 0x71)
 * - Hardware register decoding (BCD to binary, 24h mode)
 * - Registers 'cmos_rtc' into the kernel driver subsystem
 * - Extends OS shell with live hardware commands: 'date', 'time', and 'rtc'
 */

#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <kernel/driver.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t bcd_to_bin(uint8_t val) {
    return (uint8_t)(((val >> 4) * 10) + (val & 0x0F));
}

typedef struct {
    uint8_t sec;
    uint8_t min;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} rtc_date_time_t;

static void rtc_get_hardware_time(rtc_date_time_t *t) {
    int timeout = 100000;
    while ((cmos_read(0x0A) & 0x80) && --timeout > 0);

    uint8_t sec = cmos_read(0x00);
    uint8_t min = cmos_read(0x02);
    uint8_t hr  = cmos_read(0x04);
    uint8_t day = cmos_read(0x07);
    uint8_t mon = cmos_read(0x08);
    uint8_t yr  = cmos_read(0x09);
    uint8_t reg_b = cmos_read(0x0B);

    if (!(reg_b & 0x04)) {
        sec = bcd_to_bin(sec);
        min = bcd_to_bin(min);
        hr  = (uint8_t)(((hr & 0x7F) ? bcd_to_bin(hr & 0x7F) : 0) | (hr & 0x80));
        day = bcd_to_bin(day);
        mon = bcd_to_bin(mon);
        yr  = bcd_to_bin(yr);
    }

    if (!(reg_b & 0x02) && (hr & 0x80)) {
        hr = (uint8_t)(((hr & 0x7F) + 12) % 24);
    }

    t->sec = sec;
    t->min = min;
    t->hour = hr;
    t->day = day;
    t->month = mon;
    t->year = (uint16_t)(2000 + yr);
}

static int rtc_driver_command(const char *cmd, int argc, char **argv) {
    (void)argc; (void)argv;
    if (cmd == NULL) return -1;

    if (strcmp(cmd, "date") == 0) {
        rtc_date_time_t t;
        rtc_get_hardware_time(&t);
        printf("Current Date: %u-%s%u-%s%u\n",
               t.year,
               (t.month < 10 ? "0" : ""), (uint32_t)t.month,
               (t.day < 10 ? "0" : ""), (uint32_t)t.day);
        return 0;
    }

    if (strcmp(cmd, "time") == 0) {
        rtc_date_time_t t;
        rtc_get_hardware_time(&t);
        printf("Current Time: %s%u:%s%u:%s%u UTC\n",
               (t.hour < 10 ? "0" : ""), (uint32_t)t.hour,
               (t.min < 10 ? "0" : ""), (uint32_t)t.min,
               (t.sec < 10 ? "0" : ""), (uint32_t)t.sec);
        return 0;
    }

    if (strcmp(cmd, "rtc") == 0) {
        rtc_date_time_t t;
        rtc_get_hardware_time(&t);
        uint8_t reg_a = cmos_read(0x0A);
        uint8_t reg_b = cmos_read(0x0B);
        printf("====================== CMOS RTC HARDWARE ======================\n");
        printf("  Controller    : Motorola MC146818 Real-Time Clock & CMOS RAM\n");
        printf("  I/O Ports     : 0x70 (Index), 0x71 (Data)\n");
        printf("  Hardware Time : %u-%s%u-%s%u %s%u:%s%u:%s%u UTC\n",
               t.year,
               (t.month < 10 ? "0" : ""), (uint32_t)t.month,
               (t.day < 10 ? "0" : ""), (uint32_t)t.day,
               (t.hour < 10 ? "0" : ""), (uint32_t)t.hour,
               (t.min < 10 ? "0" : ""), (uint32_t)t.min,
               (t.sec < 10 ? "0" : ""), (uint32_t)t.sec);
        printf("  Status Reg A  : 0x%x (UIP: %d, Rate: %d)\n",
               reg_a, (reg_a >> 7) & 1, reg_a & 0x0F);
        printf("  Status Reg B  : 0x%x (Format: %s, Mode: %s)\n",
               reg_b, (reg_b & 0x04) ? "Binary" : "BCD", (reg_b & 0x02) ? "24-Hour" : "12-Hour");
        printf("===============================================================\n");
        return 0;
    }

    return -1;
}

static driver_t rtc_driver;

static void rtc_background_monitor(void) {
    /* Background periodic RTC monitor */
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("[driver_demo] Probing Motorola MC146818 CMOS Real-Time Clock...\n");
    rtc_date_time_t t;
    rtc_get_hardware_time(&t);
    printf("[driver_demo] Hardware Time read: %u-%s%u-%s%u %s%u:%s%u:%s%u UTC\n",
           t.year,
           (t.month < 10 ? "0" : ""), (uint32_t)t.month,
           (t.day < 10 ? "0" : ""), (uint32_t)t.day,
           (t.hour < 10 ? "0" : ""), (uint32_t)t.hour,
           (t.min < 10 ? "0" : ""), (uint32_t)t.min,
           (t.sec < 10 ? "0" : ""), (uint32_t)t.sec);

    /* Initialize driver dynamically with valid runtime function pointers */
    memset(&rtc_driver, 0, sizeof(driver_t));
    strcpy(rtc_driver.name, "cmos_rtc");
    strcpy(rtc_driver.description, "Motorola MC146818 CMOS Real-Time Clock");
    rtc_driver.flags = DRIVER_FLAG_USER | DRIVER_FLAG_ACTIVE;
    rtc_driver.on_command = rtc_driver_command;

    printf("[driver_demo] Registering real hardware driver 'cmos_rtc' with kernel...\n");

    int res = ipo_driver_register(&rtc_driver);
    if (res == 0) {
        /* Register async task to keep driver memory resident in background */
        uint32_t async_args[] = {
            (uint32_t)(uintptr_t)"cmos_rtc_service",
            10000u,
            (uint32_t)(uintptr_t)rtc_background_monitor
        };
        ipo_syscall(IPO_SYSCALL_ASYNC_START, 3u, async_args);

        printf("[driver_demo] Driver 'cmos_rtc' successfully registered in background!\n");
        printf("[driver_demo] Type 'driver' to see all active drivers.\n");
        printf("[driver_demo] New live shell commands active:\n");
        printf("    - date : Print current calendar date\n");
        printf("    - time : Print current real-time clock\n");
        printf("    - rtc  : Detailed CMOS hardware registers\n");
    } else {
        printf("[driver_demo] Registration failed with error %d\n", res);
    }
    return res;
}
