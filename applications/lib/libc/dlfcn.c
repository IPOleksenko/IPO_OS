#include <dlfcn.h>
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <math.h>
#include <syscall.h>

/* Maximum internal error message buffer */
static char s_dlerror_buf[256] = {0};
static int  s_has_dlerror = 0;

static void set_dlerror(const char *msg) {
    if (!msg) {
        s_dlerror_buf[0] = 0;
        s_has_dlerror = 0;
        return;
    }
    strncpy(s_dlerror_buf, msg, sizeof(s_dlerror_buf) - 1);
    s_dlerror_buf[sizeof(s_dlerror_buf) - 1] = 0;
    s_has_dlerror = 1;
}

/* User registered symbols list (dlregister_symbol) */
typedef struct registered_sym {
    char name[64];
    void *addr;
    struct registered_sym *next;
} registered_sym_t;

static registered_sym_t *s_custom_syms = NULL;

/* Loaded Dynamic Modules descriptor */
typedef struct dl_module {
    char *path;
    uint16_t elf_type; /* ET_REL or ET_DYN */
    int ref_count;
    int flags;
    void *image_base;
    uint32_t image_size;

    /* For ET_REL sections */
    void **section_addrs;
    uint32_t *section_sizes;
    uint32_t num_sections;

    /* For ET_REL COMMON symbols */
    uint32_t *common_addrs;

    /* Exported symbols and strings for runtime dlsym() */
    Elf32_Sym *symtab;
    uint32_t num_syms;
    char *strtab;
    uint32_t strtab_size;

    /* Constructors and Destructors */
    void (*init_fn)(void);
    void (*fini_fn)(void);

    struct dl_module *next;
} dl_module_t;

static dl_module_t *s_modules = NULL;

/* Built-in standard symbol table */
typedef struct {
    const char *name;
    void *addr;
} builtin_sym_t;

#define BUILTIN_SYM(fn) { #fn, (void *)&fn }

extern uintptr_t __stack_chk_guard;
extern void __stack_chk_fail(void);

