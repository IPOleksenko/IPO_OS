#include <kernel/kmod.h>
#include <kernel/driver.h>
#include <kernel/device.h>
#include <kernel/smp.h>
#include <driver/storage/block_dev.h>
#include <file_system/ipo_fs.h>
#include <memory/kmalloc.h>
#include <system/timer.h>
#include <ioport.h>
#include <string.h>
#include <stdio.h>

typedef struct kmod_format_entry {
    char *name;
    uint8_t *magic;
    uint32_t magic_len;
    uint32_t offset;
    kmod_format_loader_fn_t loader;
    struct kmod_format_entry *next;
} kmod_format_entry_t;

static kmod_t              *s_kmod_head = NULL;
static kmod_format_entry_t *s_kmod_formats = NULL;
static spinlock_t           s_kmod_lock = {0};
static uint32_t             s_next_kmod_id = 1;
static bool                 s_kmod_initialized = false;

static char *kmod_strdup_local(const char *s) {
    if (!s) s = "";
    size_t len = strlen(s);
    char *p = (char *)kmalloc(len + 1u);
    if (p) {
        if (len > 0u) memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}

const kernel_sym_entry_t *kmod_get_symbols(size_t *count) {
    if (&s_kernel_syms_count && count) *count = s_kernel_syms_count;
    else if (count) *count = 0;
    return s_kernel_syms;
}

void *kmod_resolve_symbol(const char *name) {
    if (!name || !*name) return NULL;
    if (!s_kernel_syms || !&s_kernel_syms_count || s_kernel_syms_count == 0) return NULL;

    /* Kernel symbol table is sorted alphabetically */
    int left = 0;
    int right = (int)s_kernel_syms_count - 1;
    while (left <= right) {
        int mid = left + (right - left) / 2;
        int cmp = strcmp(s_kernel_syms[mid].name, name);
        if (cmp == 0) {
            return s_kernel_syms[mid].addr;
        } else if (cmp < 0) {
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    return NULL;
}

int kmod_init(void) {
    if (s_kmod_initialized) return 0;
    spinlock_init(&s_kmod_lock);
    s_kmod_head = NULL;
    s_kmod_formats = NULL;
    s_next_kmod_id = 1;
    s_kmod_initialized = true;
    printf("[kmod] Driver Subsystem initialized (%u kernel symbols exported, bare-metal format).\n",
           &s_kernel_syms_count ? (uint32_t)s_kernel_syms_count : 0);
    return 0;
}

int kmod_register_format(const char *name, const uint8_t *magic,
                         uint32_t magic_len, uint32_t offset,
                         kmod_format_loader_fn_t loader) {
    if (!name || !*name || !magic || magic_len == 0u || !loader) return -1;
    if (!s_kmod_initialized) kmod_init();

    kmod_format_entry_t *e = (kmod_format_entry_t *)kmalloc(sizeof(kmod_format_entry_t));
    if (!e) return -2;
    e->name = kmod_strdup_local(name);
    e->magic = (uint8_t *)kmalloc(magic_len);
    if (!e->name || !e->magic) {
        if (e->name) kfree(e->name);
        if (e->magic) kfree(e->magic);
        kfree(e);
        return -2;
    }
    memcpy(e->magic, magic, magic_len);
    e->magic_len = magic_len;
    e->offset = offset;
    e->loader = loader;

    spin_lock(&s_kmod_lock);
    e->next = s_kmod_formats;
    s_kmod_formats = e;
    spin_unlock(&s_kmod_lock);
    return 0;
}

int kmod_unregister_format(const char *name) {
    if (!name || !*name) return -1;
    spin_lock(&s_kmod_lock);
    kmod_format_entry_t **curr = &s_kmod_formats;
    while (*curr) {
        kmod_format_entry_t *e = *curr;
        if (strcmp(e->name, name) == 0) {
            *curr = e->next;
            spin_unlock(&s_kmod_lock);
            kfree(e->name);
            kfree(e->magic);
            kfree(e);
            return 0;
        }
        curr = &e->next;
    }
    spin_unlock(&s_kmod_lock);
    return -2;
}

static void kmod_free_strings(kmod_t *m) {
    if (m->name) kfree(m->name);
    if (m->author) kfree(m->author);
    if (m->description) kfree(m->description);
    m->name = m->author = m->description = NULL;
}

static char *kmod_basename_alloc(const char *path) {
    if (!path || !*path) return NULL;
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    if (!*base) return NULL;

    size_t len = strlen(base);
    size_t cut = len;
    for (size_t i = len; i > 0; i--) {
        if (base[i - 1] == '.') {
            if (i - 1 > 0) cut = i - 1;
            break;
        }
    }
    char *out = (char *)kmalloc(cut + 1u);
    if (!out) return NULL;
    memcpy(out, base, cut);
    out[cut] = '\0';
    return out;
}

static kmod_format_loader_fn_t kmod_find_format_loader(const uint8_t *buf, size_t size) {
    spin_lock(&s_kmod_lock);
    for (kmod_format_entry_t *f = s_kmod_formats; f != NULL; f = f->next) {
        if (size >= (size_t)f->offset + f->magic_len &&
            memcmp(buf + f->offset, f->magic, f->magic_len) == 0) {
            kmod_format_loader_fn_t fn = f->loader;
            spin_unlock(&s_kmod_lock);
            return fn;
        }
    }
    spin_unlock(&s_kmod_lock);
    return NULL;
}

int kmod_load_buffer_named(const char *default_name, const void *buf,
                           size_t size, kmod_t **out_mod) {
    if (!buf || size == 0u) return -1;
    if (!s_kmod_initialized) kmod_init();

    /* Try registered format loaders before falling back to flat binary */
    static int s_kmod_fmt_depth = 0;
    if (s_kmod_fmt_depth < 16) {
        kmod_format_loader_fn_t fmt_loader = kmod_find_format_loader((const uint8_t *)buf, size);
        if (fmt_loader) {
            s_kmod_fmt_depth++;
            int fmt_rc = fmt_loader(default_name, buf, size, out_mod);
            s_kmod_fmt_depth--;
            return fmt_rc;
        }
    }

    if (default_name && default_name[0] && kmod_find_by_name(default_name) != NULL) {
        serial_printf("[kmod] Module '%s' is already loaded.\n", default_name);
        return -5;
    }

    uint8_t *image = (uint8_t *)kmalloc(size);
    if (!image) {
        serial_printf("[kmod] Failed to allocate %u bytes for bare-metal module image.\n", (uint32_t)size);
        return -6;
    }
    memcpy(image, buf, size);

    kmod_init_meta_t meta;
    memset(&meta, 0, sizeof(meta));

    kmod_entry_fn_t entry = (kmod_entry_fn_t)(uintptr_t)image;
    int init_ret = entry(kmod_resolve_symbol, &meta);
    if (init_ret < 0) {
        serial_printf("[kmod] Bare-metal module '%s' entry() failed with code %d\n",
                      (default_name && default_name[0]) ? default_name : "unknown", init_ret);
        kfree(image);
        return init_ret;
    }

    char *fallback_buf = NULL;
    const char *mod_name = (meta.name && meta.name[0]) ? meta.name : default_name;
    if (!mod_name || !mod_name[0]) {
        int need = snprintf(NULL, 0, "kmod_%u", s_next_kmod_id);
        if (need >= 0) {
            fallback_buf = (char *)kmalloc((size_t)need + 1u);
            if (fallback_buf) {
                snprintf(fallback_buf, (size_t)need + 1u, "kmod_%u", s_next_kmod_id);
                mod_name = fallback_buf;
            }
        }
        if (!mod_name || !mod_name[0]) mod_name = "kmod";
    }

    if (kmod_find_by_name(mod_name) != NULL) {
        serial_printf("[kmod] Module '%s' is already loaded.\n", mod_name);
        if (fallback_buf) kfree(fallback_buf);
        if (meta.cleanup) meta.cleanup();
        kfree(image);
        return -5;
    }

    kmod_t *mod = (kmod_t *)kmalloc(sizeof(kmod_t));
    if (!mod) {
        if (fallback_buf) kfree(fallback_buf);
        if (meta.cleanup) meta.cleanup();
        kfree(image);
        return -8;
    }
    memset(mod, 0, sizeof(kmod_t));

    mod->name        = kmod_strdup_local(mod_name);
    if (fallback_buf) kfree(fallback_buf);
    mod->author      = kmod_strdup_local((meta.author && meta.author[0]) ? meta.author : "");
    mod->description = kmod_strdup_local((meta.description && meta.description[0])
                                         ? meta.description
                                         : "Bare-Metal Kernel Driver Binary");
    if (!mod->name || !mod->author || !mod->description) {
        if (meta.cleanup) meta.cleanup();
        kmod_free_strings(mod);
        kfree(mod);
        kfree(image);
        return -6;
    }

    mod->id         = s_next_kmod_id++;
    mod->flags      = KMOD_FLAG_LOADED | KMOD_FLAG_ACTIVE;
    mod->image_base = image;
    mod->image_size = size;
    mod->init       = (int (*)(void))(uintptr_t)image;
    mod->cleanup    = meta.cleanup;

    if (driver_find(mod->name) == NULL) {
        driver_t drv;
        memset(&drv, 0, sizeof(drv));
        drv.name = mod->name;
        drv.description = mod->description;
        drv.flags = DRIVER_FLAG_KERNEL | DRIVER_FLAG_ACTIVE;
        driver_register(&drv);
    }

    spin_lock(&s_kmod_lock);
    mod->next = s_kmod_head;
    s_kmod_head = mod;
    spin_unlock(&s_kmod_lock);

    serial_printf("[kmod] Loaded bare-metal module '%s' (by %s, id=%u, %u bytes @ %p)\n",
                  mod->name, mod->author, mod->id, (uint32_t)mod->image_size, mod->image_base);

    if (out_mod) *out_mod = mod;
    return 0;
}

int kmod_load_buffer(const void *buf, size_t size, kmod_t **out_mod) {
    return kmod_load_buffer_named(NULL, buf, size, out_mod);
}

int kmod_load_file(const char *path, kmod_t **out_mod) {
    if (!path || !*path) return -1;

    struct ipo_inode st;
    if (!ipo_fs_stat(path, &st)) {
        serial_printf("[kmod] File not found: '%s'\n", path);
        return -2;
    }

    if ((st.mode & IPO_INODE_TYPE_DIR) != 0) {
        serial_printf("[kmod] Cannot load '%s': is a directory\n", path);
        return -3;
    }

    uint32_t file_sz = (uint32_t)st.size;
    if (file_sz == 0u) {
        serial_printf("[kmod] Invalid module file size: %u bytes\n", file_sz);
        return -3;
    }

    int fd = ipo_fs_open(path);
    if (fd < 0) {
        serial_printf("[kmod] Failed to open module file: '%s'\n", path);
        return -2;
    }

    uint8_t *buf = (uint8_t *)kmalloc(file_sz);
    if (!buf) {
        ipo_fs_close(fd);
        return -4;
    }

    int read_bytes = ipo_fs_read(fd, buf, file_sz, 0);
    ipo_fs_close(fd);

    if (read_bytes != (int)file_sz) {
        kfree(buf);
        return -5;
    }

    char *base_name = kmod_basename_alloc(path);
    int ret = kmod_load_buffer_named(base_name, buf, (size_t)file_sz, out_mod);
    if (base_name) kfree(base_name);
    kfree(buf);
    return ret;
}

int kmod_unload(uint32_t id) {
    if (id == 0) return -1;

    spin_lock(&s_kmod_lock);
    kmod_t *curr = s_kmod_head;
    kmod_t *prev = NULL;

    while (curr) {
        if (curr->id == id) {
            if (curr->ref_count > 0) {
                spin_unlock(&s_kmod_lock);
                serial_printf("[kmod] Cannot unload '%s': active references (%u)\n", curr->name, curr->ref_count);
                return -2;
            }

            if (prev) {
                prev->next = curr->next;
            } else {
                s_kmod_head = curr->next;
            }
            spin_unlock(&s_kmod_lock);

            /* Run module cleanup before unregistering its driver */
            if (curr->cleanup != NULL) {
                curr->cleanup();
            }

            driver_unregister_force(curr->name);

            serial_printf("[kmod] Unloaded module '%s' (id=%u)\n", curr->name, curr->id);

            if (curr->image_base) {
                kfree(curr->image_base);
            }
            kmod_free_strings(curr);
            kfree(curr);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }

    spin_unlock(&s_kmod_lock);
    return -3;
}

int kmod_unload_by_name(const char *name) {
    if (!name || !*name) return -1;
    kmod_t *mod = kmod_find_by_name(name);
    if (!mod) return -2;
    return kmod_unload(mod->id);
}

kmod_t *kmod_find_by_id(uint32_t id) {
    if (id == 0) return NULL;
    spin_lock(&s_kmod_lock);
    kmod_t *curr = s_kmod_head;
    while (curr) {
        if (curr->id == id) {
            spin_unlock(&s_kmod_lock);
            return curr;
        }
        curr = curr->next;
    }
    spin_unlock(&s_kmod_lock);
    return NULL;
}

kmod_t *kmod_find_by_name(const char *name) {
    if (!name || !*name) return NULL;
    spin_lock(&s_kmod_lock);
    kmod_t *curr = s_kmod_head;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            spin_unlock(&s_kmod_lock);
            return curr;
        }
        curr = curr->next;
    }
    spin_unlock(&s_kmod_lock);
    return NULL;
}

uint32_t kmod_count(void) {
    spin_lock(&s_kmod_lock);
    uint32_t cnt = 0;
    kmod_t *curr = s_kmod_head;
    while (curr) {
        cnt++;
        curr = curr->next;
    }
    spin_unlock(&s_kmod_lock);
    return cnt;
}

void kmod_print_list(void) {
    if (!s_kmod_initialized) kmod_init();

    printf("=== Loaded Dynamic Kernel Modules (KMOD) ===\n");
    printf("  Loaded Modules: %u\n", kmod_count());

    spin_lock(&s_kmod_lock);
    kmod_t *curr = s_kmod_head;
    if (!curr) {
        printf("  No modules loaded.\n");
        spin_unlock(&s_kmod_lock);
        return;
    }

    printf("  ID   Name             Author          Base Addr   Size    Status\n");
    printf("  -----------------------------------------------------------------\n");

    while (curr) {
        printf("  %-4u %-16s %-15s %p  %-7u %s\n",
               curr->id,
               curr->name,
               curr->author ? curr->author : "",
               curr->image_base,
               (uint32_t)curr->image_size,
               (curr->flags & KMOD_FLAG_ACTIVE) ? "ACTIVE" : "LOADED");
        curr = curr->next;
    }
    spin_unlock(&s_kmod_lock);
}
