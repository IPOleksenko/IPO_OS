/**
 * reboot_driver - User-Space Interactive Dynamic Key-Combination Reboot Driver
 *
 * All driver logic is completely self-contained in this file:
 * - Prompts user to press any arbitrary combination of keys (no artificial key count limits)
 * - get_scancode_name supports both known and all unrecognized/custom scancodes (Key_0xXX)
 * - Hooks into keyboard events via on_key
 * - Tracks simultaneous hold duration via on_tick
 * - Triggers 8042 / Fast A20 / reset reboot when the recorded combination is held for >= 5 seconds
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <syscall.h>
#include <kernel/driver.h>
#include <driver/input/keyboard.h>
#include <system/timer.h>
#include <ioport.h>
#include <memory/kmalloc.h>

static char s_key_name_bufs[16][32];
static int s_key_name_buf_idx = 0;

static const char *get_scancode_name(uint8_t sc) {
    switch (sc) {
        case 0x01: return "Esc";
        case 0x02: return "1";
        case 0x03: return "2";
        case 0x04: return "3";
        case 0x05: return "4";
        case 0x06: return "5";
        case 0x07: return "6";
        case 0x08: return "7";
        case 0x09: return "8";
        case 0x0A: return "9";
        case 0x0B: return "0";
        case 0x0C: return "-";
        case 0x0D: return "=";
        case 0x0E: return "Backspace";
        case 0x0F: return "Tab";
        case 0x10: return "Q";
        case 0x11: return "W";
        case 0x12: return "E";
        case 0x13: return "R";
        case 0x14: return "T";
        case 0x15: return "Y";
        case 0x16: return "U";
        case 0x17: return "I";
        case 0x18: return "O";
        case 0x19: return "P";
        case 0x1A: return "[";
        case 0x1B: return "]";
        case 0x1C: return "Enter";
        case 0x1D: return "Ctrl";
        case 0x1E: return "A";
        case 0x1F: return "S";
        case 0x20: return "D";
        case 0x21: return "F";
        case 0x22: return "G";
        case 0x23: return "H";
        case 0x24: return "J";
        case 0x25: return "K";
        case 0x26: return "L";
        case 0x27: return ";";
        case 0x28: return "'";
        case 0x29: return "`";
        case 0x2A: return "Shift";
        case 0x2B: return "\\";
        case 0x2C: return "Z";
        case 0x2D: return "X";
        case 0x2E: return "C";
        case 0x2F: return "V";
        case 0x30: return "B";
        case 0x31: return "N";
        case 0x32: return "M";
        case 0x33: return ",";
        case 0x34: return ".";
        case 0x35: return "/";
        case 0x36: return "RShift";
        case 0x37: return "Keypad*";
        case 0x38: return "Alt";
        case 0x39: return "Space";
        case 0x3A: return "CapsLock";
        case 0x3B: return "F1";
        case 0x3C: return "F2";
        case 0x3D: return "F3";
        case 0x3E: return "F4";
        case 0x3F: return "F5";
        case 0x40: return "F6";
        case 0x41: return "F7";
        case 0x42: return "F8";
        case 0x43: return "F9";
        case 0x44: return "F10";
        case 0x45: return "NumLock";
        case 0x46: return "ScrollLock";
        case 0x47: return "Home";
        case 0x48: return "Up";
        case 0x49: return "PageUp";
        case 0x4A: return "Keypad-";
        case 0x4B: return "Left";
        case 0x4C: return "Keypad5";
        case 0x4D: return "Right";
        case 0x4E: return "Keypad+";
        case 0x4F: return "End";
        case 0x50: return "Down";
        case 0x51: return "PageDown";
        case 0x52: return "Insert";
        case 0x53: return "Delete";
        case 0x57: return "F11";
        case 0x58: return "F12";
        default: {
            /* Support any unrecognized or custom scancode without restriction */
            char *buf = s_key_name_bufs[s_key_name_buf_idx];
            s_key_name_buf_idx = (s_key_name_buf_idx + 1) % 16;
            snprintf(buf, 32, "Key_0x%02X", sc);
            return buf;
        }
    }
}

/* Dynamically allocated combination array (no arbitrary 8-key limit) */
static uint8_t *s_combo_keys = NULL;
static int s_combo_count = 0;

static bool s_keys_down[128] = {false};
static uint32_t s_hold_start_ms = 0;
static uint32_t s_last_progress_sec = 0;
static bool s_reboot_triggered = false;

static void reboot_system(void) {
    printf("\n[reboot_driver] 5-second hold detected! Rebooting system...\n");
    for (volatile int i = 0; i < 500000; i++) io_wait();

    /* 8042 Keyboard Controller reset */
    while (inb(0x64) & 0x02) io_wait();
    outb(0x64, 0xFE);

    /* Fast A20 / reset port 0x92 */
    uint8_t temp = inb(0x92);
    outb(0x92, (temp | 0x01));

    /* Fallback: Triple fault via null IDT */
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile("lidt %0; int3" : : "m"(null_idt));

    for (;;) {
        __asm__ volatile("hlt");
    }
}

static bool reboot_driver_on_key(uint8_t scancode, bool is_break) {
    uint8_t clean_sc = scancode & 0x7F;
    if (clean_sc < 128) {
        s_keys_down[clean_sc] = !is_break;
        if (is_break) {
            s_hold_start_ms = 0;
            s_last_progress_sec = 0;
        }
    }
    return false; /* do not consume key, allow normal typing in shell/applications */
}

