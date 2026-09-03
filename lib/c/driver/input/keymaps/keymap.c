#include <driver/input/keymap/keymap.h>
#include <string.h>

bool key_state[NUM_KEYS] = {false};
bool shift_mode = false;

bool keyboard_is_ctrl_pressed(void) {
    return key_state[SC_PRESS_CTRL];
}

bool keyboard_is_left_shift_pressed(void) {
    return key_state[SC_PRESS_LEFT_SHIFT];
}

bool keyboard_is_right_shift_pressed(void) {
    return key_state[SC_PRESS_RIGHT_SHIFT];
}

bool keyboard_is_shift_pressed(void) {
    return (key_state[SC_PRESS_LEFT_SHIFT] || key_state[SC_PRESS_RIGHT_SHIFT]);
}

bool keyboard_is_alt_pressed(void) {
    return key_state[SC_PRESS_ALT];
}

uint8_t keyboard_get_modifiers(void) {
    uint8_t mod = KBD_MOD_NONE;
    if (keyboard_is_ctrl_pressed())  mod |= KBD_MOD_CTRL;
    if (keyboard_is_shift_pressed()) mod |= KBD_MOD_SHIFT;
    if (keyboard_is_alt_pressed())   mod |= KBD_MOD_ALT;
    return mod;
}

char* get_keymap(void) {
    if (shift_mode) {
        return (char*)keymap_english_shift;
    } else {
        return (char*)keymap_english;
    }
}

static char static_char_buf[2] = {0, 0};

const char* keyboard_get_key_string(uint8_t scancode) {
    if (scancode == 0x00u || scancode >= 0x80u) {
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

    /* Check dynamic keymap translation */
    const char *dyn = dynamic_keymap_translate(scancode, shift_mode);
    if (dyn != NULL) {
        if (dyn[0] == '\0') {
            return ""; /* Key explicitly disabled, outputs nothing */
        }
        return dyn;
    }

    /* When a custom layout is active: unmapped typing keys output default symbol */
    if (dynamic_keymap_is_active()) {
        if (scancode == KEY_ENTER)     return "\n";
        if (scancode == KEY_BACKSPACE) return "\b";
        if (scancode == KEY_TAB)       return "\t";
        if (scancode == KEY_ESC)       return "\x1B";
        if (scancode == KEY_SPACE)     return " ";
        return dynamic_keymap_get_default_symbol();
    }

    char* keymap = get_keymap();
    char c = keymap[scancode];
    if (c != 0x00) {
        static_char_buf[0] = c;
        static_char_buf[1] = '\0';
        return static_char_buf;
    }

    return NULL;
}

char get_char(uint8_t scancode) {
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

bool keyboard_dispatch_hotkey(uint8_t scancode) {
    (void)scancode;
    return false;
}

static bool e0_prefix = false;
static bool right_ctrl_pressed = false;

void keyboard_clear_key_state(void) {
    for (size_t i = 0; i < NUM_KEYS; i++) {
        key_state[i] = false;
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

    if (scancode < (uint8_t)NUM_KEYS) {
        key_state[scancode] = true;
    } else if (scancode >= 0x80u && (scancode & 0x7Fu) < (uint8_t)NUM_KEYS) {
        key_state[scancode & 0x7Fu] = false;
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