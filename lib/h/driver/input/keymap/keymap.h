#ifndef _KEYMAP_H
#define _KEYMAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SC_PRESS_LEFT_SHIFT     0x2A
#define SC_UNPRESS_LEFT_SHIFT   0xAA

#define SC_PRESS_RIGHT_SHIFT    0x36
#define SC_UNPRESS_RIGHT_SHIFT  0xB6

#define SC_PRESS_CTRL           0x1D
#define SC_UNPRESS_CTRL         0x9D

#define SC_PRESS_ALT            0x38
#define SC_UNPRESS_ALT          0xB8

#define NUM_KEYS 128
static bool key_state[NUM_KEYS];

struct keyboard_struct {
    const char* name;
    const char* keymap;
    const char* shift_keymap;
};

extern struct keyboard_struct available_keyboards[];
extern const size_t keyboards_count;
extern size_t current_index;
extern bool shift_mode;
extern struct keyboard_struct* current_keyboard;

char* get_keymap(void);
char get_char(uint8_t scancode);
void switch_to_next_keyboard(void);
void hot_key_handler(uint8_t scancode);
void update_hot_key_state(uint8_t scancode);

/* ENGLISH KEYMAPS */
extern const char keymap_english[128];
extern const char keymap_english_shift[128];

#endif