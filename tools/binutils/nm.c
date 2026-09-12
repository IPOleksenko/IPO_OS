#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <elf.h>

static void dump_symbols(const uint8_t *raw, long file_size, const char *filename, int show_filename) {
    if (file_size < (long)sizeof(Elf32_Ehdr)) return;
    const Elf32_Ehdr *e = (const Elf32_Ehdr *)raw;
    if (memcmp(e->e_ident, ELF_MAGIC, 4) != 0) return;

    if (e->e_shoff >= (uint32_t)file_size || e->e_shnum == 0) return;
    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(raw + e->e_shoff);

    const char *shstrtab = "";
    if (e->e_shstrndx < e->e_shnum) {
        shstrtab = (const char *)(raw + shdrs[e->e_shstrndx].sh_offset);
    }

    for (uint16_t i = 0; i < e->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB || shdrs[i].sh_type == SHT_DYNSYM) {
            uint32_t count = shdrs[i].sh_size / sizeof(Elf32_Sym);
            const Elf32_Sym *syms = (const Elf32_Sym *)(raw + shdrs[i].sh_offset);
            const char *strtab = (const char *)(raw + shdrs[shdrs[i].sh_link].sh_offset);

            for (uint32_t j = 1; j < count; j++) {
                const char *name = strtab + syms[j].st_name;
                if (!name || !*name) continue;

                char type = '?';
                int bind = ELF32_ST_BIND(syms[j].st_info);
                int sym_type = ELF32_ST_TYPE(syms[j].st_info);

                if (sym_type == STT_FILE || sym_type == STT_SECTION) continue;

                if (syms[j].st_shndx == SHN_UNDEF) {
                    type = 'U';
                } else if (syms[j].st_shndx == SHN_ABS) {
                    type = 'A';
                } else if (syms[j].st_shndx == SHN_COMMON) {
                    type = 'C';
                } else if (syms[j].st_shndx < e->e_shnum) {
                    const Elf32_Shdr *sec = &shdrs[syms[j].st_shndx];
                    const char *sname = shstrtab + sec->sh_name;
                    if (strcmp(sname, ".text") == 0 || (sec->sh_flags & SHF_EXECINSTR)) {
                        type = 'T';
                    } else if (strcmp(sname, ".rodata") == 0) {
                        type = 'R';
                    } else if (strcmp(sname, ".bss") == 0 || sec->sh_type == SHT_NOBITS) {
                        type = 'B';
                    } else if (strcmp(sname, ".data") == 0 || (sec->sh_flags & SHF_WRITE)) {
                        type = 'D';
                    } else {
                        type = 'N';
                    }
                }

                if (bind == STB_LOCAL && type != '?' && type != 'U') {
                    type = (char)(type + ('a' - 'A'));
                }

                if (show_filename) printf("%s: ", filename);

                if (type == 'U') {
                    printf("         %c %s\n", type, name);
                } else {
                    printf("%08x %c %s\n", syms[j].st_value, type, name);
                }
            }
            break; // Usually print standard symtab
        }
    }
}

static void process_file(const char *path, int multi) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "nm: '%s': No such file\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *raw = (uint8_t *)malloc(size);
    if (fread(raw, 1, size, f) != (size_t)size) {
        free(raw);
        fclose(f);
        return;
    }
    fclose(f);

    if (size >= 8 && memcmp(raw, "!<arch>\n", 8) == 0) {
        // Archive
        long offset = 8;
        while (offset + 60 <= size) {
            char name[17];
            memcpy(name, raw + offset, 16);
            name[16] = '\0';
            char *slash = strchr(name, '/');
            if (slash) *slash = '\0';

            char size_buf[11];
            memcpy(size_buf, raw + offset + 48, 10);
            size_buf[10] = '\0';
            long msize = strtol(size_buf, NULL, 10);

            offset += 60;
            if (name[0] && strcmp(name, "/") != 0 && strcmp(name, "__.SYMDEF") != 0) {
                printf("\n%s[%s]:\n", path, name);
                dump_symbols(raw + offset, msize, name, 0);
            }
            offset += msize;
            if (offset & 1) offset++;
        }
    } else {
        dump_symbols(raw, size, path, multi);
    }

    free(raw);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        process_file("a.out", 0);
        return 0;
    }

    int file_count = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') file_count++;
    }

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        process_file(argv[i], file_count > 1);
    }
    return 0;
}
