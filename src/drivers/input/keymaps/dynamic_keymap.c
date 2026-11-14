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

typedef struct {
    char *name;
    keymap_entry_t *entries;
    uint32_t count;
    bool is_custom;
    bool enabled;
} keymap_slot_t;

static keymap_slot_t *keymap_storage = NULL;
static uint32_t keymap_storage_capacity = 0;
static uint32_t keymap_storage_count = 0;
static uint32_t active_storage_index = 0;
static bool storage_initialized = false;

static char *default_symbol_buf = NULL;
static size_t default_symbol_cap = 0;

static void register_slot_fonts(keymap_slot_t *slot) {
    if (slot == NULL || !slot->is_custom || slot->entries == NULL || slot->count == 0) {
        return;
    }
    for (uint32_t i = 0; i < slot->count; i++) {
        if (slot->entries[i].normal != NULL && slot->entries[i].normal[0] != '\0') {
            size_t b = 0;
            utf8_to_vga_glyph(slot->entries[i].normal, strlen(slot->entries[i].normal), &b);
        }
        if (slot->entries[i].shift != NULL && slot->entries[i].shift[0] != '\0') {
            size_t b = 0;
            utf8_to_vga_glyph(slot->entries[i].shift, strlen(slot->entries[i].shift), &b);
        }
    }
}

static bool init_storage_if_needed(void) {
    if (storage_initialized && keymap_storage != NULL) return true;

    if (!keymap_storage) {
        keymap_storage_capacity = 8;
        keymap_storage = (keymap_slot_t *)kmalloc(sizeof(keymap_slot_t) * keymap_storage_capacity);
        if (!keymap_storage) return false;
        memset(keymap_storage, 0, sizeof(keymap_slot_t) * keymap_storage_capacity);
    }

    /* Slot 0 holds the immutable default system layout */
    keymap_storage[0].name = (char *)default_keymap_name;
    keymap_storage[0].entries = (keymap_entry_t *)default_keymap_entries;
    keymap_storage[0].count = default_keymap_entries_count;
    keymap_storage[0].is_custom = false;
    keymap_storage[0].enabled = true;

    keymap_storage_count = 1;
    active_storage_index = 0;
    storage_initialized = true;
    return true;
}

static void free_slot_entries(keymap_slot_t *slot) {
    if (slot->entries != NULL && slot->is_custom) {
        for (uint32_t i = 0; i < slot->count; i++) {
            /* Non-empty strings were heap-allocated; "" is a string literal sentinel */
            if (slot->entries[i].normal && slot->entries[i].normal[0] != '\0') {
                kfree((void *)slot->entries[i].normal);
            }
            if (slot->entries[i].shift && slot->entries[i].shift[0] != '\0') {
                kfree((void *)slot->entries[i].shift);
            }
        }
        kfree(slot->entries);
        slot->entries = NULL;
    }
    slot->count = 0;
}

static const char *dup_keymap_str(const char *s) {
    if (!s || s[0] == '\0') return "";
    size_t len = strlen(s);
    char *copy = (char *)kmalloc(len + 1u);
    if (!copy) return "";
    memcpy(copy, s, len + 1u);
    return copy;
}

