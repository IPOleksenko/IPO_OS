/**
 * Dynamic Keymap & Layout Repository
 *
 * Stores registered keyboard layouts and allows cycling between them
 * (e.g. English -> Russian -> Chinese -> English) via Ctrl + Shift.
 */

#include <driver/input/keymap/dynamic_keymap.h>
#include <driver/input/keymap/keymap.h>
#include <kernel/terminal.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <syscall.h>
#include <vga.h>

static bool dynamic_keymap_app_mode = false;

void dynamic_keymap_set_app_mode(bool is_app) {
    dynamic_keymap_app_mode = is_app;
}

static inline bool should_use_keymap_syscalls(void) {
    return dynamic_keymap_app_mode;
}

#define MAX_KEYMAPS 8

typedef struct {
    char *name;
    keymap_entry_t *entries;
    uint32_t count;
    bool is_custom;
    bool enabled;
} keymap_slot_t;

static keymap_slot_t keymap_storage[MAX_KEYMAPS];
static uint32_t keymap_storage_count = 0;
static uint32_t active_storage_index = 0;
static bool storage_initialized = false;

static char default_symbol_buf[16] = DEFAULT_OS_KEY_SYMBOL;

static void register_slot_fonts(keymap_slot_t *slot) {
    if (slot == NULL || !slot->is_custom || slot->entries == NULL || slot->count == 0) {
        return;
    }
    for (uint32_t i = 0; i < slot->count; i++) {
        if (slot->entries[i].normal[0] != '\0') {
            size_t b = 0;
            utf8_to_vga_glyph(slot->entries[i].normal, strlen(slot->entries[i].normal), &b);
        }
        if (slot->entries[i].shift[0] != '\0') {
            size_t b = 0;
            utf8_to_vga_glyph(slot->entries[i].shift, strlen(slot->entries[i].shift), &b);
        }
    }
}

static void init_storage_if_needed(void) {
    if (storage_initialized) return;

    /* Slot 0 is always default English */
    keymap_storage[0].name = (char *)default_keymap_name;
    keymap_storage[0].entries = NULL;
    keymap_storage[0].count = 0;
    keymap_storage[0].is_custom = false;
    keymap_storage[0].enabled = true;

    keymap_storage_count = 1;
    active_storage_index = 0;
    storage_initialized = true;
}

static void free_slot_entries(keymap_slot_t *slot) {
    if (slot->entries != NULL) {
        kfree(slot->entries);
        slot->entries = NULL;
    }
    slot->count = 0;
}

int dynamic_keymap_set(const char *name, const keymap_entry_t *entries, uint32_t count) {
    init_storage_if_needed();

    if (name == NULL) {
        /* Reset to English (Slot 0) */
        active_storage_index = 0;
        register_slot_fonts(&keymap_storage[0]);
        terminal_render_language_bar();
        return 0;
    }

    /* Find existing slot with this name, or create a new slot */
    int slot = -1;
    for (uint32_t i = 1; i < keymap_storage_count; i++) {
        if (keymap_storage[i].name != NULL && strcmp(keymap_storage[i].name, name) == 0) {
            slot = (int)i;
            break;
        }
    }

    if (slot < 0) {
        if (keymap_storage_count < MAX_KEYMAPS) {
            slot = (int)keymap_storage_count++;
        } else {
            /* Storage full: replace last slot */
            slot = MAX_KEYMAPS - 1;
            free_slot_entries(&keymap_storage[slot]);
            if (keymap_storage[slot].name != NULL) {
                kfree(keymap_storage[slot].name);
                keymap_storage[slot].name = NULL;
            }
        }
    } else {
        free_slot_entries(&keymap_storage[slot]);
        if (keymap_storage[slot].name != NULL) {
            kfree(keymap_storage[slot].name);
            keymap_storage[slot].name = NULL;
        }
    }

    /* Allocate and copy name */
    size_t name_len = strlen(name);
    keymap_storage[slot].name = (char *)kmalloc(name_len + 1u);
    if (keymap_storage[slot].name == NULL) {
        return -1;
    }
    memcpy(keymap_storage[slot].name, name, name_len + 1u);

    /* Allocate and copy entries */
    if (entries != NULL && count > 0) {
        keymap_storage[slot].entries = (keymap_entry_t *)kmalloc(sizeof(keymap_entry_t) * count);
        if (keymap_storage[slot].entries == NULL) {
            kfree(keymap_storage[slot].name);
            keymap_storage[slot].name = NULL;
            return -2;
        }

        keymap_storage[slot].count = count;
        memcpy(keymap_storage[slot].entries, entries, sizeof(keymap_entry_t) * count);
    } else {
        keymap_storage[slot].entries = NULL;
        keymap_storage[slot].count = 0;
    }

    keymap_storage[slot].is_custom = true;
    keymap_storage[slot].enabled = true;
    active_storage_index = (uint32_t)slot;
    register_slot_fonts(&keymap_storage[slot]);
    terminal_render_language_bar();
    return 0;
}

