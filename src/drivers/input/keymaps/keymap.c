#include <driver/input/keymap/keymap.h>
#include <kernel/driver.h>
#include <kernel/input_ext.h>
#include <memory/kmalloc.h>
#include <kernel/process.h>
#include <string.h>

static uint64_t s_early_keys_0 = 0;
static uint64_t s_early_keys_1 = 0;
static uint64_t s_early_keys_2 = 0;
static uint64_t s_early_keys_3 = 0;
bool *key_state = NULL;
uint32_t key_state_cap = 0;
bool shift_mode = false;

static bool early_key_get(uint32_t key) {
    if (key < 64u) return (s_early_keys_0 & (1ULL << key)) != 0;
    if (key < 128u) return (s_early_keys_1 & (1ULL << (key - 64u))) != 0;
    if (key < 192u) return (s_early_keys_2 & (1ULL << (key - 128u))) != 0;
    if (key < 256u) return (s_early_keys_3 & (1ULL << (key - 192u))) != 0;
    return false;
}

static void early_key_set(uint32_t key, bool val) {
    uint64_t bit = 1ULL << (key & 63u);
    uint64_t *slot = NULL;
    if (key < 64u) slot = &s_early_keys_0;
    else if (key < 128u) slot = &s_early_keys_1;
    else if (key < 192u) slot = &s_early_keys_2;
    else if (key < 256u) slot = &s_early_keys_3;
    if (!slot) return;
    if (val) *slot |= bit;
    else *slot &= ~bit;
}

static void key_state_ensure_capacity(uint32_t needed) {
    if (key_state && needed < key_state_cap) return;
    uint32_t new_cap = (key_state_cap == 0) ? 256u : (key_state_cap * 2u);
    while (new_cap <= needed) {
        new_cap *= 2u;
    }
    bool *new_arr = (bool *)kmalloc(new_cap * sizeof(bool));
    if (!new_arr) return;
    memset(new_arr, 0, new_cap * sizeof(bool));
    if (key_state && key_state_cap > 0) {
        memcpy(new_arr, key_state, key_state_cap * sizeof(bool));
        kfree(key_state);
    } else {
        for (uint32_t k = 0; k < 256u && k < new_cap; k++) {
            new_arr[k] = early_key_get(k);
        }
    }
    key_state = new_arr;
    key_state_cap = new_cap;
}

bool keyboard_is_key_pressed(uint32_t key) {
    if (!key_state) return early_key_get(key);
    if (key >= key_state_cap) return false;
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
    static char *s_kernel_keymap = NULL;
    if (!s_kernel_keymap) {
        s_kernel_keymap = (char *)kmalloc(256);
        if (!s_kernel_keymap) return NULL;
    }
    memset(s_kernel_keymap, 0, 256);
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
static bool layout_switch_triggered = false;

void keyboard_clear_key_state(void) {
    if (process_is_batch_active() && !process_is_foreground()) {
        return;
    }
    if (key_state && key_state_cap > 0) {
        memset(key_state, 0, key_state_cap * sizeof(bool));
    }
    s_early_keys_0 = 0;
    s_early_keys_1 = 0;
    s_early_keys_2 = 0;
    s_early_keys_3 = 0;
    shift_mode = false;
    e0_prefix = false;
    right_ctrl_pressed = false;
    layout_switch_triggered = false;
    input_ext_clear_key_state();
    driver_clear_key_state();
}

void update_hot_key_state(uint8_t scancode) {
    if (scancode == 0x00u) return;

    if (scancode == 0xE0u) {
        e0_prefix = true;
        return;
    }

    if (e0_prefix && (scancode == SC_PRESS_CTRL || scancode == (uint8_t)SC_UNPRESS_CTRL)) {
        right_ctrl_pressed = (scancode == SC_PRESS_CTRL);
        e0_prefix = false;
        shift_mode = keyboard_is_shift_pressed();
        goto check_layout_hotkey;
    }

    key_state_ensure_capacity(scancode);

    if (scancode < 0x80u) {
        if (key_state && scancode < key_state_cap) {
            key_state[scancode] = true;
        } else {
            early_key_set(scancode, true);
        }
    } else {
        uint8_t base = scancode & 0x7Fu;
        if (key_state) {
            if (base < key_state_cap) {
                key_state[base] = false;
            }
            if (scancode < key_state_cap) {
                key_state[scancode] = false;
            }
        } else {
            early_key_set(base, false);
            early_key_set(scancode, false);
        }
    }

    e0_prefix = false;
    shift_mode = keyboard_is_shift_pressed();

check_layout_hotkey:
    /* Hotkey: Ctrl+Shift or Alt+Shift cycles between enabled layouts */
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