/**
 * esc_driver - User-Space Emergency Shutdown Driver for IPO_OS
 *
 * All driver logic is completely self-contained in this file:
 * - Hooks into keyboard events via on_key
 * - Tracks ESC key hold duration via on_tick
 * - Triggers ACPI / APM hardware power-off when ESC is held for >= 10 seconds
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <syscall.h>
#include <kernel/driver.h>
#include <system/timer.h>
#include <ioport.h>

static bool s_esc_down = false;
static uint32_t s_esc_hold_start_ms = 0;
static uint32_t s_esc_last_progress_sec = 0;
static bool s_shutdown_triggered = false;

static void shutdown_system(void) {
    printf("\n[esc_driver] 10-second ESC hold detected! Shutting down system...\n");

    /* Modern ACPI power-off (QEMU / Bochs) */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);

    /* VirtualBox power-off */
    outw(0x4004, 0x3400);

    /* APM power-off */
    outw(0x5307, 0x0001);

    printf("[esc_driver] System halted. You may turn off your computer.\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

static bool esc_driver_on_key(uint8_t scancode, bool is_break) {
    uint8_t clean_sc = scancode & 0x7F;
    if (clean_sc == 0x01) { /* ESC scancode */
        s_esc_down = !is_break;
        if (is_break) {
            s_esc_hold_start_ms = 0;
            s_esc_last_progress_sec = 0;
        }
    }
    return false; /* do not consume key, allow normal input */
}

static void esc_driver_on_tick(void) {
    if (s_shutdown_triggered) return;

    if (s_esc_down) {
        uint32_t now = timer_millis();
        if (s_esc_hold_start_ms == 0) {
            s_esc_hold_start_ms = now;
            s_esc_last_progress_sec = 0;
        } else {
            uint32_t elapsed = now - s_esc_hold_start_ms;
            uint32_t sec = elapsed / 1000u;
            if (sec > 0 && sec != s_esc_last_progress_sec && sec < 10) {
                s_esc_last_progress_sec = sec;
                printf("[esc_driver] ESC held for %u/10 seconds...\n", (unsigned int)sec);
            }
            if (elapsed >= 10000u && !s_shutdown_triggered) {
                s_shutdown_triggered = true;
                shutdown_system();
            }
        }
    } else {
        s_esc_hold_start_ms = 0;
        s_esc_last_progress_sec = 0;
    }
}

static driver_t s_esc_driver;

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("[esc_driver] Initializing 10s ESC Emergency Shutdown Driver...\n");

    memset(&s_esc_driver, 0, sizeof(driver_t));
    s_esc_driver.name = "esc_driver";
    s_esc_driver.description = "10s ESC Key-Hold Emergency ACPI Shutdown Driver";
    s_esc_driver.flags = DRIVER_FLAG_USER | DRIVER_FLAG_ACTIVE;
    s_esc_driver.on_key = esc_driver_on_key;
    s_esc_driver.on_tick = esc_driver_on_tick;

    int res = ipo_driver_register(&s_esc_driver);
    if (res == 0) {
        printf("[esc_driver] Driver 'esc_driver' successfully registered in OS!\n");
        printf("[esc_driver] Hold the ESC key continuously for 10 seconds anytime to power off the PC.\n");
        printf("[esc_driver] Type 'driver' to view active drivers.\n");
    } else {
        printf("[esc_driver] Error registering driver: %d\n", res);
    }

    return res;
}