static void reboot_driver_on_tick(void) {
    if (s_combo_count == 0 || s_reboot_triggered) return;

    bool all_down = true;
    for (int i = 0; i < s_combo_count; i++) {
        uint8_t sc = s_combo_keys[i];
        if (sc >= 128 || !s_keys_down[sc]) {
            all_down = false;
            break;
        }
    }

    if (all_down) {
        uint32_t now = timer_millis();
        if (s_hold_start_ms == 0) {
            s_hold_start_ms = now;
            s_last_progress_sec = 0;
        } else {
            uint32_t elapsed = now - s_hold_start_ms;
            uint32_t sec = elapsed / 1000u;
            if (sec > 0 && sec != s_last_progress_sec && sec < 5) {
                s_last_progress_sec = sec;
                printf("[reboot_driver] Combo held for %u/5 seconds...\n", (unsigned int)sec);
            }
            if (elapsed >= 5000u && !s_reboot_triggered) {
                s_reboot_triggered = true;
                reboot_system();
            }
        }
    } else {
        s_hold_start_ms = 0;
        s_last_progress_sec = 0;
    }
}

static driver_t s_reboot_driver;

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("[reboot_driver] ====================================================\n");
    printf("[reboot_driver] Initializing Dynamic Reboot Driver...\n");
    printf("[reboot_driver] Waiting for key combination...\n");
    printf("[reboot_driver] Press any desired key combination on your keyboard:\n");
    printf("[reboot_driver] (press and hold keys together, then release)\n");
    printf("[reboot_driver] ====================================================\n\n");

    keyboard_flush_hardware();
    keyboard_flush_queue();
    keyboard_flush_app_queue();
    keyboard_set_app_input_mode(true);

    /* Support up to all 128 scancodes without arbitrary limit */
    uint8_t recorded_keys[128];
    int recorded_count = 0;

    bool current_pressed[128];
    memset(current_pressed, 0, sizeof(current_pressed));

    bool combo_captured = false;
    uint32_t last_key_time = 0;
    int max_simultaneous = 0;

    while (!combo_captured) {
        uint8_t sc = keyboard_get_scancode();
        uint32_t now = timer_millis();

        if (sc != 0) {
            bool is_break = (sc & 0x80u) != 0;
            uint8_t clean_sc = sc & 0x7Fu;

            if (clean_sc > 0 && clean_sc < 128) {
                if (!is_break) {
                    /* Key pressed (make) */
                    if (!current_pressed[clean_sc]) {
                        current_pressed[clean_sc] = true;
                        last_key_time = now;

                        /* Add to recorded keys if not already present */
                        bool exists = false;
                        for (int i = 0; i < recorded_count; i++) {
                            if (recorded_keys[i] == clean_sc) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists && recorded_count < 128) {
                            recorded_keys[recorded_count++] = clean_sc;
                        }

                        /* Print key name (supports any unrecognized key seamlessly) */
                        const char *kn = get_scancode_name(clean_sc);
                        printf("   [+] Key pressed: '%s' (scancode: 0x%02x)\n", kn, clean_sc);

                        /* Count simultaneous pressed keys */
                        int sim_count = 0;
                        for (int i = 0; i < 128; i++) {
                            if (current_pressed[i]) sim_count++;
                        }
                        if (sim_count > max_simultaneous) {
                            max_simultaneous = sim_count;
                        }
                    }
                } else {
                    /* Key released (break) */
                    current_pressed[clean_sc] = false;
                    last_key_time = now;
                }
            }
        }

        /* If at least one key was pressed, and now all keys are released (or timeout) */
        if (recorded_count > 0 && last_key_time > 0) {
            int pressed_now = 0;
            for (int i = 0; i < 128; i++) {
                if (current_pressed[i]) pressed_now++;
            }

            if (pressed_now == 0 || (now - last_key_time > 1200)) {
                combo_captured = true;
            }
        }

        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
    }

    keyboard_set_app_input_mode(false);
    keyboard_flush_hardware();
    keyboard_flush_app_queue();

    printf("\n[reboot_driver] ====================================================\n");
    printf("[reboot_driver] Key combination recorded (%d key%s):\n   ", recorded_count, (recorded_count == 1) ? "" : "s");
    for (int i = 0; i < recorded_count; i++) {
        const char *kn = get_scancode_name(recorded_keys[i]);
        printf("[%s]%s", kn, (i + 1 < recorded_count) ? " + " : "");
    }
    printf("\n[reboot_driver] ====================================================\n");

    /* Allocate and copy combination dynamically */
    s_combo_count = recorded_count;
    s_combo_keys = (uint8_t *)kmalloc(s_combo_count);
    if (s_combo_keys) {
        memcpy(s_combo_keys, recorded_keys, s_combo_count);
    }

    /* Register driver with kernel driver subsystem */
    memset(&s_reboot_driver, 0, sizeof(driver_t));
    s_reboot_driver.name = "reboot_driver";
    s_reboot_driver.description = "5s Dynamic Key-Combination Reboot Driver";
    s_reboot_driver.flags = DRIVER_FLAG_USER | DRIVER_FLAG_ACTIVE;
    s_reboot_driver.on_key = reboot_driver_on_key;
    s_reboot_driver.on_tick = reboot_driver_on_tick;

    int res = ipo_driver_register(&s_reboot_driver);
    if (res == 0) {
        printf("[reboot_driver] Driver 'reboot_driver' successfully registered in OS!\n");
        printf("[reboot_driver] Hold the recorded key combination continuously for 5 seconds\n");
        printf("[reboot_driver] anytime to reboot the OS.\n");
        printf("[reboot_driver] Type 'driver' to view active drivers.\n");
    } else {
        printf("[reboot_driver] Error registering driver: %d\n", res);
    }

    return res;
}
