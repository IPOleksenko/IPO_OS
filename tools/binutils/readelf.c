#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <elf.h>

static void dump_ehdr(const Elf32_Ehdr *e) {
    printf("ELF Header:\n");
    printf("  Magic:   ");
    for (int i = 0; i < EI_NIDENT; i++) printf("%02x ", e->e_ident[i]);
    printf("\n");
    printf("  Class:                             %s\n", e->e_ident[4] == 1 ? "ELF32" : "Unknown");
    printf("  Data:                              %s\n", e->e_ident[5] == 1 ? "2's complement, little endian" : "Unknown");
    printf("  Version:                           %d\n", e->e_ident[6]);
    printf("  Type:                              ");
    switch (e->e_type) {
        case ET_REL:  printf("REL (Relocatable file)\n"); break;
        case ET_EXEC: printf("EXEC (Executable file)\n"); break;
        case ET_DYN:  printf("DYN (Shared object file)\n"); break;
        default:      printf("0x%x\n", e->e_type); break;
    }
    printf("  Machine:                           ");
    switch (e->e_machine) {
        case EM_386: printf("Intel 80386\n"); break;
        default:     printf("0x%x\n", e->e_machine); break;
    }
    printf("  Version:                           0x%x\n", e->e_version);
    printf("  Entry point address:               0x%x\n", e->e_entry);
    printf("  Start of program headers:          %u (bytes into file)\n", e->e_phoff);
    printf("  Start of section headers:          %u (bytes into file)\n", e->e_shoff);
    printf("  Flags:                             0x%x\n", e->e_flags);
    printf("  Size of this header:               %u (bytes)\n", e->e_ehsize);
    printf("  Size of program headers:           %u (bytes)\n", e->e_phentsize);
    printf("  Number of program headers:         %u\n", e->e_phnum);
    printf("  Size of section headers:           %u (bytes)\n", e->e_shentsize);
    printf("  Number of section headers:         %u\n", e->e_shnum);
    printf("  Section header string table index: %u\n\n", e->e_shstrndx);
}

static void dump_phdrs(const uint8_t *raw, const Elf32_Ehdr *e) {
    if (e->e_phnum == 0) {
        printf("There are no program headers in this file.\n\n");
        return;
    }
    printf("Program Headers:\n");
    printf("  Type           Offset   VirtAddr   PhysAddr   FileSiz MemSiz  Flg Align\n");
    const Elf32_Phdr *p = (const Elf32_Phdr *)(raw + e->e_phoff);
    for (uint16_t i = 0; i < e->e_phnum; i++) {
        const char *t = "UNKNOWN";
        switch (p[i].p_type) {
            case PT_NULL:    t = "NULL"; break;
            case PT_LOAD:    t = "LOAD"; break;
            case PT_DYNAMIC: t = "DYNAMIC"; break;
            case PT_INTERP:  t = "INTERP"; break;
            case PT_NOTE:    t = "NOTE"; break;
            case PT_SHLIB:   t = "SHLIB"; break;
            case PT_PHDR:    t = "PHDR"; break;
        }
        char flags[4] = "   ";
        if (p[i].p_flags & PF_R) flags[0] = 'R';
        if (p[i].p_flags & PF_W) flags[1] = 'W';
        if (p[i].p_flags & PF_X) flags[2] = 'E';

        printf("  %-14s 0x%06x 0x%08x 0x%08x 0x%05x 0x%05x %s 0x%x\n",
               t, p[i].p_offset, p[i].p_vaddr, p[i].p_paddr,
               p[i].p_filesz, p[i].p_memsz, flags, p[i].p_align);
    }
    printf("\n");
}

static void dump_shdrs(const uint8_t *raw, const Elf32_Ehdr *e) {
    if (e->e_shnum == 0) {
        printf("There are no sections in this file.\n\n");
        return;
    }
    const Elf32_Shdr *s = (const Elf32_Shdr *)(raw + e->e_shoff);
    const char *strtab = "";
    if (e->e_shstrndx < e->e_shnum) {
        strtab = (const char *)(raw + s[e->e_shstrndx].sh_offset);
    }

    printf("Section Headers:\n");
    printf("  [Nr] Name              Type            Addr     Off    Size   ES Flg Lk Inf Al\n");
    for (uint16_t i = 0; i < e->e_shnum; i++) {
        const char *name = strtab + s[i].sh_name;
        const char *type = "UNKNOWN";
        switch (s[i].sh_type) {
            case SHT_NULL:     type = "NULL"; break;
            case SHT_PROGBITS: type = "PROGBITS"; break;
            case SHT_SYMTAB:   type = "SYMTAB"; break;
            case SHT_STRTAB:   type = "STRTAB"; break;
            case SHT_RELA:     type = "RELA"; break;
            case SHT_HASH:     type = "HASH"; break;
            case SHT_DYNAMIC:  type = "DYNAMIC"; break;
            case SHT_NOTE:     type = "NOTE"; break;
            case SHT_NOBITS:   type = "NOBITS"; break;
            case SHT_REL:      type = "REL"; break;
            case SHT_SHLIB:    type = "SHLIB"; break;
            case SHT_DYNSYM:   type = "DYNSYM"; break;
        }
        char flags[4] = "";
        int fidx = 0;
        if (s[i].sh_flags & SHF_WRITE)     flags[fidx++] = 'W';
        if (s[i].sh_flags & SHF_ALLOC)     flags[fidx++] = 'A';
        if (s[i].sh_flags & SHF_EXECINSTR) flags[fidx++] = 'X';
        flags[fidx] = '\0';

        printf("  [%2u] %-17s %-15s %08x %06x %06x %02x %3s %2u %3u %2u\n",
               i, name, type, s[i].sh_addr, s[i].sh_offset, s[i].sh_size,
               s[i].sh_entsize, flags, s[i].sh_link, s[i].sh_info, s[i].sh_addralign);
    }
    printf("\n");
}