int dynamic_keymap_set(const char *name, const keymap_entry_t *entries, uint32_t count) {
    if (!init_storage_if_needed()) return -1;

    if (name == NULL) {
        active_storage_index = 0;
        register_slot_fonts(&keymap_storage[0]);
        terminal_render_language_bar();
        return 0;
    }

    size_t name_len = strlen(name);
    char *new_name = (char *)kmalloc(name_len + 1u);
    if (!new_name) return -1;
    memcpy(new_name, name, name_len + 1u);

    keymap_entry_t *new_entries = NULL;
    uint32_t new_count = 0;
    if (entries != NULL && count > 0) {
        if ((size_t)count > SIZE_MAX / sizeof(keymap_entry_t)) {
            kfree(new_name);
            return -2;
        }
        new_entries = (keymap_entry_t *)kmalloc(sizeof(keymap_entry_t) * count);
        if (!new_entries) {
            kfree(new_name);
            return -2;
        }
        new_count = count;
        for (uint32_t i = 0; i < count; i++) {
            new_entries[i].scancode = entries[i].scancode;
            new_entries[i].normal = dup_keymap_str(entries[i].normal);
            new_entries[i].shift = dup_keymap_str(entries[i].shift);
        }
    }

    int slot = -1;
    for (uint32_t i = 1; i < keymap_storage_count; i++) {
        if (keymap_storage[i].name != NULL && strcmp(keymap_storage[i].name, name) == 0) {
            slot = (int)i;
            break;
        }
    }

    if (slot < 0) {
        if (keymap_storage_count >= keymap_storage_capacity) {
            uint32_t new_cap = (keymap_storage_capacity == 0) ? 8 : (keymap_storage_capacity * 2);
            keymap_slot_t *new_storage = (keymap_slot_t *)kmalloc(sizeof(keymap_slot_t) * new_cap);
            if (!new_storage) {
                keymap_slot_t tmp = { new_name, new_entries, new_count, true, false };
                free_slot_entries(&tmp);
                kfree(new_name);
                return -1;
            }
            if (keymap_storage && keymap_storage_capacity > 0) {
                memcpy(new_storage, keymap_storage, sizeof(keymap_slot_t) * keymap_storage_capacity);
                kfree(keymap_storage);
            }
            memset(new_storage + keymap_storage_capacity, 0, sizeof(keymap_slot_t) * (new_cap - keymap_storage_capacity));
            keymap_storage = new_storage;
            keymap_storage_capacity = new_cap;
        }
        slot = (int)keymap_storage_count++;
    } else {
        free_slot_entries(&keymap_storage[slot]);
        if (keymap_storage[slot].name != NULL) {
            kfree(keymap_storage[slot].name);
            keymap_storage[slot].name = NULL;
        }
    }

    keymap_storage[slot].name = new_name;
    keymap_storage[slot].entries = new_entries;
    keymap_storage[slot].count = new_count;
    keymap_storage[slot].is_custom = true;
    keymap_storage[slot].enabled = true;
    active_storage_index = (uint32_t)slot;
    register_slot_fonts(&keymap_storage[slot]);
    terminal_render_language_bar();
    return 0;
}

