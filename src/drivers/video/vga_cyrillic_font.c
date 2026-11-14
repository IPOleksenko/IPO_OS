#include <vga.h>
#include <ioport.h>
#include <stddef.h>
#include <string.h>
#include <memory/kmalloc.h>
#include <file_system/ipo_fs.h>
#include <syscall.h>

static uint8_t *vga_font_cache = NULL;
static bool vga_font_cache_initialized = false;

static void vga_ensure_font_cache_storage(void) {
    if (!vga_font_cache) {
        vga_font_cache = (uint8_t *)kmalloc(256u * 16u);
    }
}

uint32_t vga_plane2_font_enter(void) {
    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));

    outb(0x3C4, 0x00); outb(0x3C5, 0x01); /* Synchronous reset */
    outb(0x3C4, 0x02); outb(0x3C5, 0x04); /* Plane 2 */
    outb(0x3C4, 0x04); outb(0x3C5, 0x07); /* Sequential addressing (disable odd/even) */
    outb(0x3C4, 0x00); outb(0x3C5, 0x03); /* Clear reset */

    outb(0x3CE, 0x00); outb(0x3CF, 0x00);
    outb(0x3CE, 0x01); outb(0x3CF, 0x00);
    outb(0x3CE, 0x03); outb(0x3CF, 0x00);
    outb(0x3CE, 0x04); outb(0x3CF, 0x02); /* Read map = Plane 2 */
    outb(0x3CE, 0x05); outb(0x3CF, 0x00); /* Write mode 0 */
    outb(0x3CE, 0x06); outb(0x3CF, 0x04); /* Map 64KB at 0xA0000 only */
    outb(0x3CE, 0x08); outb(0x3CF, 0xFF);

    return flags;
}

void vga_plane2_font_exit(uint32_t saved_flags) {
    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x03); /* Enable planes 0 & 1 */
    outb(0x3C4, 0x04); outb(0x3C5, 0x03); /* Odd/even addressing */
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);

    outb(0x3CE, 0x00); outb(0x3CF, 0x00);
    outb(0x3CE, 0x01); outb(0x3CF, 0x00);
    outb(0x3CE, 0x03); outb(0x3CF, 0x00);
    outb(0x3CE, 0x04); outb(0x3CF, 0x00);
    outb(0x3CE, 0x05); outb(0x3CF, 0x10); /* Normal host mode */
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E); /* Map memory back to 0xB8000 */
    outb(0x3CE, 0x08); outb(0x3CF, 0xFF);

    __asm__ volatile("push %0; popf" : : "r"(saved_flags));
}

void vga_init_font_cache(void) {
    vga_ensure_font_cache_storage();
    if (!vga_font_cache) {
        return;
    }

    for (int i = 0; i < 256; i++) {
        memcpy(&vga_font_cache[(size_t)i * 16u], vga_default_font_data[i], 16u);
    }

    uint32_t flags = vga_plane2_font_enter();

    /* Verify Plane 2 font memory is populated by checking 'A' (0x41). */
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
                vga_font_cache[c * 16u + (uint32_t)r] = src[r];
            }
        }
    }

    vga_plane2_font_exit(flags);

    vga_font_cache_initialized = true;
}

const uint8_t* vga_font_get_cached_glyph(uint8_t char_code) {
    if (!vga_font_cache_initialized) {
        vga_init_font_cache();
    }
    if (!vga_font_cache) {
        return vga_default_font_data[char_code];
    }
    return &vga_font_cache[(size_t)char_code * 16u];
}