static const builtin_sym_t s_builtin_syms[] = {
    /* stdio */
    BUILTIN_SYM(printf),
    BUILTIN_SYM(fprintf),
    BUILTIN_SYM(sprintf),
    BUILTIN_SYM(snprintf),
    BUILTIN_SYM(vprintf),
    BUILTIN_SYM(vfprintf),
    BUILTIN_SYM(vsprintf),
    BUILTIN_SYM(vsnprintf),
    BUILTIN_SYM(puts),
    BUILTIN_SYM(putchar),
    BUILTIN_SYM(getchar),
    BUILTIN_SYM(fputc),
    BUILTIN_SYM(fputs),
    BUILTIN_SYM(fgetc),
    BUILTIN_SYM(fgets),
    BUILTIN_SYM(fopen),
    BUILTIN_SYM(fclose),
    BUILTIN_SYM(fread),
    BUILTIN_SYM(fwrite),
    BUILTIN_SYM(fseek),
    BUILTIN_SYM(ftell),
    BUILTIN_SYM(rewind),
    BUILTIN_SYM(fflush),
    BUILTIN_SYM(feof),
    BUILTIN_SYM(ferror),
    BUILTIN_SYM(clearerr),
    BUILTIN_SYM(perror),
    BUILTIN_SYM(remove),
    BUILTIN_SYM(rename),
    BUILTIN_SYM(stdin),
    BUILTIN_SYM(stdout),
    BUILTIN_SYM(stderr),

    /* stdlib */
    BUILTIN_SYM(malloc),
    BUILTIN_SYM(free),
    BUILTIN_SYM(calloc),
    BUILTIN_SYM(realloc),
    BUILTIN_SYM(exit),
    BUILTIN_SYM(abort),
    BUILTIN_SYM(atexit),
    BUILTIN_SYM(atoi),
    BUILTIN_SYM(atol),
    BUILTIN_SYM(strtol),
    BUILTIN_SYM(strtoul),
    BUILTIN_SYM(rand),
    BUILTIN_SYM(srand),
    BUILTIN_SYM(qsort),
    BUILTIN_SYM(bsearch),
    BUILTIN_SYM(abs),
    BUILTIN_SYM(getenv),
    BUILTIN_SYM(setenv),

    /* string */
    BUILTIN_SYM(strlen),
    BUILTIN_SYM(strcpy),
    BUILTIN_SYM(strncpy),
    BUILTIN_SYM(strcat),
    BUILTIN_SYM(strncat),
    BUILTIN_SYM(strcmp),
    BUILTIN_SYM(strncmp),
    BUILTIN_SYM(strchr),
    BUILTIN_SYM(strrchr),
    BUILTIN_SYM(strstr),
    BUILTIN_SYM(strdup),
    BUILTIN_SYM(strtok),
    BUILTIN_SYM(strspn),
    BUILTIN_SYM(strcspn),
    BUILTIN_SYM(strpbrk),
    BUILTIN_SYM(memset),
    BUILTIN_SYM(memcpy),
    BUILTIN_SYM(memmove),
    BUILTIN_SYM(memcmp),
    BUILTIN_SYM(memchr),

    /* unistd & sys */
    BUILTIN_SYM(sleep),
    BUILTIN_SYM(usleep),
    BUILTIN_SYM(open),
    BUILTIN_SYM(close),
    BUILTIN_SYM(read),
    BUILTIN_SYM(write),
    BUILTIN_SYM(lseek),
    BUILTIN_SYM(stat),
    BUILTIN_SYM(fstat),
    BUILTIN_SYM(ipo_syscall),

    /* math */
    BUILTIN_SYM(sin),
    BUILTIN_SYM(cos),
    BUILTIN_SYM(tan),
    BUILTIN_SYM(sqrt),
    BUILTIN_SYM(pow),
    BUILTIN_SYM(fabs),
    BUILTIN_SYM(floor),
    BUILTIN_SYM(ceil),
    BUILTIN_SYM(exp),
    BUILTIN_SYM(log),

    /* dlfcn */
    BUILTIN_SYM(dlopen),
    BUILTIN_SYM(dlsym),
    BUILTIN_SYM(dlclose),
    BUILTIN_SYM(dlerror),
    BUILTIN_SYM(dladdr),
    BUILTIN_SYM(dlregister_symbol),

    /* compiler runtime */
    BUILTIN_SYM(__stack_chk_guard),
    BUILTIN_SYM(__stack_chk_fail),

    { NULL, NULL }
};

int dlregister_symbol(const char *name, void *addr) {
    if (!name || !*name || !addr) return -1;
    registered_sym_t *cur = s_custom_syms;
    while (cur) {
        if (strcmp(cur->name, name) == 0) {
            cur->addr = addr;
            return 0;
        }
        cur = cur->next;
    }
    registered_sym_t *node = (registered_sym_t *)malloc(sizeof(registered_sym_t));
    if (!node) return -1;
    strncpy(node->name, name, sizeof(node->name) - 1);
    node->name[sizeof(node->name) - 1] = 0;
    node->addr = addr;
    node->next = s_custom_syms;
    s_custom_syms = node;
    return 0;
}

static void *dlsym_in_module(dl_module_t *mod, const char *symbol) {
    if (!mod || !mod->symtab || !mod->strtab || !symbol) return NULL;
    for (uint32_t i = 0; i < mod->num_syms; i++) {
        Elf32_Sym *sym = &mod->symtab[i];
        if (sym->st_shndx == SHN_UNDEF) continue;
        uint8_t bind = ELF32_ST_BIND(sym->st_info);
        if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
        if (sym->st_name >= mod->strtab_size) continue;
        const char *name = mod->strtab + sym->st_name;
        if (strcmp(name, symbol) == 0) {
            if (mod->elf_type == ET_REL) {
                if (sym->st_shndx == SHN_ABS) {
                    return (void *)sym->st_value;
                } else if (sym->st_shndx == SHN_COMMON) {
                    return mod->common_addrs ? (void *)mod->common_addrs[i] : NULL;
                } else if (sym->st_shndx < mod->num_sections && mod->section_addrs[sym->st_shndx]) {
                    return (void *)((uint32_t)mod->section_addrs[sym->st_shndx] + sym->st_value);
                }
            } else if (mod->elf_type == ET_DYN) {
                return (void *)((uint32_t)mod->image_base + sym->st_value);
            }
        }
    }
    return NULL;
}

