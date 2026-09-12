#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <elf.h>

#define DEFAULT_BASE    0x00800000

#define AR_MAGIC "!<arch>\n"
#define AR_MAGIC_LEN 8

struct ar_hdr {
    char ar_name[16];
    char ar_date[12];
    char ar_uid[6];
    char ar_gid[6];
    char ar_mode[8];
    char ar_size[10];
    char ar_fmag[2];
};

typedef struct Section Section;

typedef struct {
    char *name;
    uint8_t *data;
    uint32_t size;
    int *sec_map;
} InputObject;

struct Section {
    char name[64];
    uint32_t type;
    uint32_t flags;
    uint32_t vaddr;
    uint32_t offset;
    uint32_t size;
    uint32_t align;
    uint8_t *data;
    InputObject *obj;
    uint32_t obj_sec_idx;
};

typedef struct {
    char name[128];
    uint32_t value;
    uint32_t size;
    uint8_t bind;
    uint8_t type;
    int defined;
    int section_index;
} Symbol;

static InputObject *input_objs = NULL;
static size_t num_input_objs = 0;
static size_t cap_input_objs = 0;

static const char **lib_paths = NULL;
static size_t num_lib_paths = 0;
static size_t cap_lib_paths = 0;

static Section *sections = NULL;
static size_t num_sections = 0;
static size_t cap_sections = 0;

static Symbol *symbols = NULL;
static size_t num_symbols = 0;
static size_t cap_symbols = 0;

#define SYM_HASH_SIZE 4096
static int sym_hash_head[SYM_HASH_SIZE];
static int *sym_hash_next = NULL;

static uint32_t sym_hash(const char *str) {
    uint32_t h = 5381;
    while (*str) {
        h = ((h << 5) + h) + (uint8_t)(*str++);
    }
    return h & (SYM_HASH_SIZE - 1);
}

static void sym_hash_init(void) {
    static int inited = 0;
    if (inited) return;
    inited = 1;
    for (int i = 0; i < SYM_HASH_SIZE; i++) {
        sym_hash_head[i] = -1;
    }
}

static const char *output_filename = "a.out";
static const char *entry_symbol_name = "_start";
static uint32_t base_address = DEFAULT_BASE;
static int flag_strip = 0;

static void add_lib_path(const char *p) {
    if (num_lib_paths >= cap_lib_paths) {
        size_t new_cap = (cap_lib_paths == 0) ? 16 : cap_lib_paths * 2;
        const char **new_paths = (const char **)realloc(lib_paths, new_cap * sizeof(const char *));
        if (!new_paths) {
            fprintf(stderr, "ld: out of memory allocating lib paths\n");
            exit(1);
        }
        lib_paths = new_paths;
        cap_lib_paths = new_cap;
    }
    lib_paths[num_lib_paths++] = p;
}

static Symbol *find_symbol(const char *name) {
    sym_hash_init();
    uint32_t h = sym_hash(name);
    int idx = sym_hash_head[h];
    while (idx != -1) {
        if (strcmp(symbols[idx].name, name) == 0) return &symbols[idx];
        idx = sym_hash_next[idx];
    }
    return NULL;
}

static Symbol *add_or_get_symbol(const char *name) {
    sym_hash_init();
    Symbol *s = find_symbol(name);
    if (s) return s;
    if (num_symbols >= cap_symbols) {
        size_t new_cap = (cap_symbols == 0) ? 256 : cap_symbols * 2;
        Symbol *new_syms = (Symbol *)realloc(symbols, new_cap * sizeof(Symbol));
        int *new_next = (int *)realloc(sym_hash_next, new_cap * sizeof(int));
        if (!new_syms || !new_next) {
            fprintf(stderr, "ld: out of memory allocating symbols\n");
            exit(1);
        }
        symbols = new_syms;
        sym_hash_next = new_next;
        cap_symbols = new_cap;
    }
    int idx = (int)num_symbols++;
    s = &symbols[idx];
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = '\0';
    s->value = 0;
    s->size = 0;
    s->bind = STB_GLOBAL;
    s->type = STT_NOTYPE;
    s->defined = 0;
    s->section_index = -1;

    uint32_t h = sym_hash(name);
    sym_hash_next[idx] = sym_hash_head[h];
    sym_hash_head[h] = idx;

    return s;
}