/* Load 8x16 glyphs (16 bytes per glyph) into VGA Plane 2 font RAM at 0xA0000. */
void vga_load_font(uint8_t start_code, uint32_t count, const uint8_t *glyphs) {
    if (glyphs == NULL || count == 0) return;

    uint32_t max_count = 256u - (uint32_t)start_code;
    if (count > max_count) count = max_count;

    if (!vga_font_cache_initialized) {
        vga_init_font_cache();
    }

    if (vga_font_cache) {
        for (uint32_t c = 0; c < count; c++) {
            uint8_t code = (uint8_t)(start_code + c);
            if (code != VGA_CURSOR_GLYPH_SLOT) {
                memcpy(&vga_font_cache[(size_t)code * 16u], glyphs + (c * 16u), 16u);
            }
        }
    }

    if (vga_is_graphics_mode()) return;

    uint32_t flags = vga_plane2_font_enter();

    for (uint32_t c = 0; c < count; c++) {
        uint8_t char_code = (uint8_t)(start_code + c);
        volatile uint8_t *dest = (volatile uint8_t *)(0xA0000 + ((uint32_t)char_code * 32u));
        const uint8_t *src = glyphs + (c * 16u);
        for (int row = 0; row < 16; row++) {
            dest[row] = src[row];
        }
    }

    vga_plane2_font_exit(flags);
}

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
        while (new_cap < len) {
            if (new_cap > SIZE_MAX / 2) return false;
            new_cap *= 2;
        }
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
        if (font_db_capacity > UINT32_MAX / 2u) return false;
        uint32_t new_cap = (font_db_capacity == 0) ? 32u : (font_db_capacity * 2u);
        if ((size_t)new_cap > SIZE_MAX / sizeof(font_record_t)) return false;
        font_record_t *new_db = (font_record_t *)krealloc(font_db, (size_t)new_cap * sizeof(font_record_t));
        if (!new_db) return false;
        font_db = new_db;
        font_db_capacity = new_cap;
    }
    font_record_t *rec = &font_db[font_db_count];
    font_record_init(rec);
    rec->codepoint = codepoint;
    if (!font_record_set_bitmap(rec, bitmap, len)) {
        return false;
    }
    font_db_count++;
    return true;
}

static char *current_font_name = NULL;
static size_t current_font_name_cap = 0;

static char *current_font_path = NULL;
static size_t current_font_path_cap = 0;

static bool vga_font_set_str(char **dst, size_t *cap, const char *val) {
    if (!val) val = "";
    size_t len = strlen(val);
    if (len + 1 > *cap || !*dst) {
        size_t new_cap = (*cap == 0) ? 16 : *cap;
        while (new_cap < len + 1) {
            if (new_cap > SIZE_MAX / 2) return false;
            new_cap *= 2;
        }
        char *new_buf = (char *)krealloc(*dst, new_cap);
        if (!new_buf) return false;
        *dst = new_buf;
        *cap = new_cap;
    }
    memcpy(*dst, val, len + 1);
    return true;
}

static bool font_app_mode = false;

void vga_font_set_app_mode(bool is_app) {
    font_app_mode = is_app;
}

static int vga_font_file_open(const char *path) {
    if (font_app_mode) return ipo_open(path);
    return fs_mounted ? ipo_fs_open(path) : -1;
}

static bool vga_font_file_stat(const char *path, struct ipo_inode *st) {
    return font_app_mode ? (ipo_stat(path, st) >= 0) : ipo_fs_stat(path, st);
}

static int vga_font_file_read(int fd, void *buf, uint32_t size, uint32_t offset) {
    return font_app_mode ? ipo_read(fd, buf, size, offset) : ipo_fs_read(fd, buf, size, offset);
}

static void vga_font_file_close(int fd) {
    if (font_app_mode) ipo_close(fd);
    else ipo_fs_close(fd);
}