void dynamic_keymap_reapply_fonts(void) {
    if (!init_storage_if_needed()) return;
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
    if (!init_storage_if_needed() || active_storage_index >= keymap_storage_count) {
        return default_keymap_name;
    }
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
    if (!init_storage_if_needed() || active_storage_index >= keymap_storage_count) {
        return NULL;
    }

    keymap_slot_t *active = &keymap_storage[active_storage_index];
    if (!active->enabled || active->entries == NULL || active->count == 0) {
        return NULL;
    }

    for (uint32_t i = 0; i < active->count; i++) {
        if (active->entries[i].scancode == scancode) {
            const char *str = shift ? active->entries[i].shift : active->entries[i].normal;
            if (shift && str == NULL) {
                str = active->entries[i].normal;
            }

            if (str == NULL || str[0] == '\0' ||
                (scancode != KEY_SPACE && strcmp(str, " ") == 0) ||
                strcmp(str, "__DISABLED__") == 0 ||
                strcmp(str, "none") == 0) {
                return "";
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
    if (!init_storage_if_needed() || active_storage_index >= keymap_storage_count) {
        return false;
    }
    return (keymap_storage[active_storage_index].is_custom && keymap_storage[active_storage_index].enabled);
}

bool dynamic_keymap_is_active(void) {
    if (should_use_keymap_syscalls()) {
        return (bool)ipo_syscall(IPO_SYSCALL_KEYMAP_IS_ACTIVE, 0u, NULL);
    }
    return dynamic_keymap_is_active_local();
}

static void apply_active_font(void) {
    if (keymap_storage && active_storage_index < keymap_storage_count) {
        register_slot_fonts(&keymap_storage[active_storage_index]);
    }
}

static inline char ascii_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static int find_slot_by_name_or_id(const char *name_or_id) {
    if (name_or_id == NULL || name_or_id[0] == '\0') return -1;
    if (!init_storage_if_needed()) return -1;

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
            uint32_t digit = (uint32_t)(name_or_id[i] - '0');
            if (val > (UINT32_MAX - digit) / 10u) return -1;
            val = val * 10u + digit;
        }
        if (val < keymap_storage_count) return (int)val;
        return -1;
    }

    for (uint32_t i = 0; i < keymap_storage_count; i++) {
        if (!keymap_storage[i].name) continue;
        if (strcmp(keymap_storage[i].name, name_or_id) == 0) return (int)i;

        const char *haystack = keymap_storage[i].name;
        const char *needle = name_or_id;
        size_t nlen = strlen(needle);
        size_t hlen = strlen(haystack);
        if (nlen > 0 && nlen <= hlen) {
            for (size_t h = 0; h <= hlen - nlen; h++) {
                size_t k = 0;
                while (k < nlen && ascii_tolower(haystack[h + k]) == ascii_tolower(needle[k])) {
                    k++;
                }
                if (k == nlen) return (int)i;
            }
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
    if (!init_storage_if_needed()) return 0;
    uint32_t enabled_count = 0;
    for (uint32_t i = 0; i < keymap_storage_count; i++) {
        if (keymap_storage[i].enabled) enabled_count++;
    }
    return enabled_count;
}

bool dynamic_keymap_is_slot_enabled(uint32_t index) {
    if (!init_storage_if_needed()) return false;
    if (index < keymap_storage_count) return keymap_storage[index].enabled;
    return false;
}

const char* dynamic_keymap_get_slot_name(uint32_t index) {
    if (!init_storage_if_needed()) return "Unknown";
    if (index < keymap_storage_count && keymap_storage[index].name != NULL) {
        return keymap_storage[index].name;
    }
    return "Unknown";
}

static void dynamic_keymap_cycle_dir(bool forward) {
    if (!init_storage_if_needed() || dynamic_keymap_get_enabled_count() <= 1) {
        return;
    }

    for (uint32_t step = 1; step <= keymap_storage_count; step++) {
        uint32_t idx = forward
            ? ((active_storage_index + step) % keymap_storage_count)
            : ((active_storage_index + keymap_storage_count - (step % keymap_storage_count)) % keymap_storage_count);
        if (keymap_storage[idx].enabled) {
            active_storage_index = idx;
            apply_active_font();
            terminal_render_language_bar();
            return;
        }
    }
}

void dynamic_keymap_cycle_next_local(void) {
    dynamic_keymap_cycle_dir(true);
}

void dynamic_keymap_cycle_next(void) {
    if (should_use_keymap_syscalls()) {
        ipo_syscall(IPO_SYSCALL_KEYMAP_CYCLE_NEXT, 0u, NULL);
        return;
    }
    dynamic_keymap_cycle_next_local();
}

void dynamic_keymap_cycle_prev_local(void) {
    dynamic_keymap_cycle_dir(false);
}

void dynamic_keymap_cycle_prev(void) {
    if (should_use_keymap_syscalls()) {
        ipo_syscall(IPO_SYSCALL_KEYMAP_CYCLE_PREV, 0u, NULL);
        return;
    }
    dynamic_keymap_cycle_prev_local();
}

uint32_t dynamic_keymap_get_count(void) {
    if (!init_storage_if_needed()) return 0;
    return keymap_storage_count;
}

uint32_t dynamic_keymap_get_active_index(void) {
    if (!init_storage_if_needed()) return 0;
    return active_storage_index;
}

void dynamic_keymap_select(uint32_t index) {
    if (!init_storage_if_needed()) return;
    if (index < keymap_storage_count) {
        active_storage_index = index;
        apply_active_font();
        terminal_render_language_bar();
    }
}

void dynamic_keymap_set_default_symbol(const char *symbol) {
    if (symbol != NULL) {
        size_t len = strlen(symbol);
        if (len + 1u > default_symbol_cap) {
            size_t new_cap = len + 1u;
            char *nbuf = (char *)krealloc(default_symbol_buf, new_cap);
            if (!nbuf) return;
            default_symbol_buf = nbuf;
            default_symbol_cap = new_cap;
        }
        memcpy(default_symbol_buf, symbol, len + 1u);
    } else if (default_symbol_buf) {
        default_symbol_buf[0] = '\0';
    }
}

const char* dynamic_keymap_get_default_symbol(void) {
    return default_symbol_buf ? default_symbol_buf : "";
}

void dynamic_keymap_reset(void) {
    if (!init_storage_if_needed()) return;
    active_storage_index = 0;
    apply_active_font();
    terminal_render_language_bar();
}

uint32_t dynamic_keymap_get_entries_count(uint32_t slot_idx) {
    if (!init_storage_if_needed()) return 0;
    if (slot_idx < keymap_storage_count && keymap_storage[slot_idx].entries != NULL) {
        return keymap_storage[slot_idx].count;
    }
    return 0;
}

uint32_t dynamic_keymap_copy_entries(uint32_t slot_idx, keymap_entry_t *out_entries, uint32_t max_count) {
    if (!init_storage_if_needed()) return 0;
    if (slot_idx >= keymap_storage_count || !keymap_storage[slot_idx].entries || !out_entries || max_count == 0) {
        return 0;
    }
    uint32_t count = keymap_storage[slot_idx].count;
    if (count > max_count) {
        count = max_count;
    }
    memcpy(out_entries, keymap_storage[slot_idx].entries, sizeof(keymap_entry_t) * count);
    return count;
}
