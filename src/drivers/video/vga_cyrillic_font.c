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
    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));

    outb(0x3C4, 0x00); outb(0x3C5, 0x01); // Synchronous reset
    outb(0x3C4, 0x02); outb(0x3C5, 0x04); // Write only to Plane 2
    outb(0x3C4, 0x04); outb(0x3C5, 0x07); // Sequential addressing
    outb(0x3C4, 0x00); outb(0x3C5, 0x03); // Clear reset

    outb(0x3CE, 0x04); outb(0x3CF, 0x02); // Read map = Plane 2
    outb(0x3CE, 0x05); outb(0x3CF, 0x00); // Write mode 0
    outb(0x3CE, 0x06); outb(0x3CF, 0x04); // Map 64KB memory to 0xA0000 only

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

    __asm__ volatile("push %0; popf" : : "r"(flags));

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

    if (!vga_font_cache_initialized) {
        vga_init_font_cache();
    }

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

    /* In graphics mode (e.g. Mode 13h), do NOT touch Plane 2 sequencer/GC registers */
    if (vga_is_graphics_mode()) return;

    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));

    /* Set Sequencer registers for Plane 2 write */
    outb(0x3C4, 0x00); outb(0x3C5, 0x01); // Synchronous reset
    outb(0x3C4, 0x02); outb(0x3C5, 0x04); // Write only to Plane 2
    outb(0x3C4, 0x04); outb(0x3C5, 0x07); // Sequential addressing
    outb(0x3C4, 0x00); outb(0x3C5, 0x03); // Clear reset

    /* Set Graphics Controller registers */
    outb(0x3CE, 0x04); outb(0x3CF, 0x02); // Read map = Plane 2
    outb(0x3CE, 0x05); outb(0x3CF, 0x00); // Write mode 0
    outb(0x3CE, 0x06); outb(0x3CF, 0x04); // Map 64KB memory to 0xA0000 only

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

    __asm__ volatile("push %0; popf" : : "r"(flags));
}

/* =========================================================================
 * UNIVERSAL DYNAMIC SELF-EXPANDING FONT DATABASE (No limits, no hardcoded arrays)
 * ========================================================================= */

typedef struct {
    uint32_t codepoint;
    uint8_t  *bitmap;
    size_t   bitmap_len;
    size_t   bitmap_cap;
} font_record_t;

static void font_record_init(font_record_t *rec) {
    if (rec) memset(rec, 0, sizeof(*rec));
}

static bool font_record_set_bitmap(font_record_t *rec, const uint8_t *data, size_t len) {
    if (!rec) return false;
    if (len > rec->bitmap_cap || !rec->bitmap) {
        size_t new_cap = (rec->bitmap_cap == 0) ? 16 : rec->bitmap_cap;
        while (new_cap < len) new_cap *= 2;
        uint8_t *new_buf = (uint8_t *)krealloc(rec->bitmap, new_cap);
        if (!new_buf) return false;
        rec->bitmap = new_buf;
        rec->bitmap_cap = new_cap;
    }
    if (data && len > 0) memcpy(rec->bitmap, data, len);
    rec->bitmap_len = len;
    return true;
}

static void font_record_free(font_record_t *rec) {
    if (rec) {
        if (rec->bitmap) {
            kfree(rec->bitmap);
            rec->bitmap = NULL;
        }
        rec->bitmap_len = 0;
        rec->bitmap_cap = 0;
    }
}

static font_record_t *font_db = NULL;
static uint32_t font_db_count = 0;
static uint32_t font_db_capacity = 0;

static void vga_font_db_clear(void) {
    if (font_db) {
        for (uint32_t i = 0; i < font_db_count; i++) {
            font_record_free(&font_db[i]);
        }
        kfree(font_db);
        font_db = NULL;
    }
    font_db_count = 0;
    font_db_capacity = 0;
}

static bool vga_font_db_add(uint32_t codepoint, const uint8_t *bitmap, size_t len) {
    if (font_db_count >= font_db_capacity) {
        uint32_t new_cap = (font_db_capacity == 0) ? 32 : (font_db_capacity * 2);
        font_record_t *new_db = (font_record_t *)krealloc(font_db, new_cap * sizeof(font_record_t));
        if (!new_db) return false;
        font_db = new_db;
        font_db_capacity = new_cap;
    }
    font_record_t *rec = &font_db[font_db_count++];
    font_record_init(rec);
    rec->codepoint = codepoint;
    return font_record_set_bitmap(rec, bitmap, len);
}

/* Dynamic self-expanding current font name and path, empty by default */
static char *current_font_name = NULL;
static size_t current_font_name_len = 0;
static size_t current_font_name_cap = 0;