static void *dlsym_resolve_global(const char *name) {
    if (!name || !*name) return NULL;

    /* 1. Check custom registered symbols */
    registered_sym_t *cs = s_custom_syms;
    while (cs) {
        if (strcmp(cs->name, name) == 0) {
            return cs->addr;
        }
        cs = cs->next;
    }

    /* 2. Check built-in libc / system symbols */
    for (size_t i = 0; s_builtin_syms[i].name != NULL; i++) {
        if (strcmp(s_builtin_syms[i].name, name) == 0) {
            return s_builtin_syms[i].addr;
        }
    }

    /* 3. Check already loaded modules with RTLD_GLOBAL */
    dl_module_t *m = s_modules;
    while (m) {
        if (m->flags & RTLD_GLOBAL) {
            void *res = dlsym_in_module(m, name);
            if (res) return res;
        }
        m = m->next;
    }

    return NULL;
}

char *dlerror(void) {
    if (!s_has_dlerror) return NULL;
    s_has_dlerror = 0;
    return s_dlerror_buf;
}

static uint8_t *load_file_buffer(const char *path, uint32_t *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz <= 0) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (rd != (size_t)sz) {
        free(buf);
        return NULL;
    }
    if (out_size) *out_size = (uint32_t)sz;
    return buf;
}

