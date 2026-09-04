#ifndef DYNAMIC_KEYMAP_H
#define DYNAMIC_KEYMAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Standard scancode definitions */
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

/**
 * Single dictionary entry:
 * Maps a hardware scancode to output strings (arbitrary length: char, UTF-8, word, emoji).
 */
typedef struct {
    uint8_t scancode;          /* Key scancode */
    char normal[8];            /* Normal output string (supports UTF-8 / multi-byte) */
    char shift[8];             /* Shift output string (supports UTF-8 / multi-byte) */
} keymap_entry_t;

#include <vga.h>

/**
 * Register or replace active dynamic keymap with optional dynamic font glyphs.
 */
int dynamic_keymap_register_with_font(const char *name,
                                      const keymap_entry_t *entries,
                                      uint32_t count,
                                      const dynamic_glyph_def_t *glyphs,
                                      uint32_t glyph_count);

/**
 * Register or replace active dynamic keymap (backwards-compatible wrapper).
 * @param name Language name (any length, will be dynamically allocated)
 * @param entries Array of keymap entries
 * @param count Number of entries
 * @return 0 on success, negative error code on failure
 */
int dynamic_keymap_set(const char *name, const keymap_entry_t *entries, uint32_t count);

/**
 * Get current active language name.
 */
const char* dynamic_keymap_get_name(void);
void dynamic_keymap_reapply_fonts(void);

/**
 * Translate a scancode using the dynamic keymap.
 * @param scancode Hardware scancode
 * @param shift Whether Shift modifier is active
 * @return Output string, or NULL if not mapped in dynamic dictionary
 */
const char* dynamic_keymap_translate(uint8_t scancode, bool shift);

/**
 * Returns true if a custom dynamic keymap is currently active.
 */
bool dynamic_keymap_is_active(void);

/**
 * Default fallback symbol for unmapped keys when a custom keymap is active.
 */
#define DEFAULT_OS_KEY_SYMBOL "?"

void dynamic_keymap_set_default_symbol(const char *symbol);
const char* dynamic_keymap_get_default_symbol(void);

/**
 * Cycle to next layout in the storage (English -> Russian -> Chinese -> ...).
 */
void dynamic_keymap_cycle_next(void);
void dynamic_keymap_cycle_prev(void);

/**
 * Get total number of registered keymaps.
 */
uint32_t dynamic_keymap_get_count(void);

/**
 * Get currently active keymap index.
 */
uint32_t dynamic_keymap_get_active_index(void);

/**
 * Select a specific keymap by index.
 */
void dynamic_keymap_select(uint32_t index);

/**
 * Disable a keyboard layout by name or index.
 * @return 0 on success, negative on failure
 */
int dynamic_keymap_disable(const char *name_or_id);

/**
 * Enable a keyboard layout by name or index.
 * @return 0 on success, negative on failure
 */
int dynamic_keymap_enable(const char *name_or_id);

/**
 * Remove a keyboard layout completely.
 * @return 0 on success, negative on failure
 */
int dynamic_keymap_remove(const char *name_or_id);

/**
 * Get count of currently enabled keymaps.
 */
uint32_t dynamic_keymap_get_enabled_count(void);

/**
 * Check if a keymap at index is enabled.
 */
bool dynamic_keymap_is_slot_enabled(uint32_t index);

/**
 * Get keymap name at specific index.
 */
const char* dynamic_keymap_get_slot_name(uint32_t index);

/**
 * Reset back to default system keymap (English QWERTY).
 */
void dynamic_keymap_reset(void);

/**
 * Configure dynamic keymap app mode.
 */
void dynamic_keymap_set_app_mode(bool is_app);

#endif /* DYNAMIC_KEYMAP_H */
