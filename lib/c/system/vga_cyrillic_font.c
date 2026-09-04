#include <vga.h>
#include <ioport.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <memory/kmalloc.h>
#include <file_system/ipo_fs.h>
#include <syscall.h>

static uint8_t vga_font_cache[256][16];
static bool vga_font_cache_initialized = false;

void vga_init_font_cache(void) {
    /* Pre-fill cache with authentic default IBM VGA 8x16 font */
    for (int i = 0; i < 256; i++) {
        for (int r = 0; r < 16; r++) {
            vga_font_cache[i][r] = vga_default_font_data[i][r];
        }
    }

    /* Try to read active hardware font directly from VGA Plane 2 */
    outb(0x3C4, 0x00); outb(0x3C5, 0x01); // Synchronous reset
    outb(0x3C4, 0x02); outb(0x3C5, 0x04); // Write only to Plane 2
    outb(0x3C4, 0x04); outb(0x3C5, 0x07); // Sequential addressing
    outb(0x3C4, 0x00); outb(0x3C5, 0x03); // Clear reset

    outb(0x3CE, 0x04); outb(0x3CF, 0x02); // Read map = Plane 2
    outb(0x3CE, 0x05); outb(0x3CF, 0x00); // Write mode 0
    outb(0x3CE, 0x06); outb(0x3CF, 0x00); // Map memory to 0xA0000

    /* Check if Plane 2 font memory is accessible by testing 'A' (0x41) */
    volatile uint8_t *test_ptr = (volatile uint8_t *)(0xA0000 + ((uint32_t)'A' * 32u));
    bool readable = false;
    for (int r = 0; r < 16; r++) {
        if (test_ptr[r] != 0) {
            readable = true;
            break;
        }
    }

    if (readable) {
        for (uint32_t c = 0; c < 256; c++) {
            volatile uint8_t *src = (volatile uint8_t *)(0xA0000 + (c * 32u));
            for (int r = 0; r < 16; r++) {
                vga_font_cache[c][r] = src[r];
            }
        }
    }

    /* Restore normal text mode operation (Plane 0 & 1, 0xB8000) */
    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x03);
    outb(0x3C4, 0x04); outb(0x3C5, 0x03);
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);

    outb(0x3CE, 0x04); outb(0x3CF, 0x00);
    outb(0x3CE, 0x05); outb(0x3CF, 0x10);
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E);

    vga_font_cache_initialized = true;
}

const uint8_t* vga_font_get_cached_glyph(uint8_t char_code) {
    if (!vga_font_cache_initialized) {
        vga_init_font_cache();
    }
    return vga_font_cache[char_code];
}

/**
 * Standard VGA Plane 2 font loader.
 * Loads 8x16 glyphs (16 bytes per glyph) into VGA font memory at 0xA0000.
 */
void vga_load_font(uint8_t start_code, uint32_t count, const uint8_t *glyphs) {
    if (glyphs == NULL || count == 0) return;

    /* Cache glyph bitmaps in RAM for cursor composite rendering */
    for (uint32_t c = 0; c < count; c++) {
        uint8_t code = (uint8_t)(start_code + c);
        if (code != VGA_CURSOR_GLYPH_SLOT) {
            const uint8_t *g = glyphs + (c * 16u);
            for (int r = 0; r < 16; r++) {
                vga_font_cache[code][r] = g[r];
            }
        }
    }

    /* Set Sequencer registers for Plane 2 write */
    outb(0x3C4, 0x00); outb(0x3C5, 0x01); // Synchronous reset
    outb(0x3C4, 0x02); outb(0x3C5, 0x04); // Write only to Plane 2
    outb(0x3C4, 0x04); outb(0x3C5, 0x07); // Sequential addressing
    outb(0x3C4, 0x00); outb(0x3C5, 0x03); // Clear reset

    /* Set Graphics Controller registers */
    outb(0x3CE, 0x04); outb(0x3CF, 0x02); // Read map = Plane 2
    outb(0x3CE, 0x05); outb(0x3CF, 0x00); // Write mode 0
    outb(0x3CE, 0x06); outb(0x3CF, 0x00); // Map memory to 0xA0000

    /* Write glyphs to font RAM (32 bytes per character slot) */
    for (uint32_t c = 0; c < count; c++) {
        uint8_t char_code = (uint8_t)(start_code + c);
        volatile uint8_t *dest = (volatile uint8_t *)(0xA0000 + ((uint32_t)char_code * 32u));
        const uint8_t *src = glyphs + (c * 16u);
        for (int row = 0; row < 16; row++) {
            dest[row] = src[row];
        }
    }

    /* Restore normal text mode operation (Plane 0 & 1, 0xB8000) */
    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x03); // Enable planes 0 & 1
    outb(0x3C4, 0x04); outb(0x3C5, 0x03); // Odd/even addressing
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);

    outb(0x3CE, 0x04); outb(0x3CF, 0x00);
    outb(0x3CE, 0x05); outb(0x3CF, 0x10); // Normal host mode
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E); // Map memory back to 0xB8000
}