void *dlopen(const char *filename, int flags) {
    /* If filename is NULL, return RTLD_DEFAULT */
    if (!filename || !*filename) {
        return RTLD_DEFAULT;
    }

    /* Check if already loaded by matching path */
    dl_module_t *cur = s_modules;
    while (cur) {
        if (cur->path && strcmp(cur->path, filename) == 0) {
            cur->ref_count++;
            return (void *)cur;
        }
        cur = cur->next;
    }

    /* Locate file: try exact path, then fallback to /applications/ and /lib/ */
    uint32_t file_size = 0;
    uint8_t *file_buf = load_file_buffer(filename, &file_size);
    char effective_path[256];
    strncpy(effective_path, filename, sizeof(effective_path) - 1);
    effective_path[sizeof(effective_path) - 1] = 0;

    if (!file_buf && !strchr(filename, '/')) {
        snprintf(effective_path, sizeof(effective_path), "/applications/%s", filename);
        file_buf = load_file_buffer(effective_path, &file_size);
        if (!file_buf) {
            snprintf(effective_path, sizeof(effective_path), "/lib/%s", filename);
            file_buf = load_file_buffer(effective_path, &file_size);
        }
    }

    if (!file_buf) {
        char err[128];
        snprintf(err, sizeof(err), "dlopen: unable to open file '%s'", filename);
        set_dlerror(err);
        return NULL;
    }

    /* Validate ELF header */
    if (file_size < sizeof(Elf32_Ehdr)) {
        set_dlerror("dlopen: file too small for ELF header");
        free(file_buf);
        return NULL;
    }
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)file_buf;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3) {
        set_dlerror("dlopen: invalid ELF magic header");
        free(file_buf);
        return NULL;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_machine != EM_386) {
        set_dlerror("dlopen: unsupported architecture (expected 32-bit x86 ELF)");
        free(file_buf);
        return NULL;
    }
    if (eh->e_type != ET_REL && eh->e_type != ET_DYN) {
        set_dlerror("dlopen: ELF type must be ET_REL or ET_DYN");
        free(file_buf);
        return NULL;
    }

    dl_module_t *mod = (dl_module_t *)calloc(1, sizeof(dl_module_t));
    if (!mod) {
        set_dlerror("dlopen: out of memory allocating module descriptor");
        free(file_buf);
        return NULL;
    }

    mod->path = strdup(effective_path);
    mod->elf_type = eh->e_type;
    mod->ref_count = 1;
    mod->flags = flags;

    if (eh->e_type == ET_REL) {
        /* Relocatable Object File (gcc -c / tcc -c) */
        if (eh->e_shoff == 0 || eh->e_shnum == 0) {
            set_dlerror("dlopen: no section headers in ET_REL file");
            free(mod->path); free(mod); free(file_buf);
            return NULL;
        }

        const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(file_buf + eh->e_shoff);
        mod->num_sections = eh->e_shnum;
        mod->section_addrs = (void **)calloc(eh->e_shnum, sizeof(void *));
        mod->section_sizes = (uint32_t *)calloc(eh->e_shnum, sizeof(uint32_t));

        /* Find Symbol Table and String Table */
        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            if (shdrs[i].sh_type == SHT_SYMTAB) {
                mod->num_syms = shdrs[i].sh_size / sizeof(Elf32_Sym);
                mod->symtab = (Elf32_Sym *)malloc(shdrs[i].sh_size);
                if (mod->symtab) {
                    memcpy(mod->symtab, file_buf + shdrs[i].sh_offset, shdrs[i].sh_size);
                }
                uint32_t str_idx = shdrs[i].sh_link;
                if (str_idx < eh->e_shnum) {
                    mod->strtab_size = shdrs[str_idx].sh_size;
                    mod->strtab = (char *)malloc(shdrs[str_idx].sh_size);
                    if (mod->strtab) {
                        memcpy(mod->strtab, file_buf + shdrs[str_idx].sh_offset, shdrs[str_idx].sh_size);
                    }
                }
                break;
            }
        }

        /* Calculate total contiguous memory size for SHF_ALLOC sections */
        uint32_t total_alloc = 0;
        uint32_t *sec_offsets = (uint32_t *)calloc(eh->e_shnum, sizeof(uint32_t));
        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            if (shdrs[i].sh_flags & SHF_ALLOC) {
                uint32_t align = shdrs[i].sh_addralign > 0 ? shdrs[i].sh_addralign : 4;
                total_alloc = (total_alloc + align - 1) & ~(align - 1);
                sec_offsets[i] = total_alloc;
                total_alloc += shdrs[i].sh_size;
            }
        }

        /* Also allocate space for SHN_COMMON symbols */
        if (mod->symtab) {
            mod->common_addrs = (uint32_t *)calloc(mod->num_syms, sizeof(uint32_t));
            for (uint32_t s = 0; s < mod->num_syms; s++) {
                if (mod->symtab[s].st_shndx == SHN_COMMON) {
                    uint32_t align = mod->symtab[s].st_value > 0 ? mod->symtab[s].st_value : 4;
                    total_alloc = (total_alloc + align - 1) & ~(align - 1);
                    mod->common_addrs[s] = total_alloc;
                    total_alloc += mod->symtab[s].st_size;
                }
            }
        }

        uint8_t *image = (uint8_t *)malloc(total_alloc + 64);
        if (!image) {
            set_dlerror("dlopen: out of memory for module image");
            free(sec_offsets); free(file_buf);
            if (mod->symtab) free(mod->symtab);
            if (mod->strtab) free(mod->strtab);
            if (mod->section_addrs) free(mod->section_addrs);
            if (mod->section_sizes) free(mod->section_sizes);
            if (mod->common_addrs) free(mod->common_addrs);
            free(mod->path); free(mod);
            return NULL;
        }
        memset(image, 0, total_alloc + 64);
        mod->image_base = image;
        mod->image_size = total_alloc;

        /* Place sections into allocated image */
        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            if (shdrs[i].sh_flags & SHF_ALLOC) {
                void *dest = image + sec_offsets[i];
                mod->section_addrs[i] = dest;
                mod->section_sizes[i] = shdrs[i].sh_size;
                if (shdrs[i].sh_type != SHT_NOBITS && shdrs[i].sh_offset < file_size) {
                    uint32_t copy_bytes = (shdrs[i].sh_offset + shdrs[i].sh_size <= file_size) ?
                                           shdrs[i].sh_size : (file_size - shdrs[i].sh_offset);
                    memcpy(dest, file_buf + shdrs[i].sh_offset, copy_bytes);
                }
            }
        }

        /* Adjust COMMON addresses relative to image pointer */
        if (mod->symtab && mod->common_addrs) {
            for (uint32_t s = 0; s < mod->num_syms; s++) {
                if (mod->symtab[s].st_shndx == SHN_COMMON) {
                    mod->common_addrs[s] += (uint32_t)image;
                }
            }
        }
        free(sec_offsets);

        /* Apply Relocations (SHT_REL) */
        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            if (shdrs[i].sh_type == SHT_REL) {
                uint32_t target_sec = shdrs[i].sh_info;
                if (target_sec >= eh->e_shnum || !mod->section_addrs[target_sec]) {
                    continue; /* Ignore relocations for discarded/non-allocated sections */
                }
                uint8_t *target_base = (uint8_t *)mod->section_addrs[target_sec];
                const Elf32_Rel *relocs = (const Elf32_Rel *)(file_buf + shdrs[i].sh_offset);
                uint32_t num_relocs = shdrs[i].sh_size / sizeof(Elf32_Rel);

                for (uint32_t r = 0; r < num_relocs; r++) {
                    const Elf32_Rel *rel = &relocs[r];
                    uint32_t sym_idx = ELF32_R_SYM(rel->r_info);
                    uint8_t rel_type = ELF32_R_TYPE(rel->r_info);
                    if (!mod->symtab || sym_idx >= mod->num_syms) continue;

                    Elf32_Sym *sym = &mod->symtab[sym_idx];
                    uint32_t sym_val = 0;

                    if (sym->st_shndx == SHN_UNDEF) {
                        const char *sname = (mod->strtab && sym->st_name < mod->strtab_size) ?
                                            (mod->strtab + sym->st_name) : "";
                        void *resolved = dlsym_resolve_global(sname);
                        if (!resolved) {
                            if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
                                sym_val = 0;
                            } else {
                                char err[128];
                                snprintf(err, sizeof(err), "dlopen: unresolved external symbol '%s'", sname);
                                set_dlerror(err);
                                free(file_buf);
                                dlclose((void *)mod);
                                return NULL;
                            }
                        } else {
                            sym_val = (uint32_t)resolved;
                        }
                    } else if (sym->st_shndx == SHN_ABS) {
                        sym_val = sym->st_value;
                    } else if (sym->st_shndx == SHN_COMMON) {
                        sym_val = mod->common_addrs ? mod->common_addrs[sym_idx] : 0;
                    } else if (sym->st_shndx < eh->e_shnum) {
                        sym_val = (uint32_t)mod->section_addrs[sym->st_shndx] + sym->st_value;
                    }

                    uint32_t *patch_loc = (uint32_t *)(target_base + rel->r_offset);
                    if (rel_type == R_386_32) {
                        *patch_loc += sym_val;
                    } else if (rel_type == R_386_PC32) {
                        *patch_loc += (sym_val - (uint32_t)patch_loc);
                    }
                }
            }
        }

    } else {
        /* Shared Library (ET_DYN, e.g. gcc -shared -fPIC) */
        if (eh->e_phoff == 0 || eh->e_phnum == 0) {
            set_dlerror("dlopen: no program headers in ET_DYN file");
            free(mod->path); free(mod); free(file_buf);
            return NULL;
        }

        const Elf32_Phdr *phdrs = (const Elf32_Phdr *)(file_buf + eh->e_phoff);
        uint32_t min_vaddr = 0xFFFFFFFF;
        uint32_t max_vaddr = 0;
        for (uint16_t i = 0; i < eh->e_phnum; i++) {
            if (phdrs[i].p_type == PT_LOAD) {
                if (phdrs[i].p_vaddr < min_vaddr) min_vaddr = phdrs[i].p_vaddr;
                if (phdrs[i].p_vaddr + phdrs[i].p_memsz > max_vaddr) {
                    max_vaddr = phdrs[i].p_vaddr + phdrs[i].p_memsz;
                }
            }
        }

        if (max_vaddr <= min_vaddr) {
            set_dlerror("dlopen: invalid PT_LOAD segment bounds");
            free(mod->path); free(mod); free(file_buf);
            return NULL;
        }

        uint32_t total_mem = max_vaddr - min_vaddr;
        uint8_t *image = (uint8_t *)malloc(total_mem + 4096);
        if (!image) {
            set_dlerror("dlopen: out of memory for ET_DYN image");
            free(mod->path); free(mod); free(file_buf);
            return NULL;
        }
        memset(image, 0, total_mem + 4096);
        mod->image_base = image;
        mod->image_size = total_mem;

        /* Load PT_LOAD segments */
        for (uint16_t i = 0; i < eh->e_phnum; i++) {
            if (phdrs[i].p_type == PT_LOAD) {
                uint8_t *dest = image + (phdrs[i].p_vaddr - min_vaddr);
                if (phdrs[i].p_filesz > 0 && phdrs[i].p_offset < file_size) {
                    uint32_t copy_bytes = (phdrs[i].p_offset + phdrs[i].p_filesz <= file_size) ?
                                           phdrs[i].p_filesz : (file_size - phdrs[i].p_offset);
                    memcpy(dest, file_buf + phdrs[i].p_offset, copy_bytes);
                }
            }
        }

        /* Read Symbol & String Table from SHT_DYNSYM or SHT_SYMTAB */
        if (eh->e_shoff > 0 && eh->e_shnum > 0) {
            const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(file_buf + eh->e_shoff);
            for (uint16_t i = 0; i < eh->e_shnum; i++) {
                if (shdrs[i].sh_type == SHT_DYNSYM || (!mod->symtab && shdrs[i].sh_type == SHT_SYMTAB)) {
                    mod->num_syms = shdrs[i].sh_size / sizeof(Elf32_Sym);
                    mod->symtab = (Elf32_Sym *)malloc(shdrs[i].sh_size);
                    if (mod->symtab) {
                        memcpy(mod->symtab, file_buf + shdrs[i].sh_offset, shdrs[i].sh_size);
                    }
                    uint32_t str_idx = shdrs[i].sh_link;
                    if (str_idx < eh->e_shnum) {
                        mod->strtab_size = shdrs[str_idx].sh_size;
                        mod->strtab = (char *)malloc(shdrs[str_idx].sh_size);
                        if (mod->strtab) {
                            memcpy(mod->strtab, file_buf + shdrs[str_idx].sh_offset, shdrs[str_idx].sh_size);
                        }
                    }
                }
            }

            /* Apply Dynamic Relocations (SHT_REL) */
            for (uint16_t i = 0; i < eh->e_shnum; i++) {
                if (shdrs[i].sh_type == SHT_REL) {
                    const Elf32_Rel *relocs = (const Elf32_Rel *)(file_buf + shdrs[i].sh_offset);
                    uint32_t num_relocs = shdrs[i].sh_size / sizeof(Elf32_Rel);
                    for (uint32_t r = 0; r < num_relocs; r++) {
                        const Elf32_Rel *rel = &relocs[r];
                        uint32_t sym_idx = ELF32_R_SYM(rel->r_info);
                        uint8_t rel_type = ELF32_R_TYPE(rel->r_info);
                        uint32_t rel_offset = rel->r_offset >= min_vaddr ? (rel->r_offset - min_vaddr) : rel->r_offset;
                        if (rel_offset + 4 > total_mem) continue;

                        uint32_t *patch_loc = (uint32_t *)(image + rel_offset);

                        if (rel_type == R_386_RELATIVE) {
                            *patch_loc += (uint32_t)image;
                        } else if (rel_type == R_386_GLOB_DAT || rel_type == R_386_JMP_SLOT || rel_type == R_386_32) {
                            uint32_t sym_val = 0;
                            if (mod->symtab && sym_idx < mod->num_syms) {
                                Elf32_Sym *sym = &mod->symtab[sym_idx];
                                if (sym->st_shndx == SHN_UNDEF) {
                                    const char *sname = (mod->strtab && sym->st_name < mod->strtab_size) ?
                                                        (mod->strtab + sym->st_name) : "";
                                    void *res = dlsym_resolve_global(sname);
                                    if (res) {
                                        sym_val = (uint32_t)res;
                                    }
                                } else {
                                    sym_val = (uint32_t)image + sym->st_value;
                                }
                            }
                            if (rel_type == R_386_32) {
                                *patch_loc += sym_val;
                            } else {
                                *patch_loc = sym_val;
                            }
                        }
                    }
                }
            }
        }
    }

    free(file_buf);

    /* Look for initialization entrypoints: _init, init, or module_init */
    void (*init_fn)(void) = (void (*)(void))dlsym_in_module(mod, "_init");
    if (!init_fn) init_fn = (void (*)(void))dlsym_in_module(mod, "init");
    if (!init_fn) init_fn = (void (*)(void))dlsym_in_module(mod, "module_init");
    mod->init_fn = init_fn;

    void (*fini_fn)(void) = (void (*)(void))dlsym_in_module(mod, "_fini");
    if (!fini_fn) fini_fn = (void (*)(void))dlsym_in_module(mod, "fini");
    if (!fini_fn) fini_fn = (void (*)(void))dlsym_in_module(mod, "module_fini");
    mod->fini_fn = fini_fn;

    /* Call module initialization if present */
    if (mod->init_fn) {
        mod->init_fn();
    }

    /* Insert into active modules list */
    mod->next = s_modules;
    s_modules = mod;

    return (void *)mod;
}

