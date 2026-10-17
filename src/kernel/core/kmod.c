#include <kernel/kmod.h>
#include <kernel/driver.h>
#include <kernel/device.h>
#include <kernel/smp.h>
#include <driver/storage/block_dev.h>
#include <driver/storage/partition.h>
#include <file_system/ipo_fs.h>
#include <memory/kmalloc.h>
#include <system/timer.h>
#include <ioport.h>
#include <string.h>
#include <stdio.h>

static kmod_t    *s_kmod_head = NULL;
static spinlock_t s_kmod_lock = {0};
static uint32_t   s_next_kmod_id = 1;
static bool       s_kmod_initialized = false;

const kernel_sym_entry_t *kmod_get_symbols(size_t *count) {
    if (&s_kernel_syms_count && count) *count = s_kernel_syms_count;
    else if (count) *count = 0;
    return s_kernel_syms;
}

void *kmod_resolve_symbol(const char *name) {
    if (!name || !*name) return NULL;
    if (!s_kernel_syms || !&s_kernel_syms_count || s_kernel_syms_count == 0) return NULL;

    /* Binary search across alphabetically sorted kernel symbol table */
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
    s_next_kmod_id = 1;
    s_kmod_initialized = true;
    printf("[kmod] Dynamic Driver Subsystem initialized (ABI %04x, %u kernel symbols exported).\n",
                  KMOD_ABI_VERSION, &s_kernel_syms_count ? (uint32_t)s_kernel_syms_count : 0);
    return 0;
}

/* Duplicate a (possibly non-terminated) fixed-width header field into a dynamic string */
static char *kmod_dup_field(const char *src, size_t max) {
    size_t n = 0;
    while (n < max && src[n]) n++;
    char *d = (char *)kmalloc(n + 1);
    if (!d) return NULL;
    memcpy(d, src, n);
    d[n] = 0;
    return d;
}

static void kmod_free_strings(kmod_t *m) {
    if (m->name) kfree(m->name);
    if (m->author) kfree(m->author);
    if (m->version) kfree(m->version);
    if (m->description) kfree(m->description);
    m->name = m->author = m->version = m->description = NULL;
}

