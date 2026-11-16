#ifndef DYNAMIC_KEYMAP_H
#define DYNAMIC_KEYMAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define KEY_ESC         0x01
#define KEY_1           0x02
#define KEY_2           0x03
#define KEY_3           0x04
#define KEY_4           0x05
#define KEY_5           0x06
#define KEY_6           0x07
#define KEY_7           0x08
#define KEY_8           0x09
#define KEY_9           0x0A
#define KEY_0           0x0B
#define KEY_MINUS       0x0C
#define KEY_EQUAL       0x0D
#define KEY_BACKSPACE   0x0E
#define KEY_TAB         0x0F
#define KEY_Q           0x10
#define KEY_W           0x11
#define KEY_E           0x12
#define KEY_R           0x13
#define KEY_T           0x14
#define KEY_Y           0x15
#define KEY_U           0x16
#define KEY_I           0x17
#define KEY_O           0x18
#define KEY_P           0x19
#define KEY_LBRACKET    0x1A
#define KEY_RBRACKET    0x1B
#define KEY_ENTER       0x1C
#define KEY_LCTRL       0x1D
#define KEY_A           0x1E
#define KEY_S           0x1F
#define KEY_D           0x20
#define KEY_F           0x21
#define KEY_G           0x22
#define KEY_H           0x23
#define KEY_J           0x24
#define KEY_K           0x25
#define KEY_L           0x26
#define KEY_SEMICOLON   0x27
#define KEY_QUOTE       0x28
#define KEY_BACKTICK    0x29
#define KEY_GRAVE       0x29
#define KEY_LSHIFT      0x2A
#define KEY_BACKSLASH   0x2B
#define KEY_Z           0x2C
#define KEY_X           0x2D
#define KEY_C           0x2E
#define KEY_V           0x2F
#define KEY_B           0x30
#define KEY_N           0x31
#define KEY_M           0x32
#define KEY_COMMA       0x33
#define KEY_PERIOD      0x34
#define KEY_SLASH       0x35
#define KEY_RSHIFT      0x36
#define KEY_SPACE       0x39

typedef struct {
    uint8_t scancode;
    const char *normal;
    const char *shift;
} keymap_entry_t;

#include <vga.h>

/* Register or replace active dynamic keymap; glyph args are ignored */
int dynamic_keymap_register_with_font(const char *name,
                                      const keymap_entry_t *entries,
                                      uint32_t count,
                                      const dynamic_glyph_def_t *glyphs,
                                      uint32_t glyph_count);

/* Register or replace active keymap; returns 0 or <0 on error */
int dynamic_keymap_set(const char *name, const keymap_entry_t *entries, uint32_t count);

const char* dynamic_keymap_get_name(void);
const char* dynamic_keymap_get_name_local(void);
void dynamic_keymap_reapply_fonts(void);

/* Returns NULL if scancode is not in the active dynamic map */
const char* dynamic_keymap_translate(uint8_t scancode, bool shift);
const char* dynamic_keymap_translate_local(uint8_t scancode, bool shift);

bool dynamic_keymap_is_active(void);
bool dynamic_keymap_is_active_local(void);

void dynamic_keymap_set_default_symbol(const char *symbol);
const char* dynamic_keymap_get_default_symbol(void);

void dynamic_keymap_cycle_next(void);
void dynamic_keymap_cycle_next_local(void);
void dynamic_keymap_cycle_prev(void);
void dynamic_keymap_cycle_prev_local(void);

uint32_t dynamic_keymap_get_count(void);

uint32_t dynamic_keymap_get_active_index(void);

void dynamic_keymap_select(uint32_t index);

/* Enable/disable/remove layout by name or index; return 0 or <0 on error */
int dynamic_keymap_disable(const char *name_or_id);
int dynamic_keymap_enable(const char *name_or_id);
int dynamic_keymap_remove(const char *name_or_id);

uint32_t dynamic_keymap_get_enabled_count(void);

bool dynamic_keymap_is_slot_enabled(uint32_t index);

const char* dynamic_keymap_get_slot_name(uint32_t index);

void dynamic_keymap_reset(void);

void dynamic_keymap_set_app_mode(bool is_app);

extern const char default_keymap_name[];
extern const keymap_entry_t default_keymap_entries[];
extern const uint32_t default_keymap_entries_count;

uint32_t dynamic_keymap_get_entries_count(uint32_t slot_idx);
uint32_t dynamic_keymap_copy_entries(uint32_t slot_idx, keymap_entry_t *out_entries, uint32_t max_count);

#endif /* DYNAMIC_KEYMAP_H */
