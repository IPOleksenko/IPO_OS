#include <driver/input/keymap/keymap.h>
#include <memory/kmalloc.h>
#include <string.h>

static bool s_default_key_state[256] = {false};
bool *key_state = s_default_key_state;
uint32_t key_state_cap = 256;
bool shift_mode = false;

static void key_state_ensure_capacity(uint32_t needed) {
    if (needed < key_state_cap) return;
    uint32_t new_cap = (key_state_cap == 0) ? 256 : (key_state_cap * 2);
    while (new_cap <= needed) {
        new_cap *= 2;
    }
    bool *new_arr = (bool *)kmalloc(new_cap * sizeof(bool));
    if (!new_arr) return;
    memset(new_arr, 0, new_cap * sizeof(bool));
    if (key_state && key_state_cap > 0) {
        memcpy(new_arr, key_state, key_state_cap * sizeof(bool));
        if (key_state != s_default_key_state) {
            kfree(key_state);
        }
    }
    key_state = new_arr;
    key_state_cap = new_cap;
}

bool keyboard_is_key_pressed(uint32_t key) {
    if (!key_state || key >= key_state_cap) return false;
    return key_state[key];
}

static bool right_ctrl_pressed = false;
bool keyboard_is_ctrl_pressed(void) {
    return keyboard_is_key_pressed(SC_PRESS_CTRL) || right_ctrl_pressed;
}

bool keyboard_is_left_shift_pressed(void) {
    return keyboard_is_key_pressed(SC_PRESS_LEFT_SHIFT);
}

bool keyboard_is_right_shift_pressed(void) {
    return keyboard_is_key_pressed(SC_PRESS_RIGHT_SHIFT);
}

bool keyboard_is_shift_pressed(void) {
    return (keyboard_is_left_shift_pressed() || keyboard_is_right_shift_pressed());
}

bool keyboard_is_alt_pressed(void) {
    return keyboard_is_key_pressed(SC_PRESS_ALT);
}

uint8_t keyboard_get_modifiers(void) {
    uint8_t mod = KBD_MOD_NONE;
    if (keyboard_is_ctrl_pressed())  mod |= KBD_MOD_CTRL;
    if (keyboard_is_shift_pressed()) mod |= KBD_MOD_SHIFT;
    if (keyboard_is_alt_pressed())   mod |= KBD_MOD_ALT;
    return mod;
}

char* get_keymap(void) {
    static char s_kernel_keymap[256];
    memset(s_kernel_keymap, 0, sizeof(s_kernel_keymap));
    for (uint32_t sc = 0; sc < 256; sc++) {
        const char *s = dynamic_keymap_translate((uint8_t)sc, shift_mode);
        if (s != NULL && s[0] != '\0') {
            s_kernel_keymap[sc] = s[0];
        }
    }
    return s_kernel_keymap;
}

const char* keyboard_get_key_string(uint8_t scancode) {
    if (scancode == 0x00u) {
        return NULL;
    }

    /* Never emit characters for modifier keys */
    if (scancode == SC_PRESS_CTRL || scancode == SC_PRESS_LEFT_SHIFT ||
        scancode == SC_PRESS_RIGHT_SHIFT || scancode == SC_PRESS_ALT ||
        scancode == 0x3A /* CapsLock */ || scancode == 0x45 /* NumLock */ ||
        scancode == 0x46 /* ScrollLock */) {
        return NULL;
    }

    /* Never emit characters for navigation/editing/function keys */
    if (scancode == 0x48 /* Up */ || scancode == 0x50 /* Down */ ||
        scancode == 0x4B /* Left */ || scancode == 0x4D /* Right */ ||
        scancode == 0x47 /* Home */ || scancode == 0x4F /* End */ ||
        scancode == 0x49 /* PgUp */ || scancode == 0x51 /* PgDn */ ||
        scancode == 0x52 /* Insert */ || scancode == 0x53 /* Delete */ ||
        (scancode >= 0x3B && scancode <= 0x44) /* F1..F10 */ ||
        scancode == 0x57 /* F11 */ || scancode == 0x58 /* F12 */) {
        return NULL;
    }

    /* Check dynamic keymap translation (supports full scancode range) */
    const char *dyn = dynamic_keymap_translate(scancode, shift_mode);
    if (dyn != NULL) {
        if (dyn[0] == '\0') {
            return ""; /* Key explicitly disabled, outputs nothing */
        }
        return dyn;
    }

    /* Break codes (hardware key releases) do not emit text characters */
    if (scancode >= 0x80u) {
        return NULL;
    }

    /* When a custom layout is active: check control keys or custom fallback symbol */
    if (dynamic_keymap_is_active()) {
        if (scancode == KEY_ENTER)     return "\n";
        if (scancode == KEY_BACKSPACE) return "\b";
        if (scancode == KEY_TAB)       return "\t";
        if (scancode == KEY_ESC)       return "\x1B";
        if (scancode == KEY_SPACE)     return " ";
        const char *def = dynamic_keymap_get_default_symbol();
        if (def != NULL && def[0] != '\0') {
            return def;
        }
    }

    return NULL;
}

char __attribute__((weak)) get_char(uint8_t scancode) {
    if (scancode == 0x00)
        return 0x00;

    const char *s = keyboard_get_key_string(scancode);
    if (s != NULL && s[0] != '\0') {
        return s[0];
    }

    return 0x00;
}

void hot_key_handler(uint8_t scancode) {
    (void)scancode;
    shift_mode = keyboard_is_shift_pressed();
}


static bool e0_prefix = false;

void keyboard_clear_key_state(void) {
    if (key_state && key_state_cap > 0) {
        memset(key_state, 0, key_state_cap * sizeof(bool));
    }
    shift_mode = false;
    e0_prefix = false;
    right_ctrl_pressed = false;
}

void update_hot_key_state(uint8_t scancode) {
    if (scancode == 0x00u) return;

    if (scancode == 0xE0u) {
        e0_prefix = true;
        return;
    }

    if (scancode == SC_PRESS_CTRL) {
        if (e0_prefix) right_ctrl_pressed = true;
    } else if (scancode == (uint8_t)SC_UNPRESS_CTRL) {
        if (e0_prefix) right_ctrl_pressed = false;
    }

    key_state_ensure_capacity(scancode);

    if (scancode < 0x80u) {
        if (scancode < key_state_cap) {
            key_state[scancode] = true;
        }
    } else {
        uint8_t base = scancode & 0x7Fu;
        if (base < key_state_cap) {
            key_state[base] = false;
        }
        if (scancode < key_state_cap) {
            key_state[scancode] = false;
        }
    }

    e0_prefix = false;
    shift_mode = keyboard_is_shift_pressed();

    /* Hotkey: Ctrl + Shift or Alt + Shift cycles between registered keyboard layouts.
     * Left side (Left Shift / Left Ctrl) cycles backwards (previous layout).
     * Right side (Right Shift / Right Ctrl) cycles forwards (next layout).
     */
    static bool layout_switch_triggered = false;
    if ((keyboard_is_alt_pressed() && keyboard_is_shift_pressed()) ||
        (keyboard_is_ctrl_pressed() && keyboard_is_shift_pressed())) {
        if (!layout_switch_triggered) {
            layout_switch_triggered = true;
            if (right_ctrl_pressed || keyboard_is_right_shift_pressed()) {
                dynamic_keymap_cycle_next();
            } else {
                dynamic_keymap_cycle_prev();
            }
        }
    } else {
        layout_switch_triggered = false;
    }
}