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
extern bool key_state[NUM_KEYS];

#define KBD_MOD_NONE   0x00
#define KBD_MOD_CTRL   0x01
#define KBD_MOD_SHIFT  0x02
#define KBD_MOD_ALT    0x04

uint8_t keyboard_get_modifiers(void);
bool keyboard_is_ctrl_pressed(void);
bool keyboard_is_shift_pressed(void);
bool keyboard_is_left_shift_pressed(void);
bool keyboard_is_right_shift_pressed(void);
bool keyboard_is_alt_pressed(void);
void keyboard_clear_key_state(void);
bool keyboard_dispatch_hotkey(uint8_t scancode);

extern bool shift_mode;

#include <driver/input/keymap/dynamic_keymap.h>

char* get_keymap(void);
char get_char(uint8_t scancode);
const char* keyboard_get_key_string(uint8_t scancode);
void hot_key_handler(uint8_t scancode);
void update_hot_key_state(uint8_t scancode);

/* DEFAULT ENGLISH KEYMAP */
extern const char default_keymap_name[];
extern const char keymap_english[128];
extern const char keymap_english_shift[128];

#endif