void dynamic_keymap_reapply_fonts(void) {
    init_storage_if_needed();
    vga_font_reapply_active();
    if (active_storage_index < keymap_storage_count) {
        register_slot_fonts(&keymap_storage[active_storage_index]);
    }
    terminal_render_language_bar();
}

int dynamic_keymap_register_with_font(const char *name,
                                      const keymap_entry_t *entries,
                                      uint32_t count,
                                      const dynamic_glyph_def_t *glyphs,
                                      uint32_t glyph_count) {
    (void)glyphs; (void)glyph_count;
    return dynamic_keymap_set(name, entries, count);
}

const char* dynamic_keymap_get_name_local(void) {
    init_storage_if_needed();
    if (keymap_storage[active_storage_index].name != NULL) {
        return keymap_storage[active_storage_index].name;
    }
    return default_keymap_name;
}

const char* dynamic_keymap_get_name(void) {
    if (should_use_keymap_syscalls()) {
        const char *name = (const char *)(uintptr_t)ipo_syscall(IPO_SYSCALL_KEYMAP_GET_NAME, 0u, NULL);
        if (name != NULL) return name;
    }
    return dynamic_keymap_get_name_local();
}

const char* dynamic_keymap_translate_local(uint8_t scancode, bool shift) {
    init_storage_if_needed();

    keymap_slot_t *active = &keymap_storage[active_storage_index];
    if (!active->enabled || !active->is_custom || active->entries == NULL || active->count == 0) {
        return NULL;
    }

    for (uint32_t i = 0; i < active->count; i++) {
        if (active->entries[i].scancode == scancode) {
            const char *str = shift ? active->entries[i].shift : active->entries[i].normal;
            if (shift && str == NULL) {
                str = active->entries[i].normal;
            }

            /* Disabled key sentinel check:
             * - NULL
             * - Empty string ""
             * - " " on any key other than KEY_SPACE
             * - "__DISABLED__" or "none"
             */
            if (str == NULL || str[0] == '\0' ||
                (scancode != KEY_SPACE && strcmp(str, " ") == 0) ||
                strcmp(str, "__DISABLED__") == 0 ||
                strcmp(str, "none") == 0) {
                return ""; /* Explicitly disabled */
            }

            return str;
        }
    }

    return NULL;
}

const char* dynamic_keymap_translate(uint8_t scancode, bool shift) {
    if (should_use_keymap_syscalls()) {
        uint32_t args[2];
        args[0] = (uint32_t)scancode;
        args[1] = (uint32_t)shift;
        return (const char *)(uintptr_t)ipo_syscall(IPO_SYSCALL_KEYMAP_TRANSLATE, 2u, args);
    }
    return dynamic_keymap_translate_local(scancode, shift);
}

