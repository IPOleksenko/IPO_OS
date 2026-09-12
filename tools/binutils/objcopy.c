#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <elf.h>

static const char **selected_sections = NULL;
static size_t num_selected = 0;
static size_t cap_selected = 0;
static int flag_binary = 0;
static int flag_strip_all = 0;
static int flag_strip_debug = 0;

static void add_selected_section(const char *sec) {
    if (num_selected >= cap_selected) {
        size_t new_cap = cap_selected ? cap_selected * 2 : 16;
        const char **new_secs = (const char **)realloc(selected_sections, new_cap * sizeof(const char *));
        if (!new_secs) {
            fprintf(stderr, "objcopy: out of memory\n");
            exit(1);
        }
        selected_sections = new_secs;
        cap_selected = new_cap;
    }
    selected_sections[num_selected++] = sec;
}

static int is_selected_section(const char *name) {
    if (num_selected == 0) return 1; // all by default
    for (int i = 0; i < num_selected; i++) {
        if (strcmp(selected_sections[i], name) == 0) return 1;
    }
    return 0;
}

static int convert_to_binary(const uint8_t *raw, long sz, const char *outfile) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)raw;
    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(raw + eh->e_shoff);
    const char *shstrtab = (const char *)(raw + shdrs[eh->e_shstrndx].sh_offset);

    // Find min vaddr and max vaddr of selected allocatable sections
    uint32_t min_vaddr = 0xFFFFFFFF;
    uint32_t max_vaddr = 0;
    int found_sec = 0;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *s = &shdrs[i];
        if (s->sh_size == 0) continue;
        const char *sname = (s->sh_name < shdrs[eh->e_shstrndx].sh_size) ? &shstrtab[s->sh_name] : "";
        if (num_selected > 0) {
            if (!is_selected_section(sname)) continue;
        } else {
            if (!(s->sh_flags & SHF_ALLOC)) continue;
        }

        found_sec = 1;
        if (s->sh_addr < min_vaddr) min_vaddr = s->sh_addr;
        if (s->sh_addr + s->sh_size > max_vaddr) max_vaddr = s->sh_addr + s->sh_size;
    }

    if (!found_sec || min_vaddr > max_vaddr) {
        // Fallback: copy whole file or program headers
        if (eh->e_phnum > 0) {
            const Elf32_Phdr *phdrs = (const Elf32_Phdr *)(raw + eh->e_phoff);
            for (uint16_t i = 0; i < eh->e_phnum; i++) {
                if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_memsz > 0) {
                    if (phdrs[i].p_vaddr < min_vaddr) min_vaddr = phdrs[i].p_vaddr;
                    if (phdrs[i].p_vaddr + phdrs[i].p_memsz > max_vaddr) max_vaddr = phdrs[i].p_vaddr + phdrs[i].p_memsz;
                    found_sec = 1;
                }
            }
        }
    }

    if (!found_sec) {
        fprintf(stderr, "objcopy: no matching sections found for binary output\n");
        return 1;
    }

    uint32_t bin_size = max_vaddr - min_vaddr;
    uint8_t *bin_data = (uint8_t *)calloc(1, bin_size);
    if (!bin_data) {
        fprintf(stderr, "objcopy: out of memory allocating binary buffer (%u bytes)\n", bin_size);
        return 1;
    }

    // Copy section contents into bin_data
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *s = &shdrs[i];
        if (s->sh_size == 0) continue;
        const char *sname = (s->sh_name < shdrs[eh->e_shstrndx].sh_size) ? &shstrtab[s->sh_name] : "";
        if (num_selected > 0) {
            if (!is_selected_section(sname)) continue;
        } else {
            if (!(s->sh_flags & SHF_ALLOC)) continue;
        }

        uint32_t offset = s->sh_addr - min_vaddr;
        if (s->sh_type != SHT_NOBITS && s->sh_offset + s->sh_size <= (uint32_t)sz) {
            memcpy(bin_data + offset, raw + s->sh_offset, s->sh_size);
        } else {
            memset(bin_data + offset, 0, s->sh_size);
        }
    }

    FILE *fout = fopen(outfile, "wb");
    if (!fout) {
        perror("objcopy: fopen output");
        free(bin_data);
        return 1;
    }
    fwrite(bin_data, 1, bin_size, fout);
    fclose(fout);
    free(bin_data);
    return 0;
}