int kmod_load_buffer(const void *buf, size_t size, kmod_t **out_mod) {
    if (!buf || size < sizeof(kmod_raw_header_t)) return -1;
    if (!s_kmod_initialized) kmod_init();

    const kmod_raw_header_t *raw_hdr = (const kmod_raw_header_t *)buf;
    kmod_header_t dyn_hdr;
    kmod_header_init(&dyn_hdr);
    kmod_header_from_raw(&dyn_hdr, raw_hdr);
    const kmod_header_t *hdr = &dyn_hdr;

    /* Validate Magic */
    if (hdr->magic != KMOD_MAGIC) {
        serial_printf("[kmod] Invalid magic: 0x%08x (expected 0x%08x)\n", hdr->magic, KMOD_MAGIC);
        return -2;
    }

    /* Validate ABI Version */
    if (hdr->abi_version != KMOD_ABI_VERSION) {
        serial_printf("[kmod] ABI mismatch: 0x%04x (expected 0x%04x)\n", hdr->abi_version, KMOD_ABI_VERSION);
        return -3;
    }

    /* Validate Payload Checksum */
    size_t payload_len = size - sizeof(kmod_raw_header_t);
    if (payload_len > 0 && hdr->checksum != 0) {
        uint32_t calc_crc = partition_crc32((const uint8_t *)buf + sizeof(kmod_raw_header_t), payload_len);
        if (calc_crc != hdr->checksum) {
            serial_printf("[kmod] Payload checksum mismatch: calc 0x%08x vs hdr 0x%08x\n",
                          calc_crc, hdr->checksum);
            return -4;
        }
    }

    /* Check if already loaded by name */
    if (kmod_find_by_name(hdr->name) != NULL) {
        serial_printf("[kmod] Module '%s' is already loaded.\n", hdr->name);
        return -5;
    }

    /* Allocate memory for code and data image */
    size_t image_sz = hdr->text_size + hdr->data_size + hdr->bss_size;
    if (image_sz == 0) image_sz = 64;

    uint8_t *image = (uint8_t *)kmalloc(image_sz);
    if (!image) {
        serial_printf("[kmod] Failed to allocate %u bytes for module image.\n", image_sz);
        return -6;
    }
    memset(image, 0, image_sz);

    /* Copy payload into allocated image */
    const uint8_t *payload = (const uint8_t *)buf + sizeof(kmod_raw_header_t);
    size_t copy_bytes = (payload_len < (hdr->text_size + hdr->data_size)) ?
                         payload_len : (hdr->text_size + hdr->data_size);
    if (copy_bytes > 0) {
        memcpy(image, payload, copy_bytes);
    }

    /* Process relocations if present */
    if (hdr->reloc_count > 0 && hdr->reloc_offset < size) {
        const kmod_reloc_t   *relocs   = (const kmod_reloc_t *)((const uint8_t *)buf + hdr->reloc_offset);
        const kmod_raw_sym_t *raw_syms = (const kmod_raw_sym_t *)((const uint8_t *)buf + hdr->sym_tab_offset);

        kmod_sym_t *syms = NULL;
        if (hdr->sym_tab_count > 0 && hdr->sym_tab_offset < size) {
            syms = (kmod_sym_t *)kmalloc(hdr->sym_tab_count * sizeof(kmod_sym_t));
            if (!syms) {
                kfree(image);
                return -7;
            }
            for (uint32_t s = 0; s < hdr->sym_tab_count; s++) {
                kmod_sym_init(&syms[s]);
                kmod_sym_from_raw(&syms[s], &raw_syms[s]);
            }
        }

        bool reloc_ok = true;
        for (uint32_t r = 0; r < hdr->reloc_count; r++) {
            const kmod_reloc_t *rel = &relocs[r];
            if (rel->offset + 4 > image_sz) continue;
            if (rel->sym_index >= hdr->sym_tab_count || !syms) continue;

            const char *sym_name = syms[rel->sym_index].name;
            void *target_addr = kmod_resolve_symbol(sym_name);
            if (!target_addr) {
                serial_printf("[kmod] Unresolved symbol '%s' for module '%s'\n",
                              sym_name ? sym_name : "unknown", hdr->name);
                reloc_ok = false;
                break;
            }

            uint32_t *patch_loc = (uint32_t *)(image + rel->offset);
            if (rel->type == KMOD_RELOC_ABS32) {
                *patch_loc += (uint32_t)target_addr;
            } else if (rel->type == KMOD_RELOC_REL32) {
                *patch_loc += (uint32_t)target_addr - ((uint32_t)patch_loc + 4);
            }
        }

        if (syms) {
            for (uint32_t s = 0; s < hdr->sym_tab_count; s++) {
                kmod_sym_free(&syms[s]);
            }
            kfree(syms);
        }

        if (!reloc_ok) {
            kfree(image);
            return -7;
        }
    }

    /* Allocate runtime descriptor */
    kmod_t *mod = (kmod_t *)kmalloc(sizeof(kmod_t));
    if (!mod) {
        kfree(image);
        return -8;
    }
    memset(mod, 0, sizeof(kmod_t));

    mod->id = s_next_kmod_id++;
    mod->name = kmod_dup_field(hdr->name, strlen(hdr->name));
    mod->author = kmod_dup_field(hdr->author, strlen(hdr->author));
    mod->version = kmod_dup_field(hdr->version, strlen(hdr->version));
    mod->description = kmod_dup_field(hdr->description, strlen(hdr->description));
    if (!mod->name || !mod->author || !mod->version || !mod->description) {
        kmod_free_strings(mod);
        kfree(image);
        kfree(mod);
        return -8;
    }
    mod->abi_version = hdr->abi_version;
    mod->flags = KMOD_FLAG_LOADED;
    mod->image_base = image;
    mod->image_size = image_sz;

    if (hdr->init_offset < image_sz) {
        mod->init = (int (*)(void))(image + hdr->init_offset);
    }
    if (hdr->cleanup_offset < image_sz) {
        mod->cleanup = (int (*)(void))(image + hdr->cleanup_offset);
    }

    /* Call module init() callback */
    if (mod->init != NULL) {
        int init_ret = mod->init();
        if (init_ret < 0) {
            serial_printf("[kmod] Module '%s' init() failed with code %d\n", mod->name, init_ret);
            kfree(image);
            kmod_free_strings(mod);
            kfree(mod);
            kmod_header_free(&dyn_hdr);
            return init_ret;
        }
    }

    mod->flags |= KMOD_FLAG_ACTIVE;

    /* Register with core driver subsystem */
    driver_t drv;
    memset(&drv, 0, sizeof(drv));
    drv.name = mod->name;
    drv.description = mod->description;
    drv.flags = DRIVER_FLAG_KERNEL | DRIVER_FLAG_ACTIVE;
    driver_register(&drv);

    /* Insert into module list */
    spin_lock(&s_kmod_lock);
    mod->next = s_kmod_head;
    s_kmod_head = mod;
    spin_unlock(&s_kmod_lock);

    serial_printf("[kmod] Loaded module '%s' (v%s by %s, id=%u, %u bytes @ %p)\n",
                  mod->name, mod->version, mod->author, mod->id, mod->image_size, mod->image_base);

    kmod_header_free(&dyn_hdr);
    if (out_mod) *out_mod = mod;
    return 0;
}