void *dlsym(void *handle, const char *symbol) {
    if (!symbol || !*symbol) {
        set_dlerror("dlsym: empty symbol name");
        return NULL;
    }

    /* If handle is NULL or RTLD_DEFAULT, search all loaded modules, then global symbols */
    if (!handle || handle == RTLD_DEFAULT) {
        dl_module_t *m = s_modules;
        while (m) {
            void *res = dlsym_in_module(m, symbol);
            if (res) return res;
            m = m->next;
        }
        void *res = dlsym_resolve_global(symbol);
        if (res) return res;

        char err[128];
        snprintf(err, sizeof(err), "dlsym: symbol '%s' not found globally", symbol);
        set_dlerror(err);
        return NULL;
    }

    /* Search inside specific module */
    dl_module_t *mod = (dl_module_t *)handle;
    void *res = dlsym_in_module(mod, symbol);
    if (!res) {
        char err[128];
        snprintf(err, sizeof(err), "dlsym: symbol '%s' not found in module '%s'",
                 symbol, mod->path ? mod->path : "unnamed");
        set_dlerror(err);
        return NULL;
    }

    return res;
}

int dlclose(void *handle) {
    if (!handle || handle == RTLD_DEFAULT) {
        return 0;
    }

    dl_module_t *prev = NULL;
    dl_module_t *cur = s_modules;
    while (cur) {
        if (cur == (dl_module_t *)handle) break;
        prev = cur;
        cur = cur->next;
    }

    if (!cur) {
        set_dlerror("dlclose: invalid module handle");
        return -1;
    }

    cur->ref_count--;
    if (cur->ref_count <= 0) {
        if (cur->fini_fn) {
            cur->fini_fn();
        }
        if (prev) {
            prev->next = cur->next;
        } else {
            s_modules = cur->next;
        }

        if (cur->path) free(cur->path);
        if (cur->image_base) free(cur->image_base);
        if (cur->section_addrs) free(cur->section_addrs);
        if (cur->section_sizes) free(cur->section_sizes);
        if (cur->common_addrs) free(cur->common_addrs);
        if (cur->symtab) free(cur->symtab);
        if (cur->strtab) free(cur->strtab);
        free(cur);
    }

    return 0;
}

int dladdr(const void *addr, Dl_info *info) {
    if (!info) return 0;
    info->dli_fname = NULL;
    info->dli_fbase = NULL;
    info->dli_sname = NULL;
    info->dli_saddr = NULL;

    uint32_t a = (uint32_t)addr;
    dl_module_t *m = s_modules;
    while (m) {
        uint32_t base = (uint32_t)m->image_base;
        if (a >= base && a < base + m->image_size) {
            info->dli_fname = m->path;
            info->dli_fbase = m->image_base;

            uint32_t best_diff = 0xFFFFFFFF;
            for (uint32_t i = 0; i < m->num_syms; i++) {
                if (m->strtab && m->symtab[i].st_name < m->strtab_size) {
                    const char *sname = m->strtab + m->symtab[i].st_name;
                    void *saddr = dlsym_in_module(m, sname);
                    if (saddr && (uint32_t)saddr <= a) {
                        uint32_t diff = a - (uint32_t)saddr;
                        if (diff < best_diff) {
                            best_diff = diff;
                            info->dli_sname = sname;
                            info->dli_saddr = saddr;
                        }
                    }
                }
            }
            return 1;
        }
        m = m->next;
    }

    return 0;
}