static void dump_syms(const uint8_t *raw, const Elf32_Ehdr *e) {
    const Elf32_Shdr *s = (const Elf32_Shdr *)(raw + e->e_shoff);
    for (uint16_t i = 0; i < e->e_shnum; i++) {
        if (s[i].sh_type == SHT_SYMTAB || s[i].sh_type == SHT_DYNSYM) {
            const char *strtab = (const char *)(raw + s[s[i].sh_link].sh_offset);
            const Elf32_Sym *sym = (const Elf32_Sym *)(raw + s[i].sh_offset);
            uint32_t count = s[i].sh_size / sizeof(Elf32_Sym);

            printf("Symbol table '%s' contains %u entries:\n",
                   (const char *)(raw + s[e->e_shstrndx].sh_offset + s[i].sh_name), count);
            printf("   Num:    Value  Size Type    Bind   Vis      Ndx Name\n");
            for (uint32_t j = 0; j < count; j++) {
                const char *type = "NOTYPE";
                switch (ELF32_ST_TYPE(sym[j].st_info)) {
                    case STT_OBJECT:  type = "OBJECT"; break;
                    case STT_FUNC:    type = "FUNC"; break;
                    case STT_SECTION: type = "SECTION"; break;
                    case STT_FILE:    type = "FILE"; break;
                }
                const char *bind = "LOCAL";
                switch (ELF32_ST_BIND(sym[j].st_info)) {
                    case STB_GLOBAL: bind = "GLOBAL"; break;
                    case STB_WEAK:   bind = "WEAK"; break;
                }

                char ndx_buf[16];
                if (sym[j].st_shndx == SHN_UNDEF) strcpy(ndx_buf, "UND");
                else if (sym[j].st_shndx == SHN_ABS) strcpy(ndx_buf, "ABS");
                else if (sym[j].st_shndx == SHN_COMMON) strcpy(ndx_buf, "COM");
                else snprintf(ndx_buf, sizeof(ndx_buf), "%u", sym[j].st_shndx);

                printf("%6u: %08x %5u %-7s %-6s DEFAULT %4s %s\n",
                       j, sym[j].st_value, sym[j].st_size, type, bind,
                       ndx_buf, strtab + sym[j].st_name);
            }
            printf("\n");
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: readelf <option(s)> elf-file\n");
        fprintf(stderr, " Options are:\n");
        fprintf(stderr, "  -h -file-header       Display the ELF file header\n");
        fprintf(stderr, "  -l -program-headers   Display the program headers\n");
        fprintf(stderr, "  -S -section-headers   Display the sections' header\n");
        fprintf(stderr, "  -s -symbols           Display the symbol table\n");
        fprintf(stderr, "  -a -all               Equivalent to: -h -l -S -s\n");
        return 1;
    }

    int do_h = 0, do_l = 0, do_S = 0, do_s = 0;
    const char *file_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strchr(argv[i], 'a')) do_h = do_l = do_S = do_s = 1;
            if (strchr(argv[i], 'h')) do_h = 1;
            if (strchr(argv[i], 'l')) do_l = 1;
            if (strchr(argv[i], 'S')) do_S = 1;
            if (strchr(argv[i], 's')) do_s = 1;
        } else {
            file_path = argv[i];
        }
    }

    if (!file_path) {
        fprintf(stderr, "readelf: No file specified\n");
        return 1;
    }

    if (!do_h && !do_l && !do_S && !do_s) {
        do_h = 1;
    }

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        fprintf(stderr, "readelf: '%s': No such file\n", file_path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *raw = (uint8_t *)malloc(size);
    if (fread(raw, 1, size, f) != (size_t)size) {
        fclose(f);
        free(raw);
        return 1;
    }
    fclose(f);

    const Elf32_Ehdr *e = (const Elf32_Ehdr *)raw;
    if (memcmp(e->e_ident, ELF_MAGIC, 4) != 0) {
        fprintf(stderr, "readelf: Error: Not an ELF file\n");
        free(raw);
        return 1;
    }

    if (do_h) dump_ehdr(e);
    if (do_l) dump_phdrs(raw, e);
    if (do_S) dump_shdrs(raw, e);
    if (do_s) dump_syms(raw, e);

    free(raw);
    return 0;
}
