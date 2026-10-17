#ifndef _KMOD_H
#define _KMOD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define KMOD_MAGIC       0x444D4B49 /* "IKMD" in little-endian */
#define KMOD_ABI_VERSION 0x0100     /* 1.0 */

#define KMOD_FLAG_BUILTIN   (1 << 0)
#define KMOD_FLAG_LOADED    (1 << 1)
#define KMOD_FLAG_ACTIVE    (1 << 2)

#define KMOD_RELOC_ABS32    1
#define KMOD_RELOC_REL32    2

/* Relocation record */
typedef struct {
    uint32_t offset;               /* Offset in text/data to patch */
    uint32_t sym_index;            /* Index in module's symbol table */
    uint8_t  type;                 /* KMOD_RELOC_* */
} __attribute__((packed)) kmod_reloc_t;

/* Packed On-Disk/File Symbol Reference (36 bytes) */
typedef struct {
    char     name[32];
    uint32_t address;
} __attribute__((packed)) kmod_raw_sym_t;

/* Dynamic Symbol Reference (self-expanding in memory) */
typedef struct {
    char     *name;
    size_t   name_len;
    size_t   name_cap;
    uint32_t address;
} kmod_sym_t;

/* Dynamic kmod_sym_t lifecycle methods */
void kmod_sym_init(kmod_sym_t *sym);
void kmod_sym_free(kmod_sym_t *sym);
int  kmod_sym_set_name(kmod_sym_t *sym, const char *name);
int  kmod_sym_copy(kmod_sym_t *dst, const kmod_sym_t *src);
void kmod_sym_from_raw(kmod_sym_t *dst, const kmod_raw_sym_t *raw);
void kmod_sym_to_raw(const kmod_sym_t *src, kmod_raw_sym_t *raw);

/* Exported Kernel Symbols Table */
typedef struct {
    const char *name;
    void       *addr;
} kernel_sym_entry_t;

extern const kernel_sym_entry_t s_kernel_syms[] __attribute__((weak));
extern const size_t             s_kernel_syms_count __attribute__((weak));

/* Packed On-Disk File Header for .kmd files */
typedef struct {
    uint32_t magic;                /* KMOD_MAGIC */
    uint16_t abi_version;          /* KMOD_ABI_VERSION */
    uint16_t flags;
    char     name[32];
    char     author[32];
    char     version[16];
    char     description[64];
    uint32_t init_offset;          /* Offset of init function from text start */
    uint32_t cleanup_offset;       /* Offset of cleanup function from text start */
    uint32_t text_size;
    uint32_t data_size;
    uint32_t bss_size;
    uint32_t sym_tab_offset;
    uint32_t sym_tab_count;
    uint32_t reloc_offset;
    uint32_t reloc_count;
    uint32_t checksum;             /* CRC32 of payload */
} __attribute__((packed)) kmod_raw_header_t;

/* Dynamic In-Memory Module Header (self-expanding, no fixed array limits) */
typedef struct {
    uint32_t magic;
    uint16_t abi_version;
    uint16_t flags;
    char     *name;
    size_t   name_len;
    size_t   name_cap;
    char     *author;
    size_t   author_len;
    size_t   author_cap;
    char     *version;
    size_t   version_len;
    size_t   version_cap;
    char     *description;
    size_t   description_len;
    size_t   description_cap;
    uint32_t init_offset;
    uint32_t cleanup_offset;
    uint32_t text_size;
    uint32_t data_size;
    uint32_t bss_size;
    uint32_t sym_tab_offset;
    uint32_t sym_tab_count;
    uint32_t reloc_offset;
    uint32_t reloc_count;
    uint32_t checksum;
} kmod_header_t;

void kmod_header_init(kmod_header_t *h);
void kmod_header_free(kmod_header_t *h);
int  kmod_header_set_name(kmod_header_t *h, const char *name);
int  kmod_header_set_author(kmod_header_t *h, const char *author);
int  kmod_header_set_version(kmod_header_t *h, const char *version);
int  kmod_header_set_description(kmod_header_t *h, const char *desc);
void kmod_header_from_raw(kmod_header_t *dst, const kmod_raw_header_t *raw);
void kmod_header_to_raw(const kmod_header_t *src, kmod_raw_header_t *raw);

/* Runtime module descriptor */
typedef struct kmod {
    uint32_t       id;
    char          *name;          /* Dynamically allocated strings */
    char          *author;
    char          *version;
    char          *description;
    uint16_t       abi_version;
    uint32_t       flags;
    void          *image_base;
    size_t         image_size;
    int          (*init)(void);
    int          (*cleanup)(void);
    uint32_t       ref_count;
    struct kmod   *next;
} kmod_t;

/* Kernel Module Subsystem API */
int  kmod_init(void);
void *kmod_resolve_symbol(const char *name);
const kernel_sym_entry_t *kmod_get_symbols(size_t *count);

int  kmod_load_buffer(const void *buf, size_t size, kmod_t **out_mod);
int  kmod_load_file(const char *path, kmod_t **out_mod);
int  kmod_unload(uint32_t id);
int  kmod_unload_by_name(const char *name);

kmod_t *kmod_find_by_id(uint32_t id);
kmod_t *kmod_find_by_name(const char *name);
const kmod_t *kmod_get_list(void);
uint32_t kmod_count(void);

void kmod_print_list(void);
int  kmod_device_firmware_probe(const void *rom_data, size_t rom_size, const char *dev_name);

#endif /* _KMOD_H */