/* =========================================================================
 * UNIVERSAL DISK-BASED FONT DATABASE (No hardcoded arrays, no switch-cases)
 * ========================================================================= */

typedef struct {
    uint32_t codepoint;
    uint8_t  bitmap[16];
} __attribute__((packed)) font_record_t;

static font_record_t *font_db = NULL;
static uint32_t font_db_count = 0;
static char current_font_name[64] = "default";
static char current_font_path[128] = "/fonts/default.fnt";

static bool font_app_mode = false;

void vga_font_set_app_mode(bool is_app) {
    font_app_mode = is_app;
}

int vga_font_load_database(const char *path) {
    if (path == NULL) {
        return -1;
    }

    if (font_app_mode) {
        /* Userland process: use filesystem syscalls */
        int fd = ipo_open(path);
        if (fd < 0) return -1;

        struct ipo_inode st;
        if (ipo_stat(path, &st) < 0 || st.size < 8) {
            ipo_close(fd);
            return -2;
        }

        char magic[4];
        uint32_t count = 0;
        if (ipo_read(fd, magic, 4, 0) != 4 ||
            ipo_read(fd, &count, 4, 4) != 4) {
            ipo_close(fd);
            return -3;
        }

        if (memcmp(magic, "IFNT", 4) != 0 || count == 0) {
            ipo_close(fd);
            return -4;
        }

        uint32_t data_size = count * sizeof(font_record_t);
        if (font_db != NULL) {
            kfree(font_db);
            font_db = NULL;
            font_db_count = 0;
        }

        font_db = (font_record_t *)kmalloc(data_size);
        if (font_db == NULL) {
            ipo_close(fd);
            return -5;
        }

        int bytes_read = ipo_read(fd, font_db, data_size, 8);
        ipo_close(fd);
        if (bytes_read < (int)data_size) {
            kfree(font_db);
            font_db = NULL;
            return -6;
        }

        font_db_count = count;
        strncpy(current_font_path, path, sizeof(current_font_path) - 1);
        current_font_path[sizeof(current_font_path) - 1] = '\0';

        /* Derive font name from filename */
        const char *base = strrchr(path, '/');
        const char *name_start = base ? base + 1 : path;
        strncpy(current_font_name, name_start, sizeof(current_font_name) - 1);
        current_font_name[sizeof(current_font_name) - 1] = '\0';
        char *dot = strrchr(current_font_name, '.');
        if (dot) *dot = '\0';

        /* Upload base ASCII/extended glyphs to hardware VGA plane 2 */
        for (uint32_t i = 0; i < font_db_count; i++) {
            if (font_db[i].codepoint < 256) {
                vga_load_font((uint8_t)font_db[i].codepoint, 1, font_db[i].bitmap);
            }
        }
        vga_font_reset_registry();
        return 0;
    } else {
        /* Kernel mode: use direct kernel filesystem, only if mounted */
        if (!fs_mounted) {
            return -1;
        }

        int fd = ipo_fs_open(path);
        if (fd < 0) return -1;

        struct ipo_inode st;
        if (!ipo_fs_stat(path, &st) || st.size < 8) {
            ipo_fs_close(fd);
            return -2;
        }

        char magic[4];
        uint32_t count = 0;
        if (ipo_fs_read(fd, magic, 4, 0) != 4 ||
            ipo_fs_read(fd, &count, 4, 4) != 4) {
            ipo_fs_close(fd);
            return -3;
        }

        if (memcmp(magic, "IFNT", 4) != 0 || count == 0) {
            ipo_fs_close(fd);
            return -4;
        }

        uint32_t data_size = count * sizeof(font_record_t);
        if (font_db != NULL) {
            kfree(font_db);
            font_db = NULL;
            font_db_count = 0;
        }

        font_db = (font_record_t *)kmalloc(data_size);
        if (font_db == NULL) {
            ipo_fs_close(fd);
            return -5;
        }

        int bytes_read = ipo_fs_read(fd, font_db, data_size, 8);
        ipo_fs_close(fd);
        if (bytes_read < (int)data_size) {
            kfree(font_db);
            font_db = NULL;
            return -6;
        }

        font_db_count = count;
        strncpy(current_font_path, path, sizeof(current_font_path) - 1);
        current_font_path[sizeof(current_font_path) - 1] = '\0';

        const char *base = strrchr(path, '/');
        const char *name_start = base ? base + 1 : path;
        strncpy(current_font_name, name_start, sizeof(current_font_name) - 1);
        current_font_name[sizeof(current_font_name) - 1] = '\0';
        char *dot = strrchr(current_font_name, '.');
        if (dot) *dot = '\0';

        /* Upload base ASCII/extended glyphs to hardware VGA plane 2 */
        for (uint32_t i = 0; i < font_db_count; i++) {
            if (font_db[i].codepoint < 256) {
                vga_load_font((uint8_t)font_db[i].codepoint, 1, font_db[i].bitmap);
            }
        }
        vga_font_reset_registry();
        return 0;
    }
}

