#include <driver/input/keymap/keymap.h>

struct keyboard_struct available_keyboards[] = { 
    { "English", keymap_english, keymap_english_shift }
};
const size_t keyboards_count = sizeof(available_keyboards)/sizeof(available_keyboards[0]);
size_t current_index = 0;

bool shift_mode = false;
struct keyboard_struct* current_keyboard = &available_keyboards[0];

char* get_keymap(void) {
    if (shift_mode) {
        return (char*)current_keyboard->shift_keymap;
    } else {
        return (char*)current_keyboard->keymap;
    }
}

char get_char(uint8_t scancode) {
    if (scancode == 0x00)
        return 0x00;

    char* keymap = get_keymap();
    return keymap[scancode];
}

void switch_to_next_keyboard(void) {
    current_index++;
    if (current_index >= keyboards_count) {
        current_index = 0;
    }
    current_keyboard = &available_keyboards[current_index];
}


void hot_key_handler(uint8_t scancode) {
    shift_mode = (key_state[SC_PRESS_LEFT_SHIFT] || key_state[SC_PRESS_RIGHT_SHIFT]);

    if (key_state[SC_PRESS_CTRL] && (key_state[SC_PRESS_LEFT_SHIFT] || key_state[SC_PRESS_RIGHT_SHIFT])) {
        switch_to_next_keyboard();
    }
    
}

void update_hot_key_state(uint8_t scancode) {
    if (scancode == SC_PRESS_LEFT_SHIFT) {
        key_state[SC_PRESS_LEFT_SHIFT] = true;
    }
    if (scancode == SC_UNPRESS_LEFT_SHIFT) {
        key_state[SC_PRESS_LEFT_SHIFT] = false;
    }
    
    if (scancode == SC_PRESS_RIGHT_SHIFT) {
        key_state[SC_PRESS_RIGHT_SHIFT] = true;
    }
    if (scancode == SC_UNPRESS_RIGHT_SHIFT) {
        key_state[SC_PRESS_RIGHT_SHIFT] = false;
    }
    
    if (scancode == SC_PRESS_CTRL) {
        key_state[SC_PRESS_CTRL] = true;
    }
    if (scancode == SC_UNPRESS_CTRL) {
        key_state[SC_PRESS_CTRL] = false;
    }
    
    if (scancode == SC_PRESS_ALT) {
        key_state[SC_PRESS_ALT] = true;
    }
    if (scancode == SC_UNPRESS_ALT) {
        key_state[SC_PRESS_ALT] = false;
    }
}