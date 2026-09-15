#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <driver/vbe_bga.h>
#include <system/timer.h>
#include <system/state.h>
#include <driver/input/keyboard.h>
#include <syscall.h>

static void draw_truecolor_demo(uint32_t *fb, int w, int h) {
    /* 1. Background gradient (deep blue to magenta) */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t r = (uint8_t)((x * 255) / w);
            uint8_t g = (uint8_t)((y * 255) / h);
            uint8_t b = (uint8_t)(((w - x) * 255) / w);
            fb[y * w + x] = (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    /* 2. Pure Color test bars across the middle */
    int bar_y = h / 2 - 30;
    int bar_h = 60;
    for (int y = bar_y; y < bar_y + bar_h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t col = 0;
            int section = (x * 6) / w;
            switch (section) {
                case 0: col = 0xFFFF0000u; break; /* Red */
                case 1: col = 0xFF00FF00u; break; /* Green */
                case 2: col = 0xFF0000FFu; break; /* Blue */
                case 3: col = 0xFFFFFF00u; break; /* Yellow */
                case 4: col = 0xFF00FFFFu; break; /* Cyan */
                case 5: col = 0xFFFF00FFu; break; /* Magenta */
            }
            fb[y * w + x] = col;
        }
    }

    /* 3. Alpha-blended semi-transparent central rectangle (32-bit TrueColor) */
    int box_x0 = w / 2 - 120, box_x1 = w / 2 + 120;
    int box_y0 = h / 2 - 80,  box_y1 = h / 2 + 80;
    for (int y = box_y0; y < box_y1; y++) {
        for (int x = box_x0; x < box_x1; x++) {
            uint32_t bg = fb[y * w + x];
            uint8_t bg_r = (bg >> 16) & 0xFF;
            uint8_t bg_g = (bg >> 8) & 0xFF;
            uint8_t bg_b = bg & 0xFF;

            /* Blend with 50% white */
            uint8_t out_r = (uint8_t)((bg_r + 255) / 2);
            uint8_t out_g = (uint8_t)((bg_g + 255) / 2);
            uint8_t out_b = (uint8_t)((bg_b + 255) / 2);

            fb[y * w + x] = (0xFFu << 24) | ((uint32_t)out_r << 16) | ((uint32_t)out_g << 8) | (uint32_t)out_b;
        }
    }
}

#include <driver/input/keymap/keymap.h>

static void delay_ms(uint32_t ms) {
    uint32_t start = timer_millis();
    while (timer_elapsed_ms(start) < ms) {
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("[VBE BGA] Probing for VBE/Bochs BGA PCI display device...\n");
    vbe_bga_info_t info;
    if (!vbe_bga_probe(&info)) {
        printf("[VBE BGA] Hardware not detected. (Requires Bochs/QEMU BGA or compatible VBE PCI device).\n");
        return 1;
    }

    printf("[VBE BGA] Device detected! Version: 0x%x, LFB: 0x%x\n", info.version, info.lfb_addr);
    printf("[VBE BGA] Switching to 640x480 32-bit TrueColor (ARGB) LFB mode...\n");

    if (!vbe_bga_set_mode(640, 480, 32)) {
        printf("[VBE BGA] Failed to set video mode!\n");
        return 1;
    }

    uint32_t *fb = (uint32_t *)vbe_bga_get_framebuffer();
    if (fb) {
        draw_truecolor_demo(fb, 640, 480);
    }

    printf("[VBE BGA] Displaying 640x480 TrueColor test pattern (press 'q' or Ctrl+C to return)...\n");

    keyboard_flush_hardware();
    keyboard_flush_queue();
    keyboard_flush_app_queue();
    keyboard_set_app_input_mode(true);

    while (!system_is_interrupted()) {
        if (!vbe_bga_is_enabled()) {
            break;
        }
        uint8_t sc = keyboard_get_scancode();
        if (sc != 0x00u) {
            bool is_break = (sc & 0x80u) != 0;
            if (!is_break) {
                break;
            }
        }
        delay_ms(20);
    }

    keyboard_set_app_input_mode(false);
    system_clear_interrupt();

    vbe_bga_restore_text_mode();
    printf("\n[VBE BGA] Test successfully completed.\n");

    return 0;
}