const char* vga_font_get_current_name(void) {
    return current_font_name;
}

const char* vga_font_get_current_path(void) {
    return current_font_path;
}

uint32_t vga_font_get_glyph_count(void) {
    if (font_app_mode) {
        uint32_t count = 0;
        ipo_font_get_info(NULL, 0, &count);
        return count;
    }
    return font_db_count;
}

int vga_font_get_info(char *name_buf, size_t name_size, uint32_t *out_count) {
    if (name_buf && name_size > 0) {
        strncpy(name_buf, current_font_name, name_size - 1);
        name_buf[name_size - 1] = '\0';
    }
    if (out_count) {
        *out_count = font_db_count;
    }
    return 0;
}

static const uint8_t* vga_font_lookup_in_db(uint32_t codepoint) {
    if (font_db == NULL || font_db_count == 0) return NULL;
    int left = 0, right = (int)font_db_count - 1;
    while (left <= right) {
        int mid = left + (right - left) / 2;
        if (font_db[mid].codepoint == codepoint) {
            return font_db[mid].bitmap;
        }
        if (font_db[mid].codepoint < codepoint) {
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    return NULL;
}

/* Custom user-registered glyphs stored dynamically in heap (unlimited / infinite) */
typedef struct custom_glyph_node {
    uint32_t codepoint;
    uint8_t  bitmap[16];
    struct custom_glyph_node *next;
} custom_glyph_node_t;

static custom_glyph_node_t *custom_glyph_head = NULL;

/* Dynamic VGA Plane 2 slot recycling table (LRU) - allocated dynamically in heap */
typedef struct {
    uint8_t  slot_id;
    uint32_t codepoint;
    uint32_t last_used;
    bool     in_use;
} vga_dynamic_slot_t;

static vga_dynamic_slot_t *slot_pool = NULL;
static size_t slot_pool_count = 0;
static size_t slot_pool_capacity = 0;
static uint32_t slot_access_tick = 0;

int vga_dynamic_slot_register(uint8_t slot_id) {
    /* Check if already in pool */
    for (size_t i = 0; i < slot_pool_count; i++) {
        if (slot_pool[i].slot_id == slot_id) {
            return 0;
        }
    }

    /* Grow pool capacity dynamically if needed */
    if (slot_pool_count >= slot_pool_capacity) {
        size_t new_cap = (slot_pool_capacity == 0) ? 32 : (slot_pool_capacity * 2);
        vga_dynamic_slot_t *new_pool = (vga_dynamic_slot_t *)kmalloc(new_cap * sizeof(vga_dynamic_slot_t));
        if (new_pool == NULL) {
            return -1;
        }
        if (slot_pool != NULL) {
            memcpy(new_pool, slot_pool, slot_pool_count * sizeof(vga_dynamic_slot_t));
            kfree(slot_pool);
        }
        slot_pool = new_pool;
        slot_pool_capacity = new_cap;
    }

    slot_pool[slot_pool_count].slot_id = slot_id;
    slot_pool[slot_pool_count].codepoint = 0;
    slot_pool[slot_pool_count].last_used = 0;
    slot_pool[slot_pool_count].in_use = false;
    slot_pool_count++;
    return 0;
}

int vga_dynamic_slot_register_range(uint8_t start, uint8_t end) {
    for (uint32_t s = start; s <= end; s++) {
        vga_dynamic_slot_register((uint8_t)s);
    }
    return 0;
}

static void init_dynamic_slots_if_needed(void) {
    if (slot_pool_count > 0) return;
    /* Dynamically allocate slots from available non-ASCII ranges (no hardcoded arrays) */
    vga_dynamic_slot_register_range(0x80, 0xFF);
    vga_dynamic_slot_register_range(0x01, 0x1E);
}

static const uint8_t* lookup_custom_glyph(uint32_t codepoint) {
    custom_glyph_node_t *cur = custom_glyph_head;
    while (cur != NULL) {
        if (cur->codepoint == codepoint) {
            return cur->bitmap;
        }
        cur = cur->next;
    }
    return NULL;
}

static void save_custom_glyph(uint32_t codepoint, const uint8_t *bitmap) {
    if (bitmap == NULL) return;
    custom_glyph_node_t *cur = custom_glyph_head;
    while (cur != NULL) {
        if (cur->codepoint == codepoint) {
            memcpy(cur->bitmap, bitmap, 16);
            return;
        }
        cur = cur->next;
    }
    custom_glyph_node_t *node = kmalloc(sizeof(custom_glyph_node_t));
    if (node != NULL) {
        node->codepoint = codepoint;
        memcpy(node->bitmap, bitmap, 16);
        node->next = custom_glyph_head;
        custom_glyph_head = node;
    }
}

void vga_font_clear_registry(void) {
    /* Clear active slot cache */
    for (size_t i = 0; i < slot_pool_count; i++) {
        slot_pool[i].in_use = false;
        slot_pool[i].codepoint = 0;
        slot_pool[i].last_used = 0;
    }
}

void vga_font_reset_registry(void) {
    vga_font_clear_registry();
    /* Free all dynamically allocated custom glyphs */
    custom_glyph_node_t *cur = custom_glyph_head;
    while (cur != NULL) {
        custom_glyph_node_t *next = cur->next;
        kfree(cur);
        cur = next;
    }
    custom_glyph_head = NULL;
}

int vga_font_register_glyph(uint32_t codepoint, const uint8_t *bitmap) {
    init_dynamic_slots_if_needed();

    /* If custom bitmap provided, persist it in heap */
    if (bitmap != NULL) {
        save_custom_glyph(codepoint, bitmap);
    }

    if (slot_pool == NULL || slot_pool_count == 0) {
        return -1;
    }

    /* If codepoint already active in an allocated slot, touch LRU and return slot */
    for (size_t i = 0; i < slot_pool_count; i++) {
        if (slot_pool[i].in_use && slot_pool[i].codepoint == codepoint) {
            slot_pool[i].last_used = ++slot_access_tick;
            return (int)slot_pool[i].slot_id;
        }
    }

    /* Find best slot: either first unused slot or least-recently-used (LRU) */
    size_t best_slot = 0;
    uint32_t min_tick = 0xFFFFFFFF;

    for (size_t i = 0; i < slot_pool_count; i++) {
        if (!slot_pool[i].in_use) {
            best_slot = i;
            break;
        }
        if (slot_pool[i].last_used < min_tick) {
            min_tick = slot_pool[i].last_used;
            best_slot = i;
        }
    }

    /* Retrieve glyph bitmap from custom heap or disk font database */
    const uint8_t *glyph_bmp = bitmap;
    if (glyph_bmp == NULL) {
        glyph_bmp = lookup_custom_glyph(codepoint);
    }
    if (glyph_bmp == NULL) {
        if (font_db == NULL) {
            vga_font_load_database("/system/fonts.bin");
        }
        glyph_bmp = vga_font_lookup_in_db(codepoint);
    }

    uint8_t slot_id = slot_pool[best_slot].slot_id;

    if (glyph_bmp != NULL) {
        vga_load_font(slot_id, 1, glyph_bmp);
    }

    /* Update dynamic slot mapping */
    slot_pool[best_slot].codepoint = codepoint;
    slot_pool[best_slot].in_use = true;
    slot_pool[best_slot].last_used = ++slot_access_tick;

    return (int)slot_id;
}

void vga_font_apply_glyphs(const dynamic_glyph_def_t *glyphs, uint32_t count) {
    if (glyphs == NULL || count == 0) return;
    for (uint32_t i = 0; i < count; i++) {
        vga_font_register_glyph(glyphs[i].codepoint, glyphs[i].bitmap);
    }
}

void vga_font_reapply_active(void) {
    init_dynamic_slots_if_needed();
    if (slot_pool == NULL || slot_pool_count == 0) return;

    for (size_t i = 0; i < slot_pool_count; i++) {
        if (!slot_pool[i].in_use) continue;
        uint32_t codepoint = slot_pool[i].codepoint;
        uint8_t slot_id = slot_pool[i].slot_id;

        const uint8_t *glyph_bmp = lookup_custom_glyph(codepoint);
        if (glyph_bmp == NULL) {
            glyph_bmp = vga_font_lookup_in_db(codepoint);
        }
        if (glyph_bmp != NULL) {
            vga_load_font(slot_id, 1, glyph_bmp);
        }
    }
}

int vga_load_cyrillic_font(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    return vga_font_load_database(path);
}

uint8_t utf8_to_vga_glyph(const char *str, size_t str_len, size_t *out_bytes) {
    if (str == NULL || str_len == 0) {
        if (out_bytes) *out_bytes = 0;
        return ' ';
    }
    unsigned char b1 = (unsigned char)str[0];
    if (b1 < 0x80) {
        if (out_bytes) *out_bytes = 1;
        return b1;
    }

    uint32_t codepoint = 0;
    size_t bytes = 1;

    /* 2-byte UTF-8 */
    if ((b1 & 0xE0) == 0xC0) {
        bytes = 2;
        if (str_len >= 2) {
            unsigned char b2 = (unsigned char)str[1];
            codepoint = ((b1 & 0x1F) << 6) | (b2 & 0x3F);
        } else {
            codepoint = b1;
        }
    }
    /* 3-byte UTF-8 */
    else if ((b1 & 0xF0) == 0xE0) {
        bytes = 3;
        if (str_len >= 3) {
            unsigned char b2 = (unsigned char)str[1];
            unsigned char b3 = (unsigned char)str[2];
            codepoint = ((b1 & 0x0F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
        } else {
            codepoint = b1;
        }
    }
    /* 4-byte UTF-8 */
    else if ((b1 & 0xF8) == 0xF0) {
        bytes = 4;
        if (str_len >= 4) {
            unsigned char b2 = (unsigned char)str[1];
            unsigned char b3 = (unsigned char)str[2];
            unsigned char b4 = (unsigned char)str[3];
            codepoint = ((b1 & 0x07) << 18) | ((b2 & 0x3F) << 12) | ((b3 & 0x3F) << 6) | (b4 & 0x3F);
        } else {
            codepoint = b1;
        }
    } else {
        bytes = 1;
        codepoint = b1;
    }

    if (out_bytes) *out_bytes = bytes;

    if (font_app_mode) {
        int slot = ipo_vga_glyph(codepoint);
        if (slot >= 0) {
            return (uint8_t)slot;
        }
        return (codepoint < 0x80) ? (uint8_t)codepoint : '?';
    }

    /* Dynamic persistent slot lookup or registration */
    int slot = vga_font_register_glyph(codepoint, NULL);
    if (slot >= 0) {
        return (uint8_t)slot;
    }

    return (codepoint < 0x80) ? (uint8_t)codepoint : '?';
}
