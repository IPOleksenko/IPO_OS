/**
 * i_video.c - Platform video and input driver for DOOM on IPO_OS
 *
 * Implements Mode 13h VGA 320x200 output and PS/2 keyboard event translation.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"
#include "d_event.h"
#include "i_system.h"
#include "i_video.h"
#include "v_video.h"

#include <vga.h>
#include <vga_gfx.h>
#include <driver/input/keyboard.h>
#include <syscall.h>

extern void dynamic_keymap_reset(void);
extern void keyboard_clear_key_state(void);
extern uint32_t dynamic_keymap_get_count(void);
extern boolean dynamic_keymap_is_slot_enabled(uint32_t index);
extern const char* dynamic_keymap_get_slot_name(uint32_t index);
extern int dynamic_keymap_disable(const char *name_or_id);
extern int dynamic_keymap_enable(const char *name_or_id);
extern boolean system_is_interrupted(void);
extern void system_clear_interrupt(void);

static uint16_t s_saved_text_vram[80 * 25];
static uint16_t s_saved_cursor_pos = 0;
static boolean s_graphics_inited = false;
static boolean s_e0_prefix = false;
static uint32_t s_saved_keymap_count = 0;
static boolean  s_keymap_was_enabled[32];

void I_InitGraphics(void) {
    int i;
    volatile uint16_t *vram;

    if (s_graphics_inited) return;

    /* Backup text mode video buffer and cursor */
    vram = VGA_MEMORY;
    for (i = 0; i < 80 * 25; i++) {
        s_saved_text_vram[i] = vram[i];
    }
    s_saved_cursor_pos = vga_get_cursor_position();

    /* Disable other layouts so Shift+Ctrl / Shift+Alt during DOOM cannot switch layouts */
    s_saved_keymap_count = dynamic_keymap_get_count();
    if (s_saved_keymap_count > 32) s_saved_keymap_count = 32;
    for (i = 1; i < (int)s_saved_keymap_count; i++) {
        const char *name;
        s_keymap_was_enabled[i] = dynamic_keymap_is_slot_enabled((uint32_t)i);
        name = dynamic_keymap_get_slot_name((uint32_t)i);
        if (name) {
            dynamic_keymap_disable(name);
        }
    }
    dynamic_keymap_reset();
    keyboard_clear_key_state();

    /* Switch to 320x200 256-color Mode 13h */
    vga_set_mode_13h();
    keyboard_set_app_input_mode(true);
    keyboard_flush_app_queue();

    if (!screens[0]) {
        screens[0] = (byte *)malloc(SCREENWIDTH * SCREENHEIGHT);
    }
    if (screens[0]) {
        memset(screens[0], 0, SCREENWIDTH * SCREENHEIGHT);
    }

    s_graphics_inited = true;
}

void I_ShutdownGraphics(void) {
    int i;
    volatile uint16_t *vram;

    if (!s_graphics_inited) return;

    /* Re-enable previously enabled layouts */
    for (i = 1; i < (int)s_saved_keymap_count; i++) {
        if (s_keymap_was_enabled[i]) {
            const char *name = dynamic_keymap_get_slot_name((uint32_t)i);
            if (name) {
                dynamic_keymap_enable(name);
            }
        }
    }

    /* Reset keyboard layout back to English and clear any held modifier keys */
    dynamic_keymap_reset();
    keyboard_clear_key_state();
    keyboard_set_app_input_mode(false);
    keyboard_flush_queue();
    keyboard_flush_app_queue();
    keyboard_flush_hardware();

    vga_set_mode_text();

    /* Restore previous text mode screen content and cursor */
    vram = VGA_MEMORY;
    for (i = 0; i < 80 * 25; i++) {
        vram[i] = s_saved_text_vram[i];
    }
    vga_set_cursor(s_saved_cursor_pos);
    vga_show_cursor();

    s_graphics_inited = false;
}

void I_SetPalette(byte *palette) {
    int i;
    for (i = 0; i < 256; i++) {
        uint8_t r = palette[i * 3 + 0] >> 2;
        uint8_t g = palette[i * 3 + 1] >> 2;
        uint8_t b = palette[i * 3 + 2] >> 2;
        vga_gfx_set_palette((uint8_t)i, r, g, b);
    }
}