static int strip_elf(const uint8_t *raw, long sz, const char *outfile) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)raw;
    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(raw + eh->e_shoff);
    const char *shstrtab = (const char *)(raw + shdrs[eh->e_shstrndx].sh_offset);

    // Keep sections that are allocated, and discard symbol tables / debug sections if strip_all
    uint8_t *out = (uint8_t *)malloc(sz);
    if (!out) {
        fprintf(stderr, "objcopy: out of memory\n");
        return 1;
    }

    Elf32_Ehdr *new_eh = (Elf32_Ehdr *)out;
    memcpy(new_eh, eh, sizeof(Elf32_Ehdr));

    // Copy program headers directly
    uint32_t ph_size = eh->e_phnum * eh->e_phentsize;
    if (ph_size > 0 && eh->e_phoff + ph_size <= (uint32_t)sz) {
        memcpy(out + eh->e_phoff, raw + eh->e_phoff, ph_size);
    }

    uint32_t cur_offset = eh->e_ehsize + ph_size;
    cur_offset = (cur_offset + 15) & ~15;

    Elf32_Shdr *new_shdrs = (Elf32_Shdr *)malloc(eh->e_shnum * sizeof(Elf32_Shdr));
    uint16_t new_shnum = 0;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *s = &shdrs[i];
        const char *sname = (s->sh_name < shdrs[eh->e_shstrndx].sh_size) ? &shstrtab[s->sh_name] : "";

        int discard = 0;
        if (flag_strip_all) {
            if (s->sh_type == SHT_SYMTAB || s->sh_type == SHT_STRTAB && i != eh->e_shstrndx) discard = 1;
            if (s->sh_type == SHT_REL || s->sh_type == SHT_RELA) discard = 1;
            if (strncmp(sname, ".debug", 6) == 0 || strncmp(sname, ".comment", 8) == 0) discard = 1;
        } else if (flag_strip_debug) {
            if (strncmp(sname, ".debug", 6) == 0 || strncmp(sname, ".comment", 8) == 0) discard = 1;
        }

        if (i == 0 || !discard) {
            new_shdrs[new_shnum] = *s;
            if (s->sh_type != SHT_NOBITS && s->sh_size > 0) {
                cur_offset = (cur_offset + (s->sh_addralign ? s->sh_addralign - 1 : 0)) & ~(s->sh_addralign ? s->sh_addralign - 1 : 1);
                memcpy(out + cur_offset, raw + s->sh_offset, s->sh_size);
                new_shdrs[new_shnum].sh_offset = cur_offset;
                cur_offset += s->sh_size;
            }
            new_shnum++;
        }
    }

    cur_offset = (cur_offset + 3) & ~3;
    new_eh->e_shoff = cur_offset;
    new_eh->e_shnum = new_shnum;
    memcpy(out + cur_offset, new_shdrs, new_shnum * sizeof(Elf32_Shdr));
    cur_offset += new_shnum * sizeof(Elf32_Shdr);

    FILE *fout = fopen(outfile, "wb");
    if (!fout) {
        perror("objcopy: fopen output");
        free(out);
        free(new_shdrs);
        return 1;
    }
    fwrite(out, 1, cur_offset, fout);
    fclose(fout);

    free(out);
    free(new_shdrs);
    return 0;
}

int main(int argc, char **argv) {
    const char *infile = NULL;
    const char *outfile = NULL;

    // Check if called as strip
    char *prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];
    if (strstr(prog, "strip") != NULL) {
        flag_strip_all = 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-O") == 0 && i + 1 < argc) {
            if (strcmp(argv[++i], "binary") == 0) flag_binary = 1;
        } else if (strncmp(argv[i], "-Obinary", 8) == 0) {
            flag_binary = 1;
        } else if (strcmp(argv[i], "-j") == 0 && i + 1 < argc) {
            add_selected_section(argv[++i]);
        } else if (strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "--strip-all") == 0) {
            flag_strip_all = 1;
        } else if (strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--strip-debug") == 0) {
            flag_strip_debug = 1;
        } else if (strncmp(argv[i], "--set-section-flags", 19) == 0) {
            // Ignored or parsed
            if (strchr(argv[i], '=') == NULL && i + 1 < argc) i++;
        } else if (argv[i][0] == '-') {
            // Ignore other options or flags
        } else {
            if (!infile) infile = argv[i];
            else if (!outfile) outfile = argv[i];
        }
    }

    if (!infile) {
        fprintf(stderr, "Usage: objcopy [-O binary] [-j section] [-S] <infile> [outfile]\n");
        return 1;
    }
    if (!outfile) {
        if (flag_strip_all || flag_strip_debug) {
            outfile = infile; // in-place strip
        } else {
            fprintf(stderr, "objcopy: output file required\n");
            return 1;
        }
    }

    FILE *fin = fopen(infile, "rb");
    if (!fin) {
        perror("objcopy: fopen input");
        return 1;
    }
    fseek(fin, 0, SEEK_END);
    long sz = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    uint8_t *raw = (uint8_t *)malloc(sz);
    if (!raw) {
        fclose(fin);
        return 1;
    }
    if (fread(raw, 1, sz, fin) != (size_t)sz) {
        free(raw);
        fclose(fin);
        return 1;
    }
    fclose(fin);

    if (sz < (long)sizeof(Elf32_Ehdr) || memcmp(raw, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "objcopy: %s: not a valid ELF file\n", infile);
        free(raw);
        return 1;
    }

    int ret = 0;
    if (flag_binary) {
        ret = convert_to_binary(raw, sz, outfile);
    } else {
        ret = strip_elf(raw, sz, outfile);
    }

    free(raw);
    return ret;
}
