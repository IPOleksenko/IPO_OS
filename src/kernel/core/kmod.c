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
    printf("[kmod] Driver Subsystem initialized (ABI %04x, %u kernel symbols exported).\n",
                  KMOD_ABI_VERSION, &s_kernel_syms_count ? (uint32_t)s_kernel_syms_count : 0);
    return 0;
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

    int rc = -1;
    uint8_t *image = NULL;
    kmod_t *mod = NULL;
    kmod_sym_t *syms = NULL;

    if (!hdr->name || !hdr->author || !hdr->version || !hdr->description) {
        rc = -6;
        goto fail;
    }

    if (hdr->magic != KMOD_MAGIC) {
        serial_printf("[kmod] Invalid magic: 0x%08x (expected 0x%08x)\n", hdr->magic, KMOD_MAGIC);
        rc = -2;
        goto fail;
    }

    if (hdr->abi_version != KMOD_ABI_VERSION) {
        serial_printf("[kmod] ABI mismatch: 0x%04x (expected 0x%04x)\n", hdr->abi_version, KMOD_ABI_VERSION);
        rc = -3;
        goto fail;
    }

    size_t payload_len = size - sizeof(kmod_raw_header_t);
    if (payload_len > 0 && hdr->checksum != 0) {
        uint32_t calc_crc = partition_crc32((const uint8_t *)buf + sizeof(kmod_raw_header_t), payload_len);
        if (calc_crc != hdr->checksum) {
            serial_printf("[kmod] Payload checksum mismatch: calc 0x%08x vs hdr 0x%08x\n",
                          calc_crc, hdr->checksum);
            rc = -4;
            goto fail;
        }
    }

    if (kmod_find_by_name(hdr->name) != NULL) {
        serial_printf("[kmod] Module '%s' is already loaded.\n", hdr->name);
        rc = -5;
        goto fail;
    }

    uint64_t raw_image_sz = (uint64_t)hdr->text_size + hdr->data_size + hdr->bss_size;
    if (raw_image_sz > 16u * 1024u * 1024u) {
        rc = -6;
        goto fail;
    }
    size_t image_sz = (raw_image_sz == 0u) ? 64u : (size_t)raw_image_sz;

    image = (uint8_t *)kmalloc(image_sz);
    if (!image) {
        serial_printf("[kmod] Failed to allocate %u bytes for module image.\n", image_sz);
        rc = -6;
        goto fail;
    }
    memset(image, 0, image_sz);

    const uint8_t *payload = (const uint8_t *)buf + sizeof(kmod_raw_header_t);
    uint64_t text_data_sz = (uint64_t)hdr->text_size + hdr->data_size;
    size_t copy_bytes = (payload_len < text_data_sz) ? payload_len : (size_t)text_data_sz;
    if (copy_bytes > 0) {
        memcpy(image, payload, copy_bytes);
    }

    if (hdr->reloc_count > 0) {
        if (hdr->reloc_offset >= size ||
            (size - hdr->reloc_offset) / sizeof(kmod_reloc_t) < hdr->reloc_count) {
            rc = -7;
            goto fail;
        }
        const kmod_reloc_t *relocs = (const kmod_reloc_t *)((const uint8_t *)buf + hdr->reloc_offset);

        if (hdr->sym_tab_count > 0) {
            if (hdr->sym_tab_offset >= size ||
                (size - hdr->sym_tab_offset) / sizeof(kmod_raw_sym_t) < hdr->sym_tab_count) {
                rc = -7;
                goto fail;
            }
            const kmod_raw_sym_t *raw_syms = (const kmod_raw_sym_t *)((const uint8_t *)buf + hdr->sym_tab_offset);
            syms = (kmod_sym_t *)kmalloc(hdr->sym_tab_count * sizeof(kmod_sym_t));
            if (!syms) {
                rc = -7;
                goto fail;
            }
            for (uint32_t s = 0; s < hdr->sym_tab_count; s++) {
                kmod_sym_init(&syms[s]);
                kmod_sym_from_raw(&syms[s], &raw_syms[s]);
            }
        }

        bool reloc_ok = true;
        for (uint32_t r = 0; r < hdr->reloc_count; r++) {
            const kmod_reloc_t *rel = &relocs[r];
            if (rel->offset > image_sz || image_sz - rel->offset < 4u ||
                rel->sym_index >= hdr->sym_tab_count || !syms) {
                reloc_ok = false;
                break;
            }

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
                *patch_loc += (uint32_t)(uintptr_t)target_addr;
            } else if (rel->type == KMOD_RELOC_REL32) {
                *patch_loc += (uint32_t)(uintptr_t)target_addr - ((uint32_t)(uintptr_t)patch_loc + 4u);
            } else {
                reloc_ok = false;
                break;
            }
        }

        if (syms) {
            for (uint32_t s = 0; s < hdr->sym_tab_count; s++) {
                kmod_sym_free(&syms[s]);
            }
            kfree(syms);
            syms = NULL;
        }

        if (!reloc_ok) {
            rc = -7;
            goto fail;
        }
    }

    mod = (kmod_t *)kmalloc(sizeof(kmod_t));
    if (!mod) {
        rc = -8;
        goto fail;
    }
    memset(mod, 0, sizeof(kmod_t));

    mod->id = s_next_kmod_id++;
    mod->name = dyn_hdr.name; dyn_hdr.name = NULL;
    mod->author = dyn_hdr.author; dyn_hdr.author = NULL;
    mod->version = dyn_hdr.version; dyn_hdr.version = NULL;
    mod->description = dyn_hdr.description; dyn_hdr.description = NULL;
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

    if (mod->init != NULL) {
        int init_ret = mod->init();
        if (init_ret < 0) {
            serial_printf("[kmod] Module '%s' init() failed with code %d\n", mod->name, init_ret);
            rc = init_ret;
            goto fail;
        }
    }

    mod->flags |= KMOD_FLAG_ACTIVE;

    if (driver_find(mod->name) == NULL) {
        driver_t drv;
        memset(&drv, 0, sizeof(drv));
        drv.name = mod->name;
        drv.description = mod->description;
        drv.flags = DRIVER_FLAG_ACTIVE;
        driver_register(&drv);
    }

    spin_lock(&s_kmod_lock);
    mod->next = s_kmod_head;
    s_kmod_head = mod;
    spin_unlock(&s_kmod_lock);

    serial_printf("[kmod] Loaded module '%s' (v%s by %s, id=%u, %u bytes @ %p)\n",
                  mod->name, mod->version, mod->author, mod->id, mod->image_size, mod->image_base);

    kmod_header_free(&dyn_hdr);
    if (out_mod) *out_mod = mod;
    return 0;