void I_UpdateNoBlit(void) {
}

void I_FinishUpdate(void) {
    if (!s_graphics_inited) {
        I_InitGraphics();
    }
    if (screens[0]) {
        vga_gfx_flip(screens[0]);
    }
    ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
}

void I_WaitVBL(int count) {
    while (count-- > 0) {
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
    }
}

void I_ReadScreen(byte *scr) {
    if (screens[0] && scr) {
        memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
    }
}

void I_BeginRead(void) {
}

void I_EndRead(void) {
}

void I_StartFrame(void) {
}

static int translate_scancode(uint8_t sc, boolean e0) {
    if (e0) {
        switch (sc) {
            case 0x48: return KEY_UPARROW;
            case 0x50: return KEY_DOWNARROW;
            case 0x4B: return KEY_LEFTARROW;
            case 0x4D: return KEY_RIGHTARROW;
            case 0x1D: return KEY_RCTRL;
            case 0x38: return KEY_RALT;
            default:   return 0;
        }
    }

    switch (sc) {
        case 0x39: return ' ';          /* Spacebar -> key_use */
        case 0x1C: return KEY_ENTER;
        case 0x01: return KEY_ESCAPE;
        case 0x0E: return KEY_BACKSPACE;
        case 0x0F: return KEY_TAB;
        case 0x1D: return KEY_RCTRL;     /* Left Ctrl -> key_fire */
        case 0x2A: return KEY_RSHIFT;    /* Left Shift */
        case 0x36: return KEY_RSHIFT;    /* Right Shift */
        case 0x38: return KEY_RALT;      /* Left Alt */

        /* Function keys */
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;

        /* Number row */
        case 0x02: return '1';
        case 0x03: return '2';
        case 0x04: return '3';
        case 0x05: return '4';
        case 0x06: return '5';
        case 0x07: return '6';
        case 0x08: return '7';
        case 0x09: return '8';
        case 0x0A: return '9';
        case 0x0B: return '0';
        case 0x0C: return KEY_MINUS;
        case 0x0D: return KEY_EQUALS;

        /* Letters row 1 */
        case 0x10: return 'q';
        case 0x11: return 'w';
        case 0x12: return 'e';
        case 0x13: return 'r';
        case 0x14: return 't';
        case 0x15: return 'y';
        case 0x16: return 'u';
        case 0x17: return 'i';
        case 0x18: return 'o';
        case 0x19: return 'p';
        case 0x1A: return '[';
        case 0x1B: return ']';

        /* Letters row 2 */
        case 0x1E: return 'a';
        case 0x1F: return 's';
        case 0x20: return 'd';
        case 0x21: return 'f';
        case 0x22: return 'g';
        case 0x23: return 'h';
        case 0x24: return 'j';
        case 0x25: return 'k';
        case 0x26: return 'l';
        case 0x27: return ';';
        case 0x28: return '\'';

        /* Letters row 3 */
        case 0x2C: return 'z';
        case 0x2D: return 'x';
        case 0x2E: return 'c';
        case 0x2F: return 'v';
        case 0x30: return 'b';
        case 0x31: return 'n';
        case 0x32: return 'm';
        case 0x33: return ',';
        case 0x34: return '.';
        case 0x35: return '/';

        default: return 0;
    }
}

void I_StartTic(void) {
    uint8_t sc;

    /* Handle Ctrl+C interrupt cleanly */
    if (system_is_interrupted()) {
        system_clear_interrupt();
        I_Quit();
        return;
    }

    while ((sc = keyboard_get_scancode()) != 0) {
        if (sc == 0xE0) {
            s_e0_prefix = true;
            continue;
        }

        boolean is_release = (sc & 0x80) != 0;
        uint8_t make_code = sc & 0x7F;

        int doom_key = translate_scancode(make_code, s_e0_prefix);
        s_e0_prefix = false;

        if (doom_key != 0) {
            event_t ev;
            ev.type = is_release ? ev_keyup : ev_keydown;
            ev.data1 = doom_key;
            ev.data2 = 0;
            ev.data3 = 0;
            D_PostEvent(&ev);
        }
    }
}
