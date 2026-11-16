#ifndef _KMOD_H
#define _KMOD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define KMOD_FLAG_BUILTIN   (1 << 0)
#define KMOD_FLAG_LOADED    (1 << 1)
#define KMOD_FLAG_ACTIVE    (1 << 2)

typedef struct {
    const char *name;
    void       *addr;
} kernel_sym_entry_t;

extern const kernel_sym_entry_t s_kernel_syms[] __attribute__((weak));
extern const size_t             s_kernel_syms_count __attribute__((weak));

typedef void *(*kmod_sym_resolve_fn_t)(const char *name);

/* Optional metadata filled by a bare-metal module during entry */
typedef struct {
    const char *name;
    const char *author;
    const char *description;
    int       (*cleanup)(void);
    uint32_t    meta_flags;
    uint32_t    abi_version;
} kmod_init_meta_t;

typedef int (*kmod_entry_fn_t)(kmod_sym_resolve_fn_t resolve_sym,
                                 kmod_init_meta_t *out_meta);

typedef struct kmod {
    uint32_t       id;
    char          *name;
    char          *author;
    char          *description;
    uint32_t       flags;
    void          *image_base;
    size_t         image_size;
    int          (*init)(void);
    int          (*cleanup)(void);
    uint32_t       ref_count;
    struct kmod   *next;
} kmod_t;

/* Custom module format loader hook (bare-metal only; none registered by default) */
typedef int (*kmod_format_loader_fn_t)(const char *default_name,
                                       const void *buf, size_t size,
                                       kmod_t **out_mod);

int  kmod_init(void);
void *kmod_resolve_symbol(const char *name);
const kernel_sym_entry_t *kmod_get_symbols(size_t *count);

int  kmod_register_format(const char *name, const uint8_t *magic,
                          uint32_t magic_len, uint32_t offset,
                          kmod_format_loader_fn_t loader);
int  kmod_unregister_format(const char *name);

int  kmod_load_buffer(const void *buf, size_t size, kmod_t **out_mod);
int  kmod_load_buffer_named(const char *default_name, const void *buf,
                            size_t size, kmod_t **out_mod);
int  kmod_load_file(const char *path, kmod_t **out_mod);
int  kmod_unload(uint32_t id);
int  kmod_unload_by_name(const char *name);

kmod_t *kmod_find_by_id(uint32_t id);
kmod_t *kmod_find_by_name(const char *name);
uint32_t kmod_count(void);

void kmod_print_list(void);

#endif /* _KMOD_H */