static void add_input_object(const char *name, uint8_t *data, uint32_t size) {
    if (num_input_objs >= cap_input_objs) {
        size_t new_cap = (cap_input_objs == 0) ? 32 : cap_input_objs * 2;
        InputObject *new_objs = (InputObject *)realloc(input_objs, new_cap * sizeof(InputObject));
        if (!new_objs) {
            fprintf(stderr, "ld: out of memory allocating input objects\n");
            exit(1);
        }
        input_objs = new_objs;
        cap_input_objs = new_cap;
    }
    input_objs[num_input_objs].name = strdup(name);
    input_objs[num_input_objs].data = data;
    input_objs[num_input_objs].size = size;
    input_objs[num_input_objs].sec_map = NULL;
    num_input_objs++;
}

static Section *add_section(void) {
    if (num_sections >= cap_sections) {
        size_t new_cap = (cap_sections == 0) ? 64 : cap_sections * 2;
        Section *new_sec = (Section *)realloc(sections, new_cap * sizeof(Section));
        if (!new_sec) {
            fprintf(stderr, "ld: out of memory allocating sections\n");
            exit(1);
        }
        sections = new_sec;
        cap_sections = new_cap;
    }
    return &sections[num_sections++];
}

static void load_archive_needed(const char *ar_path, const uint8_t *ar_data, uint32_t ar_size) {
    int added_any = 1;
    while (added_any) {
        added_any = 0;
        uint32_t offset = AR_MAGIC_LEN;
        while (offset + sizeof(struct ar_hdr) <= ar_size) {
            const struct ar_hdr *hdr = (const struct ar_hdr *)(ar_data + offset);
            offset += sizeof(struct ar_hdr);

            char sz_buf[16];
            memcpy(sz_buf, hdr->ar_size, 10);
            sz_buf[10] = '\0';
            uint32_t member_size = (uint32_t)strtoul(sz_buf, NULL, 10);

            char name_buf[17];
            memcpy(name_buf, hdr->ar_name, 16);
            name_buf[16] = '\0';
            char *slash = strchr(name_buf, '/');
            if (slash) *slash = '\0';

            const uint8_t *member_data = ar_data + offset;

            // Align to 2 bytes
            offset += (member_size + 1) & ~1;

            if (name_buf[0] == '\0' || name_buf[0] == '/' || name_buf[0] == ' ') continue;

            if (member_size >= sizeof(Elf32_Ehdr) && memcmp(member_data, ELFMAG, SELFMAG) == 0) {
                const Elf32_Ehdr *eh = (const Elf32_Ehdr *)member_data;
                const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(member_data + eh->e_shoff);

                int needed = 0;
                for (uint16_t i = 0; i < eh->e_shnum; i++) {
                    if (shdrs[i].sh_type == SHT_SYMTAB) {
                        const Elf32_Sym *syms = (const Elf32_Sym *)(member_data + shdrs[i].sh_offset);
                        uint32_t count = shdrs[i].sh_size / sizeof(Elf32_Sym);
                        const char *strtab = (const char *)(member_data + shdrs[shdrs[i].sh_link].sh_offset);

                        for (uint32_t s = 1; s < count; s++) {
                            if (syms[s].st_shndx != SHN_UNDEF) {
                                const char *sname = &strtab[syms[s].st_name];
                                Symbol *sym = find_symbol(sname);
                                if (sym && !sym->defined) {
                                    needed = 1;
                                    break;
                                }
                            }
                        }
                    }
                    if (needed) break;
                }

                if (needed) {
                    // Check if already added
                    int already = 0;
                    for (int o = 0; o < num_input_objs; o++) {
                        if (input_objs[o].data == member_data) { already = 1; break; }
                    }
                    if (!already) {
                        char full_name[256];
                        snprintf(full_name, sizeof(full_name), "%s(%s)", ar_path, name_buf);
                        uint8_t *copy = (uint8_t *)malloc(member_size);
                        memcpy(copy, member_data, member_size);
                        add_input_object(full_name, copy, member_size);
                        added_any = 1;

                        // Scan and register defined symbols from this object immediately
                        const Elf32_Shdr *shs = (const Elf32_Shdr *)(copy + eh->e_shoff);
                        for (uint16_t i = 0; i < eh->e_shnum; i++) {
                            if (shs[i].sh_type == SHT_SYMTAB) {
                                const Elf32_Sym *syms = (const Elf32_Sym *)(copy + shs[i].sh_offset);
                                uint32_t count = shs[i].sh_size / sizeof(Elf32_Sym);
                                const char *strtab = (const char *)(copy + shs[shs[i].sh_link].sh_offset);
                                for (uint32_t s = 1; s < count; s++) {
                                    const char *sname = &strtab[syms[s].st_name];
                                    if (syms[s].st_shndx != SHN_UNDEF && ELF32_ST_BIND(syms[s].st_info) != STB_LOCAL) {
                                        Symbol *sym = add_or_get_symbol(sname);
                                        sym->defined = 1;
                                    } else if (syms[s].st_shndx == SHN_UNDEF) {
                                        add_or_get_symbol(sname);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

static void load_file(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "ld: cannot open %s: ", filename);
        perror("");
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = (uint8_t *)malloc(sz);
    if (!data || fread(data, 1, sz, f) != (size_t)sz) {
        fprintf(stderr, "ld: failed to read %s\n", filename);
        fclose(f);
        exit(1);
    }
    fclose(f);

    if (sz >= AR_MAGIC_LEN && memcmp(data, AR_MAGIC, AR_MAGIC_LEN) == 0) {
        load_archive_needed(filename, data, sz);
        free(data);
    } else if (sz >= (long)sizeof(Elf32_Ehdr) && memcmp(data, ELFMAG, SELFMAG) == 0) {
        add_input_object(filename, data, sz);
        // Pre-scan symbols
        const Elf32_Ehdr *eh = (const Elf32_Ehdr *)data;
        const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(data + eh->e_shoff);
        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            if (shdrs[i].sh_type == SHT_SYMTAB) {
                const Elf32_Sym *syms = (const Elf32_Sym *)(data + shdrs[i].sh_offset);
                uint32_t count = shdrs[i].sh_size / sizeof(Elf32_Sym);
                const char *strtab = (const char *)(data + shdrs[shdrs[i].sh_link].sh_offset);
                for (uint32_t s = 1; s < count; s++) {
                    const char *sname = &strtab[syms[s].st_name];
                    if (ELF32_ST_BIND(syms[s].st_info) != STB_LOCAL) {
                        Symbol *sym = add_or_get_symbol(sname);
                        if (syms[s].st_shndx != SHN_UNDEF) {
                            sym->defined = 1;
                        }
                    }
                }
            }
        }
    } else {
        fprintf(stderr, "ld: %s: unrecognized file format\n", filename);
        free(data);
    }
}

static void search_and_load_lib(const char *libname) {
    char filename[256];
    snprintf(filename, sizeof(filename), "lib%s.a", libname);

    for (int i = 0; i < num_lib_paths; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", lib_paths[i], filename);
        FILE *f = fopen(path, "rb");
        if (f) {
            fclose(f);
            load_file(path);
            return;
        }
    }
    // Also try without prefix
    for (int i = 0; i < num_lib_paths; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", lib_paths[i], libname);
        FILE *f = fopen(path, "rb");
        if (f) {
            fclose(f);
            load_file(path);
            return;
        }
    }

    fprintf(stderr, "ld: cannot find -l%s\n", libname);
    exit(1);
}

static void parse_linker_script(const char *script_file) {
    FILE *f = fopen(script_file, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, ". = 0x", 6) == 0 || strncmp(p, ". = 0X", 6) == 0) {
            base_address = (uint32_t)strtoul(p + 4, NULL, 16);
        } else if (strncmp(p, "ENTRY(", 6) == 0) {
            char *end = strchr(p + 6, ')');
            if (end) {
                *end = '\0';
                entry_symbol_name = strdup(p + 6);
            }
        }
    }
    fclose(f);
}

static void write_padding(FILE *fout, uint32_t count) {
    static const uint8_t zeros[512] = {0};
    while (count > 0) {
        uint32_t chunk = (count > sizeof(zeros)) ? sizeof(zeros) : count;
        fwrite(zeros, 1, chunk, fout);
        count -= chunk;
    }
}

int main(int argc, char **argv) {
    add_lib_path("/usr/lib");
    add_lib_path("/lib");
    add_lib_path("build/lib");
    add_lib_path(".");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_filename = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            i++; // ignore emulation (e.g. elf_i386)
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            parse_linker_script(argv[++i]);
        } else if (strncmp(argv[i], "-T", 2) == 0 && argv[i][2] != '\0') {
            parse_linker_script(&argv[i][2]);
        } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            entry_symbol_name = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--strip-all") == 0) {
            flag_strip = 1;
        } else if (strncmp(argv[i], "-L", 2) == 0) {
            if (argv[i][2] != '\0') add_lib_path(&argv[i][2]);
            else if (i + 1 < argc) add_lib_path(argv[++i]);
        } else if (strncmp(argv[i], "-l", 2) == 0) {
            const char *lib = (argv[i][2] != '\0') ? &argv[i][2] : argv[++i];
            search_and_load_lib(lib);
        } else if (strcmp(argv[i], "--start-group") == 0 || strcmp(argv[i], "--end-group") == 0 ||
                   strcmp(argv[i], "-static") == 0 || strcmp(argv[i], "-nostdlib") == 0 ||
                   strcmp(argv[i], "-nostartfiles") == 0) {
            // Ignored flags
        } else if (argv[i][0] == '-') {
            // Ignore other flags
        } else {
            load_file(argv[i]);
        }
    }

    if (num_input_objs == 0) {
        fprintf(stderr, "ld: no input files\n");
        return 1;
    }

    // Step 2: Extract and categorize sections
    // Standard output sections: .text, .rodata, .data, .bss
    uint32_t text_size = 0, rodata_size = 0, data_size = 0, bss_size = 0;

    for (int o = 0; o < num_input_objs; o++) {
        InputObject *obj = &input_objs[o];
        const Elf32_Ehdr *eh = (const Elf32_Ehdr *)obj->data;
        const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(obj->data + eh->e_shoff);
        const char *shstrtab = (const char *)(obj->data + shdrs[eh->e_shstrndx].sh_offset);
        obj->sec_map = (int *)malloc(eh->e_shnum * sizeof(int));
        for (uint16_t s = 0; s < eh->e_shnum; s++) obj->sec_map[s] = -1;

        for (uint16_t s = 0; s < eh->e_shnum; s++) {
            const Elf32_Shdr *sh = &shdrs[s];
            if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0) continue;

            const char *sname = (sh->sh_name < shdrs[eh->e_shstrndx].sh_size) ? &shstrtab[sh->sh_name] : "";
            int sec_idx = (int)num_sections;
            Section *sec = add_section();
            strncpy(sec->name, sname, sizeof(sec->name) - 1);
            sec->type = sh->sh_type;
            sec->flags = sh->sh_flags;
            sec->align = sh->sh_addralign ? sh->sh_addralign : 4;
            sec->size = sh->sh_size;
            sec->data = (sh->sh_type != SHT_NOBITS) ? (obj->data + sh->sh_offset) : NULL;
            sec->obj = obj;
            sec->obj_sec_idx = s;
            if (obj->sec_map) {
                obj->sec_map[s] = sec_idx;
            }
        }
    }

    // Layout sections into virtual memory
    // Segment 1 (RX): Headers, .text, .rodata
    // Segment 2 (RW): .data, .bss
    uint32_t cur_vaddr = base_address;

    // Estimate header sizes
    uint32_t ehdr_size = sizeof(Elf32_Ehdr);
    uint32_t phdr_size = 2 * sizeof(Elf32_Phdr);
    uint32_t headers_total = (ehdr_size + phdr_size + 0x0F) & ~0x0F;
    cur_vaddr += headers_total;

    // 1. Text sections (SHF_EXECINSTR)
    uint32_t text_start_vaddr = cur_vaddr;
    for (int i = 0; i < num_sections; i++) {
        Section *s = &sections[i];
        if (s->flags & SHF_EXECINSTR) {
            cur_vaddr = (cur_vaddr + s->align - 1) & ~(s->align - 1);
            s->vaddr = cur_vaddr;
            cur_vaddr += s->size;
        }
    }

    // 2. Rodata sections (ALLOC, not WRITE, not EXEC)
    for (int i = 0; i < num_sections; i++) {
        Section *s = &sections[i];
        if ((s->flags & SHF_ALLOC) && !(s->flags & SHF_WRITE) && !(s->flags & SHF_EXECINSTR)) {
            cur_vaddr = (cur_vaddr + s->align - 1) & ~(s->align - 1);
            s->vaddr = cur_vaddr;
            cur_vaddr += s->size;
        }
    }

    uint32_t rx_end_vaddr = cur_vaddr;
    uint32_t rx_filesz = rx_end_vaddr - base_address;

    // Align to 4096-byte page boundary for RW segment
    cur_vaddr = (cur_vaddr + 0xFFF) & ~0xFFF;
    uint32_t rw_start_vaddr = cur_vaddr;
    uint32_t rw_file_offset = (rx_filesz + 0xFFF) & ~0xFFF;

    // 3. Data sections (ALLOC, WRITE, PROGBITS)
    for (int i = 0; i < num_sections; i++) {
        Section *s = &sections[i];
        if ((s->flags & SHF_WRITE) && s->type != SHT_NOBITS) {
            cur_vaddr = (cur_vaddr + s->align - 1) & ~(s->align - 1);
            s->vaddr = cur_vaddr;
            cur_vaddr += s->size;
        }
    }
    uint32_t data_end_vaddr = cur_vaddr;
    uint32_t rw_filesz = data_end_vaddr - rw_start_vaddr;

    // 4. BSS sections (ALLOC, WRITE, NOBITS)
    for (int i = 0; i < num_sections; i++) {
        Section *s = &sections[i];
        if ((s->flags & SHF_WRITE) && s->type == SHT_NOBITS) {
            cur_vaddr = (cur_vaddr + s->align - 1) & ~(s->align - 1);
            s->vaddr = cur_vaddr;
            cur_vaddr += s->size;
        }
    }
    uint32_t rw_memsz = cur_vaddr - rw_start_vaddr;

    // Provide standard linker symbols
    Symbol *sym_got = add_or_get_symbol("_GLOBAL_OFFSET_TABLE_");
    sym_got->defined = 1;
    sym_got->value = data_end_vaddr;

    Symbol *sym_bss = add_or_get_symbol("__bss_start");
    sym_bss->defined = 1;
    sym_bss->value = data_end_vaddr;

    Symbol *sym_bss_end = add_or_get_symbol("__bss_end");
    sym_bss_end->defined = 1;
    sym_bss_end->value = cur_vaddr;

    Symbol *sym_edata = add_or_get_symbol("_edata");
    sym_edata->defined = 1;
    sym_edata->value = data_end_vaddr;

    Symbol *sym_end = add_or_get_symbol("_end");
    sym_end->defined = 1;
    sym_end->value = cur_vaddr;

    Symbol *sym_end2 = add_or_get_symbol("end");
    sym_end2->defined = 1;
    sym_end2->value = cur_vaddr;

    uint32_t preinit_start = cur_vaddr, preinit_end = cur_vaddr;
    uint32_t init_start = cur_vaddr, init_end = cur_vaddr;
    uint32_t fini_start = cur_vaddr, fini_end = cur_vaddr;

    for (int i = 0; i < num_sections; i++) {
        Section *s = &sections[i];
        if (strstr(s->name, ".preinit_array")) {
            preinit_start = s->vaddr;
            preinit_end = s->vaddr + s->size;
        } else if (strstr(s->name, ".init_array") || strstr(s->name, ".ctors")) {
            if (init_start == cur_vaddr || s->vaddr < init_start) init_start = s->vaddr;
            if (s->vaddr + s->size > init_end) init_end = s->vaddr + s->size;
        } else if (strstr(s->name, ".fini_array") || strstr(s->name, ".dtors")) {
            if (fini_start == cur_vaddr || s->vaddr < fini_start) fini_start = s->vaddr;
            if (s->vaddr + s->size > fini_end) fini_end = s->vaddr + s->size;
        }
    }
    if (init_start == cur_vaddr) init_end = init_start;
    if (fini_start == cur_vaddr) fini_end = fini_start;

    Symbol *sym_pia_s = add_or_get_symbol("__preinit_array_start");
    sym_pia_s->defined = 1;
    sym_pia_s->value = preinit_start;

    Symbol *sym_pia_e = add_or_get_symbol("__preinit_array_end");
    sym_pia_e->defined = 1;
    sym_pia_e->value = preinit_end;

    Symbol *sym_ia_s = add_or_get_symbol("__init_array_start");
    sym_ia_s->defined = 1;
    sym_ia_s->value = init_start;

    Symbol *sym_ia_e = add_or_get_symbol("__init_array_end");
    sym_ia_e->defined = 1;
    sym_ia_e->value = init_end;

    Symbol *sym_fa_s = add_or_get_symbol("__fini_array_start");
    sym_fa_s->defined = 1;
    sym_fa_s->value = fini_start;

    Symbol *sym_fa_e = add_or_get_symbol("__fini_array_end");
    sym_fa_e->defined = 1;
    sym_fa_e->value = fini_end;

    Symbol *sym_dso = add_or_get_symbol("__dso_handle");
    if (!sym_dso->defined) {
        sym_dso->defined = 1;
        sym_dso->value = 0;
    }

    // Step 3: Resolve all symbols
    for (int o = 0; o < num_input_objs; o++) {
        InputObject *obj = &input_objs[o];
        const Elf32_Ehdr *eh = (const Elf32_Ehdr *)obj->data;
        const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(obj->data + eh->e_shoff);

        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            if (shdrs[i].sh_type == SHT_SYMTAB) {
                const Elf32_Sym *syms = (const Elf32_Sym *)(obj->data + shdrs[i].sh_offset);
                uint32_t count = shdrs[i].sh_size / sizeof(Elf32_Sym);
                const char *strtab = (const char *)(obj->data + shdrs[shdrs[i].sh_link].sh_offset);

                for (uint32_t s = 1; s < count; s++) {
                    const Elf32_Sym *sym = &syms[s];
                    const char *sname = &strtab[sym->st_name];
                    if (sym->st_shndx != SHN_UNDEF && sym->st_shndx < eh->e_shnum) {
                        int sec_idx = (obj->sec_map) ? obj->sec_map[sym->st_shndx] : -1;
                        if (sec_idx >= 0 && (size_t)sec_idx < num_sections) {
                            Section *target_sec = &sections[sec_idx];
                            Symbol *gsym = add_or_get_symbol(sname);
                            gsym->value = target_sec->vaddr + sym->st_value;
                            gsym->size = sym->st_size;
                            gsym->bind = ELF32_ST_BIND(sym->st_info);
                            gsym->type = ELF32_ST_TYPE(sym->st_info);
                            gsym->defined = 1;
                            gsym->section_index = sec_idx;
                        }
                    }
                }
            }
        }
    }

    // Step 4: Perform Relocations
    for (int o = 0; o < num_input_objs; o++) {
        InputObject *obj = &input_objs[o];
        const Elf32_Ehdr *eh = (const Elf32_Ehdr *)obj->data;
        const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(obj->data + eh->e_shoff);

        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            if (shdrs[i].sh_type == SHT_REL) {
                uint32_t target_sec_idx = shdrs[i].sh_info;
                int sec_idx = (obj->sec_map && target_sec_idx < eh->e_shnum) ? obj->sec_map[target_sec_idx] : -1;
                Section *target_sec = (sec_idx >= 0 && (size_t)sec_idx < num_sections) ? &sections[sec_idx] : NULL;
                if (!target_sec || !target_sec->data) continue;

                const Elf32_Rel *rels = (const Elf32_Rel *)(obj->data + shdrs[i].sh_offset);
                uint32_t num_rels = shdrs[i].sh_size / sizeof(Elf32_Rel);

                const Elf32_Shdr *symtab_sh = &shdrs[shdrs[i].sh_link];
                const Elf32_Sym *syms = (const Elf32_Sym *)(obj->data + symtab_sh->sh_offset);
                const char *strtab = (const char *)(obj->data + shdrs[symtab_sh->sh_link].sh_offset);

                for (uint32_t r = 0; r < num_rels; r++) {
                    const Elf32_Rel *rel = &rels[r];
                    uint32_t sym_idx = ELF32_R_SYM(rel->r_info);
                    uint32_t r_type  = ELF32_R_TYPE(rel->r_info);
                    const Elf32_Sym *sym = &syms[sym_idx];

                    uint32_t sym_val = 0;
                    if (ELF32_ST_BIND(sym->st_info) == STB_LOCAL) {
                        if (sym->st_shndx != SHN_UNDEF && sym->st_shndx < eh->e_shnum && obj->sec_map) {
                            int sec_ref_idx = obj->sec_map[sym->st_shndx];
                            if (sec_ref_idx >= 0 && (size_t)sec_ref_idx < num_sections) {
                                sym_val = sections[sec_ref_idx].vaddr + sym->st_value;
                            }
                        }
                    } else {
                        const char *sname = &strtab[sym->st_name];
                        Symbol *gsym = find_symbol(sname);
                        if (!gsym || !gsym->defined) {
                            if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
                                sym_val = 0;
                            } else {
                                fprintf(stderr, "ld: undefined reference to `%s'\n", sname);
                                return 1;
                            }
                        } else {
                            sym_val = gsym->value;
                        }
                    }

                    uint8_t *patch_ptr = target_sec->data + rel->r_offset;
                    uint32_t ref_addr = target_sec->vaddr + rel->r_offset;

                    if (r_type == R_386_32) {
                        *(uint32_t *)patch_ptr += sym_val;
                    } else if (r_type == R_386_PC32 || r_type == 4 /* R_386_PLT32 */ || r_type == 10 /* R_386_GOTPC */) {
                        *(uint32_t *)patch_ptr += sym_val - ref_addr;
                    } else if (r_type == 9 /* R_386_GOTOFF */) {
                        Symbol *got_sym = find_symbol("_GLOBAL_OFFSET_TABLE_");
                        uint32_t got_val = got_sym ? got_sym->value : 0;
                        *(uint32_t *)patch_ptr += sym_val - got_val;
                    }
                }
            }
        }
    }

    // Step 5: Determine entry point
    uint32_t entry_point = text_start_vaddr;
    Symbol *entry_sym = find_symbol(entry_symbol_name);
    if (entry_sym && entry_sym->defined) {
        entry_point = entry_sym->value;
    }

    // Step 6: Generate output ELF32 binary
    FILE *fout = fopen(output_filename, "wb");
    if (!fout) {
        perror("ld: cannot create output file");
        return 1;
    }

    Elf32_Ehdr out_eh;
    memset(&out_eh, 0, sizeof(out_eh));
    out_eh.e_ident[EI_MAG0] = ELFMAG0;
    out_eh.e_ident[EI_MAG1] = ELFMAG1;
    out_eh.e_ident[EI_MAG2] = ELFMAG2;
    out_eh.e_ident[EI_MAG3] = ELFMAG3;
    out_eh.e_ident[EI_CLASS] = 1; // ELFCLASS32
    out_eh.e_ident[EI_DATA] = 1;  // ELFDATA2LSB
    out_eh.e_ident[EI_VERSION] = 1; // EV_CURRENT
    out_eh.e_type = ET_EXEC;
    out_eh.e_machine = EM_386;
    out_eh.e_version = 1;
    out_eh.e_entry = entry_point;
    out_eh.e_phoff = sizeof(Elf32_Ehdr);
    out_eh.e_ehsize = sizeof(Elf32_Ehdr);
    out_eh.e_phentsize = sizeof(Elf32_Phdr);
    out_eh.e_phnum = 2;

    Elf32_Phdr phdrs[2];
    memset(phdrs, 0, sizeof(phdrs));

    // Phdr 0: RX segment (headers + text + rodata)
    phdrs[0].p_type = PT_LOAD;
    phdrs[0].p_offset = 0;
    phdrs[0].p_vaddr = base_address;
    phdrs[0].p_paddr = base_address;
    phdrs[0].p_filesz = rx_filesz;
    phdrs[0].p_memsz = rx_filesz;
    phdrs[0].p_flags = PF_R | PF_X;
    phdrs[0].p_align = 0x1000;

    // Phdr 1: RW segment (data + bss)
    phdrs[1].p_type = PT_LOAD;
    phdrs[1].p_offset = rw_file_offset;
    phdrs[1].p_vaddr = rw_start_vaddr;
    phdrs[1].p_paddr = rw_start_vaddr;
    phdrs[1].p_filesz = rw_filesz;
    phdrs[1].p_memsz = rw_memsz;
    phdrs[1].p_flags = PF_R | PF_W;
    phdrs[1].p_align = 0x1000;

    // Write file
    // 1. Ehdr + Phdrs
    fwrite(&out_eh, 1, sizeof(out_eh), fout);
    fwrite(phdrs, 1, sizeof(phdrs), fout);

    // Pad to headers_total
    uint32_t written = sizeof(out_eh) + sizeof(phdrs);
    if (written < headers_total) {
        write_padding(fout, headers_total - written);
        written = headers_total;
    }

    // Write 1. Text sections (SHF_EXECINSTR)
    for (size_t i = 0; i < num_sections; i++) {
        Section *s = &sections[i];
        if (s->flags & SHF_EXECINSTR) {
            uint32_t target_off = s->vaddr - base_address;
            if (written < target_off) {
                write_padding(fout, target_off - written);
                written = target_off;
            }
            s->offset = target_off;
            if (s->data && s->size > 0) {
                fwrite(s->data, 1, s->size, fout);
                written += s->size;
            }
        }
    }

    // Write 2. Rodata sections (ALLOC, not WRITE, not EXEC)
    for (size_t i = 0; i < num_sections; i++) {
        Section *s = &sections[i];
        if ((s->flags & SHF_ALLOC) && !(s->flags & SHF_WRITE) && !(s->flags & SHF_EXECINSTR)) {
            uint32_t target_off = s->vaddr - base_address;
            if (written < target_off) {
                write_padding(fout, target_off - written);
                written = target_off;
            }
            s->offset = target_off;
            if (s->data && s->size > 0) {
                fwrite(s->data, 1, s->size, fout);
                written += s->size;
            }
        }
    }

    // Pad to rw_file_offset
    if (written < rw_file_offset) {
        write_padding(fout, rw_file_offset - written);
        written = rw_file_offset;
    }

    // Write RW sections (data only, bss is NOBITS in file)
    for (size_t i = 0; i < num_sections; i++) {
        Section *s = &sections[i];
        if ((s->flags & SHF_WRITE) && s->type != SHT_NOBITS) {
            uint32_t target_off = rw_file_offset + (s->vaddr - rw_start_vaddr);
            if (written < target_off) {
                write_padding(fout, target_off - written);
                written = target_off;
            }
            s->offset = target_off;
            if (s->data && s->size > 0) {
                fwrite(s->data, 1, s->size, fout);
                written += s->size;
            }
        } else if (s->type == SHT_NOBITS) {
            s->offset = rw_file_offset + (s->vaddr - rw_start_vaddr);
        }
    }

    // Build and write .shstrtab
    size_t shstrtab_cap = 1024;
    for (size_t i = 0; i < num_sections; i++) {
        shstrtab_cap += strlen(sections[i].name) + 1;
    }
    shstrtab_cap += sizeof(".shstrtab") + 16;
    char *shstrtab = (char *)calloc(1, shstrtab_cap);
    size_t shstrtab_len = 1; // shstrtab[0] is '\0'

    uint32_t *sec_name_offsets = (uint32_t *)malloc(num_sections * sizeof(uint32_t));
    for (size_t i = 0; i < num_sections; i++) {
        sec_name_offsets[i] = (uint32_t)shstrtab_len;
        size_t nlen = strlen(sections[i].name);
        memcpy(shstrtab + shstrtab_len, sections[i].name, nlen + 1);
        shstrtab_len += nlen + 1;
    }
    uint32_t shstrtab_name_offset = (uint32_t)shstrtab_len;
    memcpy(shstrtab + shstrtab_len, ".shstrtab", sizeof(".shstrtab"));
    shstrtab_len += sizeof(".shstrtab");

    // Write .shstrtab
    uint32_t shstrtab_file_offset = written;
    fwrite(shstrtab, 1, shstrtab_len, fout);
    written += shstrtab_len;

    // Pad to 4-byte boundary for section headers
    if (written % 4 != 0) {
        uint32_t pad = 4 - (written % 4);
        write_padding(fout, pad);
        written += pad;
    }

    // Build Section Header Table
    uint16_t total_shdrs = (uint16_t)(1 + num_sections + 1);
    Elf32_Shdr *out_shdrs = (Elf32_Shdr *)calloc(total_shdrs, sizeof(Elf32_Shdr));

    for (size_t i = 0; i < num_sections; i++) {
        Elf32_Shdr *sh = &out_shdrs[i + 1];
        Section *s = &sections[i];
        sh->sh_name = sec_name_offsets[i];
        sh->sh_type = s->type;
        sh->sh_flags = s->flags;
        sh->sh_addr = s->vaddr;
        sh->sh_offset = s->offset;
        sh->sh_size = s->size;
        sh->sh_link = 0;
        sh->sh_info = 0;
        sh->sh_addralign = s->align ? s->align : 4;
        sh->sh_entsize = 0;
    }

    // .shstrtab section header
    Elf32_Shdr *sh_str = &out_shdrs[1 + num_sections];
    sh_str->sh_name = shstrtab_name_offset;
    sh_str->sh_type = SHT_STRTAB;
    sh_str->sh_flags = 0;
    sh_str->sh_addr = 0;
    sh_str->sh_offset = shstrtab_file_offset;
    sh_str->sh_size = shstrtab_len;
    sh_str->sh_addralign = 1;

    // Write Section Header Table
    uint32_t shdrs_file_offset = written;
    fwrite(out_shdrs, sizeof(Elf32_Shdr), total_shdrs, fout);
    written += sizeof(Elf32_Shdr) * total_shdrs;

    // Update out_eh with section header info
    out_eh.e_shoff = shdrs_file_offset;
    out_eh.e_shentsize = sizeof(Elf32_Shdr);
    out_eh.e_shnum = total_shdrs;
    out_eh.e_shstrndx = (uint16_t)(1 + num_sections);

    fseek(fout, 0, SEEK_SET);
    fwrite(&out_eh, 1, sizeof(out_eh), fout);

    free(shstrtab);
    free(sec_name_offsets);
    free(out_shdrs);

    fclose(fout);
    printf("ld: successfully created %s\n", output_filename);
    return 0;
}