static char *current_font_path = NULL;
static size_t current_font_path_len = 0;
static size_t current_font_path_cap = 0;

static bool vga_font_set_current_name(const char *name) {
    if (!name) name = "";
    size_t len = strlen(name);
    if (len + 1 > current_font_name_cap || !current_font_name) {
        size_t new_cap = (current_font_name_cap == 0) ? 16 : current_font_name_cap;
        while (new_cap < len + 1) new_cap *= 2;
        char *new_buf = (char *)krealloc(current_font_name, new_cap);
        if (!new_buf) return false;
        current_font_name = new_buf;
        current_font_name_cap = new_cap;
    }
    memcpy(current_font_name, name, len + 1);
    current_font_name_len = len;
    return true;
}

static bool vga_font_set_current_path(const char *path) {
    if (!path) path = "";
    size_t len = strlen(path);
    if (len + 1 > current_font_path_cap || !current_font_path) {
        size_t new_cap = (current_font_path_cap == 0) ? 16 : current_font_path_cap;
        while (new_cap < len + 1) new_cap *= 2;
        char *new_buf = (char *)krealloc(current_font_path, new_cap);
        if (!new_buf) return false;
        current_font_path = new_buf;
        current_font_path_cap = new_cap;
    }
    memcpy(current_font_path, path, len + 1);
    current_font_path_len = len;
    return true;
}

static bool font_app_mode = false;

void vga_font_set_app_mode(bool is_app) {
    font_app_mode = is_app;
}