int vga_font_load_database(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return -1;
    }

    int fd = vga_font_file_open(path);
    if (fd < 0) return -1;

    struct ipo_inode st;
    if (!vga_font_file_stat(path, &st) || st.size < 8u || st.size > 0xFFFFFFFFULL) {
        vga_font_file_close(fd);
        return -2;
    }

    uint32_t magic = 0;
    uint32_t count = 0;
    if (vga_font_file_read(fd, &magic, 4u, 0u) != 4 ||
        vga_font_file_read(fd, &count, 4u, 4u) != 4) {
        vga_font_file_close(fd);
        return -3;
    }

    if (memcmp(&magic, "IFNT", 4) != 0 || count == 0u) {
        vga_font_file_close(fd);
        return -4;
    }

    uint32_t payload_len = (uint32_t)(st.size - 8u);
    uint8_t *raw_buf = (payload_len > 0u) ? (uint8_t *)kmalloc(payload_len) : NULL;
    if (raw_buf != NULL) {
        int bytes_read = vga_font_file_read(fd, raw_buf, payload_len, 8u);
        vga_font_file_close(fd);
        if (bytes_read < (int)payload_len) {
            kfree(raw_buf);
            return -6;
        }
        vga_font_db_clear();
        uint32_t num_rec = (uint32_t)bytes_read / 20u;
        if (num_rec > count) num_rec = count;
        for (uint32_t i = 0; i < num_rec; i++) {
            uint32_t cp;
            memcpy(&cp, raw_buf + i * 20u, sizeof(cp));
            if (!vga_font_db_add(cp, raw_buf + i * 20u + 4u, 16u)) {
                break;
            }
        }
        kfree(raw_buf);
    } else {
        vga_font_db_clear();
        uint8_t rec[20];
        for (uint32_t i = 0; i < count; i++) {
            if (vga_font_file_read(fd, rec, 20u, 8u + i * 20u) != 20) break;
            uint32_t cp;
            memcpy(&cp, rec, sizeof(cp));
            if (!vga_font_db_add(cp, rec + 4, 16u)) {
                break;
            }
        }
        vga_font_file_close(fd);
    }

    if (font_db_count == 0u) {
        return -6;
    }

    vga_font_set_str(&current_font_path, &current_font_path_cap, path);

    const char *base = strrchr(path, '/');
    const char *name_start = base ? base + 1 : path;
    char *temp_name = (char *)kmalloc(strlen(name_start) + 1u);
    if (temp_name) {
        strcpy(temp_name, name_start);
        char *dot = strrchr(temp_name, '.');
        if (dot) *dot = '\0';
        vga_font_set_str(&current_font_name, &current_font_name_cap, temp_name);
        kfree(temp_name);
    } else {
        vga_font_set_str(&current_font_name, &current_font_name_cap, name_start);
    }

    uint8_t *all_glyphs = (uint8_t *)kmalloc(256u * 16u);
    bool *has_glyph = (bool *)kmalloc(256u * sizeof(bool));
    if (all_glyphs && has_glyph) {
        memset(has_glyph, 0, 256u * sizeof(bool));
        for (uint32_t i = 0; i < font_db_count; i++) {
            if (font_db[i].codepoint < 256u && font_db[i].bitmap) {
                uint8_t cp = (uint8_t)font_db[i].codepoint;
                size_t cpy_len = (font_db[i].bitmap_len >= 16u) ? 16u : font_db[i].bitmap_len;
                memcpy(&all_glyphs[(size_t)cp * 16u], font_db[i].bitmap, cpy_len);
                if (cpy_len < 16u) {
                    memset(&all_glyphs[(size_t)cp * 16u + cpy_len], 0, 16u - cpy_len);
                }
                has_glyph[cp] = true;
            }
        }
        for (int i = 0; i < 256; i++) {
            if (!has_glyph[i]) {
                memcpy(&all_glyphs[(size_t)i * 16u], vga_default_font_data[i], 16u);
            }
        }
        vga_load_font(0, 256u, all_glyphs);
    }
    if (all_glyphs) kfree(all_glyphs);
    if (has_glyph) kfree(has_glyph);

    vga_font_reset_registry();
    return 0;
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
    if (font_db == NULL || font_db_count == 0u) return NULL;
    size_t left = 0u;
    size_t right = font_db_count;
    while (left < right) {
        size_t mid = left + (right - left) / 2u;
        if (font_db[mid].codepoint == codepoint) {
            return font_db[mid].bitmap;
        }
        if (font_db[mid].codepoint < codepoint) {
            left = mid + 1u;
        } else {
            right = mid;
        }
    }
    return NULL;
}

typedef struct custom_glyph_node {
    uint32_t codepoint;
    uint8_t  *bitmap;
    size_t   bitmap_len;
    size_t   bitmap_cap;
    struct custom_glyph_node *next;
} custom_glyph_node_t;

static custom_glyph_node_t *custom_glyph_head = NULL;

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