int kmod_load_file(const char *path, kmod_t **out_mod) {
    if (!path || !*path) return -1;

    struct ipo_inode st;
    if (!ipo_fs_stat(path, &st)) {
        serial_printf("[kmod] File not found: '%s'\n", path);
        return -2;
    }

    uint32_t file_sz = (uint32_t)st.size;
    if (file_sz < sizeof(kmod_raw_header_t) || file_sz > (512 * 1024)) {
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

    int ret = kmod_load_buffer(buf, (size_t)file_sz, out_mod);
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

            /* Call cleanup() callback */
            if (curr->cleanup != NULL) {
                curr->cleanup();
            }

            /* Unregister from core driver registry */
            driver_unregister(curr->name);

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

const kmod_t *kmod_get_list(void) {
    return s_kmod_head;
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
    printf("  ABI Version: 0x%04x  Loaded Modules: %u\n", KMOD_ABI_VERSION, kmod_count());

    spin_lock(&s_kmod_lock);
    kmod_t *curr = s_kmod_head;
    if (!curr) {
        printf("  No dynamic modules loaded.\n");
        spin_unlock(&s_kmod_lock);
        return;
    }

    printf("  ID   Name             Version  Author          Base Addr   Size    Status\n");
    printf("  --------------------------------------------------------------------------\n");

    while (curr) {
        printf("  %-4u %-16s %-8s %-15s %p  %-7u %s\n",
               curr->id,
               curr->name,
               (curr->version && curr->version[0]) ? curr->version : "1.0",
               (curr->author && curr->author[0]) ? curr->author : "Kernel",
               curr->image_base,
               (uint32_t)curr->image_size,
               (curr->flags & KMOD_FLAG_ACTIVE) ? "ACTIVE" : "LOADED");
        curr = curr->next;
    }
    spin_unlock(&s_kmod_lock);
}

int kmod_device_firmware_probe(const void *rom_data, size_t rom_size, const char *dev_name) {
    if (!rom_data || rom_size < sizeof(kmod_raw_header_t)) return -1;

    const uint8_t *p = (const uint8_t *)rom_data;
    for (size_t off = 0; off + sizeof(kmod_raw_header_t) <= rom_size; off += 16) {
        uint32_t magic = *(const uint32_t *)(p + off);
        if (magic == KMOD_MAGIC) {
            serial_printf("[kmod] Detected device firmware driver in ROM at offset 0x%x for '%s'\n",
                          off, dev_name ? dev_name : "unknown");
            kmod_t *mod = NULL;
            return kmod_load_buffer(p + off, rom_size - off, &mod);
        }
    }
    return 0;
}

void kmod_sym_init(kmod_sym_t *sym) {
    if (!sym) return;
    memset(sym, 0, sizeof(*sym));
}

void kmod_sym_free(kmod_sym_t *sym) {
    if (!sym) return;
    if (sym->name) {
        kfree(sym->name);
        sym->name = NULL;
    }
    sym->name_len = 0;
    sym->name_cap = 0;
    sym->address = 0;
}

int kmod_sym_set_name(kmod_sym_t *sym, const char *name) {
    if (!sym) return -1;
    if (sym->name) {
        kfree(sym->name);
        sym->name = NULL;
    }
    sym->name_len = 0;
    sym->name_cap = 0;
    if (!name) return 0;

    size_t len = strlen(name);
    size_t cap = len + 1;
    char *buf = (char *)kmalloc(cap);
    if (!buf) return -1;
    memcpy(buf, name, len);
    buf[len] = '\0';
    sym->name = buf;
    sym->name_len = len;
    sym->name_cap = cap;
    return 0;
}

int kmod_sym_copy(kmod_sym_t *dst, const kmod_sym_t *src) {
    if (!dst) return -1;
    kmod_sym_free(dst);
    if (!src) return 0;
    kmod_sym_set_name(dst, src->name);
    dst->address = src->address;
    return 0;
}

void kmod_sym_from_raw(kmod_sym_t *dst, const kmod_raw_sym_t *raw) {
    if (!dst || !raw) return;
    char name_buf[33];
    memset(name_buf, 0, sizeof(name_buf));
    strncpy(name_buf, raw->name, 32);
    kmod_sym_set_name(dst, name_buf);
    dst->address = raw->address;
}

void kmod_sym_to_raw(const kmod_sym_t *src, kmod_raw_sym_t *raw) {
    if (!src || !raw) return;
    memset(raw, 0, sizeof(*raw));
    if (src->name) strncpy(raw->name, src->name, 31);
    raw->address = src->address;
}

void kmod_header_init(kmod_header_t *h) {
    if (h) memset(h, 0, sizeof(*h));
}

void kmod_header_free(kmod_header_t *h) {
    if (!h) return;
    if (h->name) kfree(h->name);
    if (h->author) kfree(h->author);
    if (h->version) kfree(h->version);
    if (h->description) kfree(h->description);
    memset(h, 0, sizeof(*h));
}

static int kmod_hdr_str_set(char **dst, size_t *len, size_t *cap, const char *src) {
    if (!src) src = "";
    size_t slen = strlen(src);
    if (slen + 1 > *cap || !*dst) {
        char *nb = (char *)krealloc(*dst, slen + 1);
        if (!nb) return -1;
        *dst = nb;
        *cap = slen + 1;
    }
    memcpy(*dst, src, slen + 1);
    *len = slen;
    return 0;
}

int kmod_header_set_name(kmod_header_t *h, const char *name) {
    if (!h) return -1;
    return kmod_hdr_str_set(&h->name, &h->name_len, &h->name_cap, name);
}

int kmod_header_set_author(kmod_header_t *h, const char *author) {
    if (!h) return -1;
    return kmod_hdr_str_set(&h->author, &h->author_len, &h->author_cap, author);
}

int kmod_header_set_version(kmod_header_t *h, const char *version) {
    if (!h) return -1;
    return kmod_hdr_str_set(&h->version, &h->version_len, &h->version_cap, version);
}

int kmod_header_set_description(kmod_header_t *h, const char *desc) {
    if (!h) return -1;
    return kmod_hdr_str_set(&h->description, &h->description_len, &h->description_cap, desc);
}

static void kmod_raw_str_copy(int (*setter)(kmod_header_t *, const char *),
                              kmod_header_t *dst,
                              const char *raw_str,
                              size_t max_len) {
    size_t len = 0;
    while (len < max_len && raw_str[len] != '\0') len++;
    char *tmp = (char *)kmalloc(len + 1u);
    if (!tmp) return;
    memcpy(tmp, raw_str, len);
    tmp[len] = '\0';
    setter(dst, tmp);
    kfree(tmp);
}

void kmod_header_from_raw(kmod_header_t *dst, const kmod_raw_header_t *raw) {
    if (!dst || !raw) return;
    dst->magic = raw->magic;
    dst->abi_version = raw->abi_version;
    dst->flags = raw->flags;

    kmod_raw_str_copy(kmod_header_set_name, dst, raw->name, sizeof(raw->name));
    kmod_raw_str_copy(kmod_header_set_author, dst, raw->author, sizeof(raw->author));
    kmod_raw_str_copy(kmod_header_set_version, dst, raw->version, sizeof(raw->version));
    kmod_raw_str_copy(kmod_header_set_description, dst, raw->description, sizeof(raw->description));

    dst->init_offset = raw->init_offset;
    dst->cleanup_offset = raw->cleanup_offset;
    dst->text_size = raw->text_size;
    dst->data_size = raw->data_size;
    dst->bss_size = raw->bss_size;
    dst->sym_tab_offset = raw->sym_tab_offset;
    dst->sym_tab_count = raw->sym_tab_count;
    dst->reloc_offset = raw->reloc_offset;
    dst->reloc_count = raw->reloc_count;
    dst->checksum = raw->checksum;
}

void kmod_header_to_raw(const kmod_header_t *src, kmod_raw_header_t *raw) {
    if (!src || !raw) return;
    memset(raw, 0, sizeof(*raw));
    raw->magic = src->magic;
    raw->abi_version = src->abi_version;
    raw->flags = src->flags;
    if (src->name) strncpy(raw->name, src->name, 31);
    if (src->author) strncpy(raw->author, src->author, 31);
    if (src->version) strncpy(raw->version, src->version, 15);
    if (src->description) strncpy(raw->description, src->description, 63);
    raw->init_offset = src->init_offset;
    raw->cleanup_offset = src->cleanup_offset;
    raw->text_size = src->text_size;
    raw->data_size = src->data_size;
    raw->bss_size = src->bss_size;
    raw->sym_tab_offset = src->sym_tab_offset;
    raw->sym_tab_count = src->sym_tab_count;
    raw->reloc_offset = src->reloc_offset;
    raw->reloc_count = src->reloc_count;
    raw->checksum = src->checksum;
}