bool dynamic_keymap_is_active_local(void) {
    init_storage_if_needed();
    return (keymap_storage[active_storage_index].is_custom && keymap_storage[active_storage_index].enabled);
}

bool dynamic_keymap_is_active(void) {
    if (should_use_keymap_syscalls()) {
        return (bool)ipo_syscall(IPO_SYSCALL_KEYMAP_IS_ACTIVE, 0u, NULL);
    }
    return dynamic_keymap_is_active_local();
}

static void apply_active_font(void) {
    keymap_slot_t *slot = &keymap_storage[active_storage_index];
    register_slot_fonts(slot);
}

static int find_slot_by_name_or_id(const char *name_or_id) {
    if (name_or_id == NULL || name_or_id[0] == '\0') return -1;
    init_storage_if_needed();

    /* 1. Check if numeric index */
    bool is_num = true;
    for (size_t i = 0; name_or_id[i] != '\0'; i++) {
        if (name_or_id[i] < '0' || name_or_id[i] > '9') {
            is_num = false;
            break;
        }
    }
    if (is_num) {
        uint32_t val = 0;
        for (size_t i = 0; name_or_id[i] >= '0' && name_or_id[i] <= '9'; i++) {
            val = val * 10u + (uint32_t)(name_or_id[i] - '0');
        }
        if (val < keymap_storage_count) return (int)val;
        return -1;
    }

    /* 2. Direct name or substring match */
    for (uint32_t i = 0; i < keymap_storage_count; i++) {
        if (keymap_storage[i].name != NULL) {
            if (strcmp(keymap_storage[i].name, name_or_id) == 0) return (int)i;
            if ((strcmp(name_or_id, "ru") == 0 || strcmp(name_or_id, "russian") == 0) &&
                strstr(keymap_storage[i].name, "Russian") != NULL) return (int)i;
            if ((strcmp(name_or_id, "ua") == 0 || strcmp(name_or_id, "ukrainian") == 0) &&
                strstr(keymap_storage[i].name, "Ukrainian") != NULL) return (int)i;
            if ((strcmp(name_or_id, "zh") == 0 || strcmp(name_or_id, "chinese") == 0) &&
                strstr(keymap_storage[i].name, "Chinese") != NULL) return (int)i;
            if ((strcmp(name_or_id, "en") == 0 || strcmp(name_or_id, "english") == 0) &&
                strstr(keymap_storage[i].name, "English") != NULL) return (int)i;
        }
    }
    return -1;
}

int dynamic_keymap_disable(const char *name_or_id) {
    int slot = find_slot_by_name_or_id(name_or_id);
    if (slot < 0 || (uint32_t)slot >= keymap_storage_count) return -1;

    keymap_storage[slot].enabled = false;

    /* If currently active layout was disabled, switch to first available enabled layout */
    if (active_storage_index == (uint32_t)slot) {
        for (uint32_t i = 0; i < keymap_storage_count; i++) {
            if (keymap_storage[i].enabled) {
                active_storage_index = i;
                apply_active_font();
                break;
            }
        }
    }

    terminal_render_language_bar();
    return 0;
}

int dynamic_keymap_enable(const char *name_or_id) {
    int slot = find_slot_by_name_or_id(name_or_id);
    if (slot < 0 || (uint32_t)slot >= keymap_storage_count) return -1;

    keymap_storage[slot].enabled = true;
    terminal_render_language_bar();
    return 0;
}

int dynamic_keymap_remove(const char *name_or_id) {
    int slot = find_slot_by_name_or_id(name_or_id);
    if (slot <= 0 || (uint32_t)slot >= keymap_storage_count) return -1;

    free_slot_entries(&keymap_storage[slot]);
    if (keymap_storage[slot].name != NULL) {
        kfree(keymap_storage[slot].name);
        keymap_storage[slot].name = NULL;
    }

    for (uint32_t j = (uint32_t)slot; j < keymap_storage_count - 1u; j++) {
        keymap_storage[j] = keymap_storage[j + 1u];
    }
    keymap_storage_count--;

    if (active_storage_index == (uint32_t)slot) {
        active_storage_index = 0;
        apply_active_font();
    } else if (active_storage_index > (uint32_t)slot) {
        active_storage_index--;
    }

    terminal_render_language_bar();
    return 0;
}

