#include <driver/input/keymap/keymap.h>
#include <string.h>

bool key_state[NUM_KEYS] = {false};
bool shift_mode = false;

bool keyboard_is_ctrl_pressed(void) {
    return key_state[SC_PRESS_CTRL];
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

char get_char(uint8_t scancode) {
    if (scancode == 0x00)
        return 0x00;

    char* keymap = get_keymap();
    return keymap[scancode];
}

void hot_key_handler(uint8_t scancode) {
    (void)scancode;
    shift_mode = keyboard_is_shift_pressed();
}

bool keyboard_dispatch_hotkey(uint8_t scancode) {
    (void)scancode;
    return false;
}

void keyboard_clear_key_state(void) {
    for (size_t i = 0; i < NUM_KEYS; i++) {
        key_state[i] = false;
    }
    shift_mode = false;
}

void update_hot_key_state(uint8_t scancode) {
    if (scancode == 0x00u) return;

    if (scancode < (uint8_t)NUM_KEYS) {
        key_state[scancode] = true;
    } else if (scancode >= 0x80u && (scancode & 0x7Fu) < (uint8_t)NUM_KEYS) {
        key_state[scancode & 0x7Fu] = false;
    }

    shift_mode = keyboard_is_shift_pressed();
}