fail:
    if (mod) {
        kmod_free_strings(mod);
        kfree(mod);
    }
    if (image) kfree(image);
    kmod_header_free(&dyn_hdr);
    return rc;
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

            /* Call cleanup() callback before removing the module driver */
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
    printf("  ABI Version: 0x%04x  Loaded Modules: %u\n", KMOD_ABI_VERSION, kmod_count());

    spin_lock(&s_kmod_lock);
    kmod_t *curr = s_kmod_head;
    if (!curr) {
        printf("  No modules loaded.\n");
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

static int kmod_str_set_bounded(char **dst, size_t *len, size_t *cap,
                                const char *src, size_t max_len) {
    if (!src) src = "";
    size_t slen = 0;
    while (slen < max_len && src[slen] != '\0') slen++;
    if (slen + 1u > *cap || !*dst) {
        char *nb = (char *)krealloc(*dst, slen + 1u);
        if (!nb) return -1;
        *dst = nb;
        *cap = slen + 1u;
    }
    if (slen > 0u) memcpy(*dst, src, slen);
    (*dst)[slen] = '\0';
    *len = slen;
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
    if (!name) {
        if (sym->name) {
            kfree(sym->name);
            sym->name = NULL;
        }
        sym->name_len = 0;
        sym->name_cap = 0;
        return 0;
    }
    return kmod_str_set_bounded(&sym->name, &sym->name_len, &sym->name_cap, name, strlen(name));
}

void kmod_sym_from_raw(kmod_sym_t *dst, const kmod_raw_sym_t *raw) {
    if (!dst || !raw) return;
    kmod_str_set_bounded(&dst->name, &dst->name_len, &dst->name_cap, raw->name, sizeof(raw->name));
    dst->address = raw->address;
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

int kmod_header_set_name(kmod_header_t *h, const char *name) {
    if (!h) return -1;
    return kmod_str_set_bounded(&h->name, &h->name_len, &h->name_cap, name, name ? strlen(name) : 0u);
}

int kmod_header_set_author(kmod_header_t *h, const char *author) {
    if (!h) return -1;
    return kmod_str_set_bounded(&h->author, &h->author_len, &h->author_cap, author, author ? strlen(author) : 0u);
}

int kmod_header_set_version(kmod_header_t *h, const char *version) {
    if (!h) return -1;
    return kmod_str_set_bounded(&h->version, &h->version_len, &h->version_cap, version, version ? strlen(version) : 0u);
}

int kmod_header_set_description(kmod_header_t *h, const char *desc) {
    if (!h) return -1;
    return kmod_str_set_bounded(&h->description, &h->description_len, &h->description_cap, desc, desc ? strlen(desc) : 0u);
}

void kmod_header_from_raw(kmod_header_t *dst, const kmod_raw_header_t *raw) {
    if (!dst || !raw) return;
    dst->magic = raw->magic;
    dst->abi_version = raw->abi_version;
    dst->flags = raw->flags;

    kmod_str_set_bounded(&dst->name, &dst->name_len, &dst->name_cap, raw->name, sizeof(raw->name));
    kmod_str_set_bounded(&dst->author, &dst->author_len, &dst->author_cap, raw->author, sizeof(raw->author));
    kmod_str_set_bounded(&dst->version, &dst->version_len, &dst->version_cap, raw->version, sizeof(raw->version));
    kmod_str_set_bounded(&dst->description, &dst->description_len, &dst->description_cap, raw->description, sizeof(raw->description));

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