uint32_t dynamic_keymap_get_enabled_count(void) {
    init_storage_if_needed();
    uint32_t enabled_count = 0;
    for (uint32_t i = 0; i < keymap_storage_count; i++) {
        if (keymap_storage[i].enabled) enabled_count++;
    }
    return enabled_count;
}

bool dynamic_keymap_is_slot_enabled(uint32_t index) {
    init_storage_if_needed();
    if (index < keymap_storage_count) return keymap_storage[index].enabled;
    return false;
}

const char* dynamic_keymap_get_slot_name(uint32_t index) {
    init_storage_if_needed();
    if (index < keymap_storage_count && keymap_storage[index].name != NULL) {
        return keymap_storage[index].name;
    }
    return "Unknown";
}

void dynamic_keymap_cycle_next_local(void) {
    init_storage_if_needed();
    if (dynamic_keymap_get_enabled_count() <= 1) {
        return;
    }

    for (uint32_t step = 1; step <= keymap_storage_count; step++) {
        uint32_t idx = (active_storage_index + step) % keymap_storage_count;
        if (keymap_storage[idx].enabled) {
            active_storage_index = idx;
            apply_active_font();
            terminal_render_language_bar();
            return;
        }
    }
}

void dynamic_keymap_cycle_next(void) {
    if (should_use_keymap_syscalls()) {
        ipo_syscall(IPO_SYSCALL_KEYMAP_CYCLE_NEXT, 0u, NULL);
        return;
    }
    dynamic_keymap_cycle_next_local();
}

void dynamic_keymap_cycle_prev_local(void) {
    init_storage_if_needed();
    if (dynamic_keymap_get_enabled_count() <= 1) {
        return;
    }

    for (uint32_t step = 1; step <= keymap_storage_count; step++) {
        uint32_t idx = (active_storage_index + keymap_storage_count - (step % keymap_storage_count)) % keymap_storage_count;
        if (keymap_storage[idx].enabled) {
            active_storage_index = idx;
            apply_active_font();
            terminal_render_language_bar();
            return;
        }
    }
}

void dynamic_keymap_cycle_prev(void) {
    if (should_use_keymap_syscalls()) {
        ipo_syscall(IPO_SYSCALL_KEYMAP_CYCLE_PREV, 0u, NULL);
        return;
    }
    dynamic_keymap_cycle_prev_local();
}

uint32_t dynamic_keymap_get_count(void) {
    init_storage_if_needed();
    return keymap_storage_count;
}

uint32_t dynamic_keymap_get_active_index(void) {
    init_storage_if_needed();
    return active_storage_index;
}

void dynamic_keymap_select(uint32_t index) {
    init_storage_if_needed();
    if (index < keymap_storage_count) {
        active_storage_index = index;
        apply_active_font();
        terminal_render_language_bar();
    }
}

void dynamic_keymap_set_default_symbol(const char *symbol) {
    if (symbol != NULL) {
        strncpy(default_symbol_buf, symbol, sizeof(default_symbol_buf) - 1u);
        default_symbol_buf[sizeof(default_symbol_buf) - 1u] = '\0';
    } else {
        strcpy(default_symbol_buf, DEFAULT_OS_KEY_SYMBOL);
    }
}

const char* dynamic_keymap_get_default_symbol(void) {
    return default_symbol_buf;
}

void dynamic_keymap_reset(void) {
    init_storage_if_needed();
    active_storage_index = 0; /* Return to English */
    apply_active_font();
    terminal_render_language_bar();
}