static int vga_dynamic_slot_register(uint8_t slot_id) {
    for (size_t i = 0; i < slot_pool_count; i++) {
        if (slot_pool[i].slot_id == slot_id) {
            return 0;
        }
    }

    if (slot_pool_count >= slot_pool_capacity) {
        if (slot_pool_capacity > SIZE_MAX / 2u) return -1;
        size_t new_cap = (slot_pool_capacity == 0) ? 32u : (slot_pool_capacity * 2u);
        if (new_cap > SIZE_MAX / sizeof(vga_dynamic_slot_t)) return -1;
        vga_dynamic_slot_t *new_pool = (vga_dynamic_slot_t *)krealloc(
            slot_pool, new_cap * sizeof(vga_dynamic_slot_t));
        if (new_pool == NULL) {
            return -1;
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

static void vga_dynamic_slot_register_range(uint8_t start, uint8_t end) {
    for (uint32_t s = start; s <= end; s++) {
        if (vga_dynamic_slot_register((uint8_t)s) < 0) break;
    }
}

static void init_dynamic_slots_if_needed(void) {
    if (slot_pool_count > 0) return;
    vga_dynamic_slot_register_range(0x80, 0xFF);
    vga_dynamic_slot_register_range(0x01, 0x1E);
}

static const uint8_t* vga_font_lookup_builtin(uint32_t codepoint) {
    /* Built-in fallback 8x16 glyphs used before disk mount or when DB lacks punctuation. */
    static const uint8_t * const em_dash =
        (const uint8_t *)"\x00\x00\x00\x00\x00\x00\x00\x00\x00\xFF\x00\x00\x00\x00\x00\x00";
    static const uint8_t * const en_dash =
        (const uint8_t *)"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x7E\x00\x00\x00\x00\x00\x00";
    static const uint8_t * const horiz_bar =
        (const uint8_t *)"\x00\x00\x00\x00\x00\x00\x00\xFF\x00\x00\x00\x00\x00\x00\x00\x00";
    static const uint8_t * const ellipsis =
        (const uint8_t *)"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x49\x49\x00\x00";
    static const uint8_t * const quote_left =
        (const uint8_t *)"\x00\x00\x22\x44\x44\x66\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00";
    static const uint8_t * const quote_right =
        (const uint8_t *)"\x00\x00\x66\x22\x22\x44\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00";
    static const uint8_t * const guillemet_l =
        (const uint8_t *)"\x00\x00\x00\x00\x00\x12\x12\x24\x24\x48\x24\x24\x12\x12\x00\x00";
    static const uint8_t * const guillemet_r =
        (const uint8_t *)"\x00\x00\x00\x00\x00\x48\x48\x24\x24\x12\x24\x24\x48\x48\x00\x00";
    static const uint8_t * const bullet =
        (const uint8_t *)"\x00\x00\x00\x00\x00\x00\x38\x7C\x7C\x7C\x38\x00\x00\x00\x00\x00";

    switch (codepoint) {
        case 0x2014:
            return em_dash;
        case 0x2013:
        case 0x2010:
        case 0x2011:
        case 0x2012:
        case 0x2212:
            return en_dash;
        case 0x2015:
            return horiz_bar;
        case 0x2026:
            return ellipsis;
        case 0x201C:
            return quote_left;
        case 0x201D:
        case 0x201E:
            return quote_right;
        case 0x00AB:
            return guillemet_l;
        case 0x00BB:
            return guillemet_r;
        case 0x2022:
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
            if (16u > cur->bitmap_cap || !cur->bitmap) {
                uint8_t *new_b = (uint8_t *)krealloc(cur->bitmap, 16u);
                if (!new_b) return;
                cur->bitmap = new_b;
                cur->bitmap_cap = 16u;
            }
            memcpy(cur->bitmap, bitmap, 16u);
            cur->bitmap_len = 16u;
            return;
        }
        cur = cur->next;
    }
    custom_glyph_node_t *node = (custom_glyph_node_t *)kmalloc(sizeof(custom_glyph_node_t));
    if (!node) return;
    uint8_t *bmp_copy = (uint8_t *)kmalloc(16u);
    if (!bmp_copy) {
        kfree(node);
        return;
    }
    memcpy(bmp_copy, bitmap, 16u);
    node->codepoint = codepoint;
    node->bitmap = bmp_copy;
    node->bitmap_len = 16u;
    node->bitmap_cap = 16u;
    node->next = custom_glyph_head;
    custom_glyph_head = node;
}

static const uint8_t *vga_font_resolve_bitmap(uint32_t codepoint, const uint8_t *explicit_bmp) {
    if (explicit_bmp != NULL) return explicit_bmp;
    const uint8_t *bmp = lookup_custom_glyph(codepoint);
    if (bmp != NULL) return bmp;
    bmp = vga_font_lookup_in_db(codepoint);
    if (bmp != NULL) return bmp;
    return vga_font_lookup_builtin(codepoint);
}

void vga_font_clear_registry(void) {
    for (size_t i = 0; i < slot_pool_count; i++) {
        slot_pool[i].in_use = false;
        slot_pool[i].codepoint = 0;
        slot_pool[i].last_used = 0;
    }
}

void vga_font_reset_registry(void) {
    vga_font_clear_registry();
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

    if (bitmap != NULL) {
        save_custom_glyph(codepoint, bitmap);
    }

    if (slot_pool == NULL || slot_pool_count == 0) {
        return -1;
    }

    for (size_t i = 0; i < slot_pool_count; i++) {
        if (slot_pool[i].in_use && slot_pool[i].codepoint == codepoint) {
            slot_pool[i].last_used = ++slot_access_tick;
            return (int)slot_pool[i].slot_id;
        }
    }

    const uint8_t *glyph_bmp = vga_font_resolve_bitmap(codepoint, bitmap);
    if (glyph_bmp == NULL) {
        return -1;
    }

    size_t best_slot = 0;
    uint32_t min_tick = 0xFFFFFFFFu;

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
        const uint8_t *glyph_bmp = vga_font_resolve_bitmap(slot_pool[i].codepoint, NULL);
        if (glyph_bmp != NULL) {
            vga_load_font(slot_pool[i].slot_id, 1, glyph_bmp);
        }
    }
}

int vga_font_load(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    return vga_font_load_database(path);
}

int vga_load_cyrillic_font(const char *path) {
    return vga_font_load(path);
}

static uint8_t vga_font_ascii_fallback(uint32_t cp32) {
    switch (cp32) {
        case 0x2014:
        case 0x2013:
        case 0x2015:
        case 0x2212:
        case 0x2010:
        case 0x2011:
        case 0x2012:
            return '-';
        case 0x2026:
            return '.';
        case 0x00AB:
        case 0x00BB:
        case 0x201C:
        case 0x201D:
        case 0x201E:
            return '"';
        case 0x2022:
            return '*';
        default:
            return '?';
    }
}

uint8_t utf_codepoint_to_vga_glyph(uint64_t codepoint) {
    if (codepoint < 0x80ULL) {
        return (uint8_t)codepoint;
    }
    if (codepoint > 0xFFFFFFFFULL) {
        return '?';
    }

    uint32_t cp32 = (uint32_t)codepoint;
    int slot = font_app_mode ? ipo_vga_glyph(cp32) : vga_font_register_glyph(cp32, NULL);
    if (slot >= 0) {
        return (uint8_t)slot;
    }
    return vga_font_ascii_fallback(cp32);
}

uint8_t utf8_to_vga_glyph(const char *str, size_t str_len, size_t *out_bytes) {
    if (str == NULL || str_len == 0) {
        if (out_bytes) *out_bytes = 0;
        return ' ';
    }
    unsigned char b1 = (unsigned char)str[0];
    uint32_t def_enc = utf_get_default_encoding();
    bool fast_ascii = false;
    if (b1 < 0x80u && b1 != 0x00u &&
        (b1 >= 0x20u || b1 == '\n' || b1 == '\r' || b1 == '\t' || b1 == '\b' || b1 == 0x1Bu)) {
        if (def_enc == UTF_ENC_UTF8 || def_enc == UTF_ENC_UTF_VAR) {
            fast_ascii = true;
        } else if (def_enc == UTF_ENC_AUTO) {
            if (str_len == 1u) {
                fast_ascii = true;
            } else {
                unsigned char b2 = (unsigned char)str[1];
                bool wide32_le = (str_len >= 4u &&
                                  (unsigned char)str[2] == 0x00u &&
                                  (unsigned char)str[3] == 0x00u);
                if (!wide32_le && (b2 >= 0x20u || b2 == '\n' || b2 == '\r' || b2 == '\t' || b2 == '\b' || b2 == 0x1Bu)) {
                    fast_ascii = true;
                }
            }
        }
    }
    if (fast_ascii) {
        if (out_bytes) *out_bytes = 1;
        return b1;
    }

    uint64_t codepoint = 0;
    size_t consumed = 0;
    utf_decode_status_t st = utf_decode_next(def_enc, (const uint8_t *)str, str_len,
                                             &codepoint, &consumed);
    if (st != UTF_DECODE_OK) {
        if (consumed == 0) consumed = 1;
        codepoint = (uint64_t)b1;
    }
    if (out_bytes) *out_bytes = consumed;
    if (codepoint == UTF_BOM_CHAR) {
        return ' ';
    }
    return utf_codepoint_to_vga_glyph(codepoint);
}