int vga_font_load_database(const char *path) {
    if (path == NULL || path[0] == '\0') {
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

        vga_font_db_clear();

        uint32_t payload_len = (uint32_t)(st.size - 8);
        uint8_t *raw_buf = (uint8_t *)kmalloc(payload_len);
        if (raw_buf != NULL) {
            int bytes_read = ipo_read(fd, raw_buf, payload_len, 8);
            ipo_close(fd);
            if (bytes_read < (int)payload_len) {
                kfree(raw_buf);
                return -6;
            }
            uint32_t num_rec = bytes_read / 20;
            if (num_rec > count) num_rec = count;
            for (uint32_t i = 0; i < num_rec; i++) {
                uint32_t cp = *(uint32_t *)(raw_buf + i * 20);
                vga_font_db_add(cp, raw_buf + i * 20 + 4, 16);
            }
            kfree(raw_buf);
        } else {
            /* Fallback dynamic single-record read */
            uint8_t rec[20];
            for (uint32_t i = 0; i < count; i++) {
                if (ipo_read(fd, rec, 20, 8 + i * 20) != 20) break;
                uint32_t cp = *(uint32_t *)rec;
                vga_font_db_add(cp, rec + 4, 16);
            }
            ipo_close(fd);
        }

        if (font_db_count == 0) {
            return -6;
        }

        vga_font_set_current_path(path);

        /* Derive font name from filename */
        const char *base = strrchr(path, '/');
        const char *name_start = base ? base + 1 : path;
        char *temp_name = (char *)kmalloc(strlen(name_start) + 1);
        if (temp_name) {
            strcpy(temp_name, name_start);
            char *dot = strrchr(temp_name, '.');
            if (dot) *dot = '\0';
            vga_font_set_current_name(temp_name);
            kfree(temp_name);
        } else {
            vga_font_set_current_name(name_start);
        }

        /* Upload base ASCII/extended glyphs to hardware VGA plane 2 in one batch */
        uint8_t *all_glyphs = (uint8_t *)kmalloc(256 * 16);
        bool *has_glyph = (bool *)kmalloc(256 * sizeof(bool));
        if (all_glyphs && has_glyph) {
            memset(has_glyph, 0, 256 * sizeof(bool));
            for (uint32_t i = 0; i < font_db_count; i++) {
                if (font_db[i].codepoint < 256 && font_db[i].bitmap) {
                    uint8_t cp = (uint8_t)font_db[i].codepoint;
                    size_t cpy_len = (font_db[i].bitmap_len >= 16) ? 16 : font_db[i].bitmap_len;
                    memcpy(&all_glyphs[cp * 16], font_db[i].bitmap, cpy_len);
                    if (cpy_len < 16) memset(&all_glyphs[cp * 16 + cpy_len], 0, 16 - cpy_len);
                    has_glyph[cp] = true;
                }
            }
            for (int i = 0; i < 256; i++) {
                if (!has_glyph[i]) {
                    memcpy(&all_glyphs[i * 16], vga_default_font_data[i], 16);
                }
            }
            vga_load_font(0, 256, all_glyphs);
        }
        if (all_glyphs) kfree(all_glyphs);
        if (has_glyph) kfree(has_glyph);

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

        vga_font_db_clear();

        uint32_t payload_len = (uint32_t)(st.size - 8);
        uint8_t *raw_buf = (uint8_t *)kmalloc(payload_len);
        if (raw_buf != NULL) {
            int bytes_read = ipo_fs_read(fd, raw_buf, payload_len, 8);
            ipo_fs_close(fd);
            if (bytes_read < (int)payload_len) {
                kfree(raw_buf);
                return -6;
            }
            uint32_t num_rec = bytes_read / 20;
            if (num_rec > count) num_rec = count;
            for (uint32_t i = 0; i < num_rec; i++) {
                uint32_t cp = *(uint32_t *)(raw_buf + i * 20);
                vga_font_db_add(cp, raw_buf + i * 20 + 4, 16);
            }
            kfree(raw_buf);
        } else {
            /* Fallback dynamic single-record read */
            uint8_t rec[20];
            for (uint32_t i = 0; i < count; i++) {
                if (ipo_fs_read(fd, rec, 20, 8 + i * 20) != 20) break;
                uint32_t cp = *(uint32_t *)rec;
                vga_font_db_add(cp, rec + 4, 16);
            }
            ipo_fs_close(fd);
        }

        if (font_db_count == 0) {
            return -6;
        }

        vga_font_set_current_path(path);

        const char *base = strrchr(path, '/');
        const char *name_start = base ? base + 1 : path;
        char *temp_name = (char *)kmalloc(strlen(name_start) + 1);
        if (temp_name) {
            strcpy(temp_name, name_start);
            char *dot = strrchr(temp_name, '.');
            if (dot) *dot = '\0';
            vga_font_set_current_name(temp_name);
            kfree(temp_name);
        } else {
            vga_font_set_current_name(name_start);
        }

        /* Upload base ASCII/extended glyphs to hardware VGA plane 2 in a single batch */
        uint8_t *all_glyphs = (uint8_t *)kmalloc(256 * 16);
        bool *has_glyph = (bool *)kmalloc(256 * sizeof(bool));
        if (all_glyphs && has_glyph) {
            memset(has_glyph, 0, 256 * sizeof(bool));
            for (uint32_t i = 0; i < font_db_count; i++) {
                if (font_db[i].codepoint < 256 && font_db[i].bitmap) {
                    uint8_t cp = (uint8_t)font_db[i].codepoint;
                    size_t cpy_len = (font_db[i].bitmap_len >= 16) ? 16 : font_db[i].bitmap_len;
                    memcpy(&all_glyphs[cp * 16], font_db[i].bitmap, cpy_len);
                    if (cpy_len < 16) memset(&all_glyphs[cp * 16 + cpy_len], 0, 16 - cpy_len);
                    has_glyph[cp] = true;
                }
            }
            for (int i = 0; i < 256; i++) {
                if (!has_glyph[i]) {
                    memcpy(&all_glyphs[i * 16], vga_default_font_data[i], 16);
                }
            }
            vga_load_font(0, 256, all_glyphs);
        }
        if (all_glyphs) kfree(all_glyphs);
        if (has_glyph) kfree(has_glyph);

        vga_font_reset_registry();
        return 0;
    }
}

const char* vga_font_get_current_name(void) {
    return (current_font_name && current_font_name[0] != '\0') ? current_font_name : "";
}

const char* vga_font_get_current_path(void) {
    return (current_font_path && current_font_path[0] != '\0') ? current_font_path : "";
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
        const char *name = vga_font_get_current_name();
        strncpy(name_buf, name, name_size - 1);
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
    uint8_t  *bitmap;
    size_t   bitmap_len;
    size_t   bitmap_cap;
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

static const uint8_t* vga_font_lookup_builtin(uint32_t codepoint) {
    /* Built-in high-quality 8x16 glyphs for common punctuation and symbols
     * so they render immediately even before disk is mounted or if font database is missing. */
    static const uint8_t em_dash[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const uint8_t en_dash[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const uint8_t horiz_bar[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const uint8_t ellipsis[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x49, 0x00, 0x00
    };
    static const uint8_t quote_left[16] = {
        0x00, 0x00, 0x22, 0x44, 0x44, 0x66, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const uint8_t quote_right[16] = {
        0x00, 0x00, 0x66, 0x22, 0x22, 0x44, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const uint8_t guillemet_l[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x12, 0x24,
        0x24, 0x48, 0x24, 0x24, 0x12, 0x12, 0x00, 0x00
    };
    static const uint8_t guillemet_r[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x48, 0x24,
        0x24, 0x12, 0x24, 0x24, 0x48, 0x48, 0x00, 0x00
    };
    static const uint8_t bullet[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x7C,
        0x7C, 0x7C, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    switch (codepoint) {
        case 0x2014: /* em dash '—' */
            return em_dash;
        case 0x2013: /* en dash '–' */
        case 0x2010: /* hyphen */
        case 0x2011: /* non-breaking hyphen */
        case 0x2012: /* figure dash */
        case 0x2212: /* minus sign '−' */
            return en_dash;
        case 0x2015: /* horizontal bar '―' */
            return horiz_bar;
        case 0x2026: /* horizontal ellipsis '…' */
            return ellipsis;
        case 0x201C: /* left double quotation mark '“' */
            return quote_left;
        case 0x201D: /* right double quotation mark '”' */
        case 0x201E: /* double low-9 quotation mark '„' */
            return quote_right;
        case 0x00AB: /* left-pointing double angle quotation mark '«' */
            return guillemet_l;
        case 0x00BB: /* right-pointing double angle quotation mark '»' */
            return guillemet_r;
        case 0x2022: /* bullet '•' */
            return bullet;
        default:
            return NULL;
    }
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
            if (16 > cur->bitmap_cap || !cur->bitmap) {
                uint8_t *new_b = (uint8_t *)krealloc(cur->bitmap, 16);
                if (!new_b) return;
                cur->bitmap = new_b;
                cur->bitmap_cap = 16;
            }
            memcpy(cur->bitmap, bitmap, 16);
            cur->bitmap_len = 16;
            return;
        }
        cur = cur->next;
    }
    custom_glyph_node_t *node = (custom_glyph_node_t *)kmalloc(sizeof(custom_glyph_node_t));
    if (node != NULL) {
        node->codepoint = codepoint;
        node->bitmap = (uint8_t *)kmalloc(16);
        if (node->bitmap) {
            memcpy(node->bitmap, bitmap, 16);
            node->bitmap_len = 16;
            node->bitmap_cap = 16;
        } else {
            node->bitmap_len = 0;
            node->bitmap_cap = 0;
        }
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
        if (cur->bitmap) {
            kfree(cur->bitmap);
            cur->bitmap = NULL;
        }
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

    /* Retrieve glyph bitmap from custom heap, disk font database, or built-in fallback */
    const uint8_t *glyph_bmp = bitmap;
    if (glyph_bmp == NULL) {
        glyph_bmp = lookup_custom_glyph(codepoint);
    }
    if (glyph_bmp == NULL) {
        glyph_bmp = vga_font_lookup_in_db(codepoint);
    }
    if (glyph_bmp == NULL) {
        glyph_bmp = vga_font_lookup_builtin(codepoint);
    }

    /* If no bitmap is found anywhere, do not allocate an empty slot! */
    if (glyph_bmp == NULL) {
        return -1;
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

    uint8_t slot_id = slot_pool[best_slot].slot_id;
    vga_load_font(slot_id, 1, glyph_bmp);

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
        if (glyph_bmp == NULL) {
            glyph_bmp = vga_font_lookup_builtin(codepoint);
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
        if (codepoint == 0x2014 || codepoint == 0x2013 || codepoint == 0x2015 || codepoint == 0x2212 ||
            codepoint == 0x2010 || codepoint == 0x2011 || codepoint == 0x2012) {
            return '-';
        }
        if (codepoint == 0x2026) return '.';
        if (codepoint == 0x00AB || codepoint == 0x00BB || codepoint == 0x201C || codepoint == 0x201D || codepoint == 0x201E) return '"';
        if (codepoint == 0x2022) return '*';
        return (codepoint < 0x80) ? (uint8_t)codepoint : '?';
    }

    /* Dynamic persistent slot lookup or registration */
    int slot = vga_font_register_glyph(codepoint, NULL);
    if (slot >= 0) {
        return (uint8_t)slot;
    }

    if (codepoint == 0x2014 || codepoint == 0x2013 || codepoint == 0x2015 || codepoint == 0x2212 ||
        codepoint == 0x2010 || codepoint == 0x2011 || codepoint == 0x2012) {
        return '-';
    }
    if (codepoint == 0x2026) return '.';
    if (codepoint == 0x00AB || codepoint == 0x00BB || codepoint == 0x201C || codepoint == 0x201D || codepoint == 0x201E) return '"';
    if (codepoint == 0x2022) return '*';

    return (codepoint < 0x80) ? (uint8_t)codepoint : '?';
}
