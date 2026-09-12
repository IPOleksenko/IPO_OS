#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <elf.h>

static int flag_file_headers = 0;
static int flag_section_headers = 0;
static int flag_syms = 0;
static int flag_full_contents = 0;
static int flag_disassemble = 0;

static void dump_file_header(const Elf32_Ehdr *eh, const char *fname) {
    printf("\n%s:     file format elf32-i386\n", fname);
    printf("architecture: i386, flags 0x%08x:\n", eh->e_flags);
    if (eh->e_type == ET_EXEC) printf("EXEC_P, HAS_SYMS\n");
    else if (eh->e_type == ET_REL) printf("HAS_RELOC, HAS_SYMS\n");
    else printf("HAS_SYMS\n");
    printf("start address 0x%08x\n\n", eh->e_entry);
}

static void dump_section_headers(const uint8_t *raw, const Elf32_Ehdr *eh) {
    if (eh->e_shnum == 0) return;
    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(raw + eh->e_shoff);
    const char *shstrtab = (const char *)(raw + shdrs[eh->e_shstrndx].sh_offset);

    printf("Sections:\n");
    printf("Idx Name          Size      VMA       LMA       File off  Algn  Flags\n");
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *s = &shdrs[i];
        const char *name = (s->sh_name < shdrs[eh->e_shstrndx].sh_size) ? &shstrtab[s->sh_name] : "";
        char flags[64] = "";
        if (s->sh_flags & SHF_ALLOC) strcat(flags, "ALLOC, ");
        if (s->sh_flags & SHF_EXECINSTR) strcat(flags, "CODE, ");
        if (s->sh_flags & SHF_WRITE) strcat(flags, "DATA, ");
        else if (s->sh_flags & SHF_ALLOC) strcat(flags, "READONLY, ");
        if (s->sh_type == SHT_PROGBITS) strcat(flags, "LOAD, ");
        if (strlen(flags) > 2) flags[strlen(flags) - 2] = '\0';

        printf("%3u %-13s %08x  %08x  %08x  %08x  2**%u  %s\n",
               i, name, s->sh_size, s->sh_addr, s->sh_addr, s->sh_offset,
               s->sh_addralign ? (unsigned)(31 - __builtin_clz(s->sh_addralign)) : 0,
               flags);
    }
    printf("\n");
}

static void dump_symbols(const uint8_t *raw, const Elf32_Ehdr *eh) {
    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(raw + eh->e_shoff);
    const char *shstrtab = (const char *)(raw + shdrs[eh->e_shstrndx].sh_offset);

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB || shdrs[i].sh_type == SHT_DYNSYM) {
            const Elf32_Shdr *symtab_hdr = &shdrs[i];
            const Elf32_Sym *syms = (const Elf32_Sym *)(raw + symtab_hdr->sh_offset);
            uint32_t num_syms = symtab_hdr->sh_size / sizeof(Elf32_Sym);
            const char *strtab = (const char *)(raw + shdrs[symtab_hdr->sh_link].sh_offset);

            printf("SYMBOL TABLE:\n");
            for (uint32_t s = 0; s < num_syms; s++) {
                const Elf32_Sym *sym = &syms[s];
                const char *sname = (sym->st_name < shdrs[symtab_hdr->sh_link].sh_size) ? &strtab[sym->st_name] : "";
                char bind = (ELF32_ST_BIND(sym->st_info) == STB_LOCAL) ? 'l' :
                            (ELF32_ST_BIND(sym->st_info) == STB_GLOBAL) ? 'g' : 'w';
                char type = (ELF32_ST_TYPE(sym->st_info) == STT_FUNC) ? 'F' :
                            (ELF32_ST_TYPE(sym->st_info) == STT_OBJECT) ? 'O' :
                            (ELF32_ST_TYPE(sym->st_info) == STT_SECTION) ? 'd' :
                            (ELF32_ST_TYPE(sym->st_info) == STT_FILE) ? 'f' : ' ';
                const char *sec_name = "*UND*";
                if (sym->st_shndx == SHN_ABS) sec_name = "*ABS*";
                else if (sym->st_shndx == SHN_COMMON) sec_name = "*COM*";
                else if (sym->st_shndx < eh->e_shnum) {
                    sec_name = &shstrtab[shdrs[sym->st_shndx].sh_name];
                }
                printf("%08x %c %c      %-8s %08x %s\n",
                       sym->st_value, bind, type, sec_name, sym->st_size, sname);
            }
            printf("\n");
        }
    }
}

static void dump_full_contents(const uint8_t *raw, const Elf32_Ehdr *eh) {
    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(raw + eh->e_shoff);
    const char *shstrtab = (const char *)(raw + shdrs[eh->e_shstrndx].sh_offset);

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *s = &shdrs[i];
        if (s->sh_type == SHT_NOBITS || s->sh_size == 0) continue;
        const char *name = (s->sh_name < shdrs[eh->e_shstrndx].sh_size) ? &shstrtab[s->sh_name] : "";
        printf("Contents of section %s:\n", name);

        const uint8_t *data = raw + s->sh_offset;
        for (uint32_t off = 0; off < s->sh_size; off += 16) {
            printf(" %04x ", (unsigned)(s->sh_addr + off));
            for (uint32_t b = 0; b < 16; b++) {
                if (off + b < s->sh_size) {
                    printf("%02x", data[off + b]);
                } else {
                    printf("  ");
                }
                if ((b + 1) % 4 == 0) printf(" ");
            }
            printf(" ");
            for (uint32_t b = 0; b < 16; b++) {
                if (off + b < s->sh_size) {
                    uint8_t c = data[off + b];
                    putchar((c >= 32 && c <= 126) ? c : '.');
                }
            }
            printf("\n");
        }
    }
    printf("\n");
}

/* Lightweight x86 Disassembler */
static const char *reg32_names[] = {"%eax", "%ecx", "%edx", "%ebx", "%esp", "%ebp", "%esi", "%edi"};
static const char *reg16_names[] = {"%ax", "%cx", "%dx", "%bx", "%sp", "%bp", "%si", "%di"};
static const char *reg8_names[] = {"%al", "%cl", "%dl", "%bl", "%ah", "%ch", "%dh", "%bh"};

static int disassemble_instruction(const uint8_t *p, uint32_t vaddr, uint32_t max_len, char *out_text) {
    if (max_len == 0) return 0;
    uint32_t len = 0;
    uint8_t op = p[len++];

    // Prefixes
    int op_size_16 = 0;
    if (op == 0x66) {
        op_size_16 = 1;
        if (len >= max_len) return 1;
        op = p[len++];
    }

    if (op == 0x90) { sprintf(out_text, "nop"); return len; }
    if (op == 0xC3) { sprintf(out_text, "ret"); return len; }
    if (op == 0xCB) { sprintf(out_text, "lret"); return len; }
    if (op == 0xCC) { sprintf(out_text, "int3"); return len; }
    if (op == 0xCD) {
        if (len >= max_len) return len;
        sprintf(out_text, "int    $0x%02x", p[len++]);
        return len;
    }
    if (op == 0xFA) { sprintf(out_text, "cli"); return len; }
    if (op == 0xFB) { sprintf(out_text, "sti"); return len; }
    if (op == 0xF4) { sprintf(out_text, "hlt"); return len; }
    if (op == 0x9C) { sprintf(out_text, "pushf"); return len; }
    if (op == 0x9D) { sprintf(out_text, "popf"); return len; }
    if (op == 0x60) { sprintf(out_text, "pusha"); return len; }
    if (op == 0x61) { sprintf(out_text, "popa"); return len; }
    if (op == 0xC9) { sprintf(out_text, "leave"); return len; }

    // PUSH reg32 (0x50 .. 0x57)
    if (op >= 0x50 && op <= 0x57) {
        sprintf(out_text, "push   %s", reg32_names[op - 0x50]);
        return len;
    }
    // POP reg32 (0x58 .. 0x5F)
    if (op >= 0x58 && op <= 0x5F) {
        sprintf(out_text, "pop    %s", reg32_names[op - 0x58]);
        return len;
    }

    // PUSH imm32 (0x68)
    if (op == 0x68) {
        if (len + 4 > max_len) return len;
        uint32_t imm = *(const uint32_t *)(p + len);
        len += 4;
        sprintf(out_text, "push   $0x%x", imm);
        return len;
    }
    // PUSH imm8 (0x6A)
    if (op == 0x6A) {
        if (len >= max_len) return len;
        int8_t imm = (int8_t)p[len++];
        sprintf(out_text, "push   $0x%x", (uint32_t)(int32_t)imm);
        return len;
    }

    // MOV reg, imm32 (0xB8 .. 0xBF)
    if (op >= 0xB8 && op <= 0xBF) {
        if (len + 4 > max_len) return len;
        uint32_t imm = *(const uint32_t *)(p + len);
        len += 4;
        sprintf(out_text, "mov    $0x%x,%s", imm, reg32_names[op - 0xB8]);
        return len;
    }

    // MOV reg, imm8 (0xB0 .. 0xB7)
    if (op >= 0xB0 && op <= 0xB7) {
        if (len >= max_len) return len;
        uint8_t imm = p[len++];
        sprintf(out_text, "mov    $0x%x,%s", imm, reg8_names[op - 0xB0]);
        return len;
    }

    // Relative Jumps / Calls
    if (op == 0xE8) { // CALL rel32
        if (len + 4 > max_len) return len;
        int32_t rel = *(const int32_t *)(p + len);
        len += 4;
        sprintf(out_text, "call   0x%x", vaddr + len + rel);
        return len;
    }
    if (op == 0xE9) { // JMP rel32
        if (len + 4 > max_len) return len;
        int32_t rel = *(const int32_t *)(p + len);
        len += 4;
        sprintf(out_text, "jmp    0x%x", vaddr + len + rel);
        return len;
    }
    if (op == 0xEB) { // JMP rel8
        if (len >= max_len) return len;
        int8_t rel = (int8_t)p[len++];
        sprintf(out_text, "jmp    0x%x", vaddr + len + rel);
        return len;
    }

    // Jcc rel8 (0x70 .. 0x7F)
    const char *jcc_names[] = {
        "jo", "jno", "jb", "jnb", "jz", "jnz", "jbe", "ja",
        "js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg"
    };
    if (op >= 0x70 && op <= 0x7F) {
        if (len >= max_len) return len;
        int8_t rel = (int8_t)p[len++];
        sprintf(out_text, "%-6s 0x%x", jcc_names[op - 0x70], vaddr + len + rel);
        return len;
    }

    // 2-byte opcode prefix (0x0F)
    if (op == 0x0F) {
        if (len >= max_len) return len;
        uint8_t op2 = p[len++];
        if (op2 >= 0x80 && op2 <= 0x8F) { // Jcc rel32
            if (len + 4 > max_len) return len;
            int32_t rel = *(const int32_t *)(p + len);
            len += 4;
            sprintf(out_text, "%-6s 0x%x", jcc_names[op2 - 0x80], vaddr + len + rel);
            return len;
        }
        if (op2 == 0xB6 || op2 == 0xB7) { // movzbl, movzwl
            if (len >= max_len) return len;
            uint8_t modrm = p[len++];
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t reg = (modrm >> 3) & 7;
            uint8_t rm  = modrm & 7;
            if (mod == 3) {
                sprintf(out_text, "%s %s,%s", (op2 == 0xB6) ? "movzbl" : "movzwl",
                        (op2 == 0xB6) ? reg8_names[rm] : reg16_names[rm], reg32_names[reg]);
            } else if (mod == 1) {
                int8_t disp = (int8_t)p[len++];
                sprintf(out_text, "%s 0x%x(%s),%s", (op2 == 0xB6) ? "movzbl" : "movzwl",
                        disp, reg32_names[rm], reg32_names[reg]);
            } else if (mod == 2) {
                int32_t disp = *(const int32_t *)(p + len); len += 4;
                sprintf(out_text, "%s 0x%x(%s),%s", (op2 == 0xB6) ? "movzbl" : "movzwl",
                        disp, reg32_names[rm], reg32_names[reg]);
            } else {
                sprintf(out_text, "%s (%s),%s", (op2 == 0xB6) ? "movzbl" : "movzwl",
                        reg32_names[rm], reg32_names[reg]);
            }
            return len;
        }
        if (op2 == 0xAF) { // imul reg, r/m
            if (len >= max_len) return len;
            uint8_t modrm = p[len++];
            sprintf(out_text, "imul   %s,%s", reg32_names[modrm & 7], reg32_names[(modrm >> 3) & 7]);
            return len;
        }
    }

    // Common ModR/M opcodes
    const char *opname = NULL;
    int dir_reg_to_rm = 0;
    if (op == 0x89) { opname = "mov"; dir_reg_to_rm = 1; }
    else if (op == 0x8B) { opname = "mov"; dir_reg_to_rm = 0; }
    else if (op == 0x01) { opname = "add"; dir_reg_to_rm = 1; }
    else if (op == 0x03) { opname = "add"; dir_reg_to_rm = 0; }
    else if (op == 0x29) { opname = "sub"; dir_reg_to_rm = 1; }
    else if (op == 0x2B) { opname = "sub"; dir_reg_to_rm = 0; }
    else if (op == 0x31) { opname = "xor"; dir_reg_to_rm = 1; }
    else if (op == 0x33) { opname = "xor"; dir_reg_to_rm = 0; }
    else if (op == 0x39) { opname = "cmp"; dir_reg_to_rm = 1; }
    else if (op == 0x3B) { opname = "cmp"; dir_reg_to_rm = 0; }
    else if (op == 0x8D) { opname = "lea"; dir_reg_to_rm = 0; }

    if (opname) {
        if (len >= max_len) return len;
        uint8_t modrm = p[len++];
        uint8_t mod = (modrm >> 6) & 3;
        uint8_t reg = (modrm >> 3) & 7;
        uint8_t rm  = modrm & 7;

        char rm_str[64];
        if (mod == 3) {
            strcpy(rm_str, reg32_names[rm]);
        } else {
            if (rm == 4) { // SIB byte follows
                if (len >= max_len) return len;
                uint8_t sib = p[len++];
                uint8_t base = sib & 7;
                uint8_t index = (sib >> 3) & 7;
                uint8_t scale = 1 << ((sib >> 6) & 3);
                if (index == 4) sprintf(rm_str, "(%s)", reg32_names[base]);
                else sprintf(rm_str, "(%s,%s,%u)", reg32_names[base], reg32_names[index], scale);
            } else if (mod == 0) {
                if (rm == 5) { // disp32
                    if (len + 4 > max_len) return len;
                    uint32_t d = *(const uint32_t *)(p + len); len += 4;
                    sprintf(rm_str, "0x%x", d);
                } else {
                    sprintf(rm_str, "(%s)", reg32_names[rm]);
                }
            } else if (mod == 1) { // disp8
                if (len >= max_len) return len;
                int8_t d = (int8_t)p[len++];
                if (d < 0) sprintf(rm_str, "-0x%x(%s)", -d, reg32_names[rm]);
                else sprintf(rm_str, "0x%x(%s)", d, reg32_names[rm]);
            } else if (mod == 2) { // disp32
                if (len + 4 > max_len) return len;
                int32_t d = *(const int32_t *)(p + len); len += 4;
                sprintf(rm_str, "0x%x(%s)", d, reg32_names[rm]);
            }
        }

        if (dir_reg_to_rm) {
            sprintf(out_text, "%-6s %s,%s", opname, reg32_names[reg], rm_str);
        } else {
            sprintf(out_text, "%-6s %s,%s", opname, rm_str, reg32_names[reg]);
        }
        return len;
    }

    // 0x83: op r/m32, imm8
    if (op == 0x83) {
        if (len >= max_len) return len;
        uint8_t modrm = p[len++];
        uint8_t subop = (modrm >> 3) & 7;
        uint8_t mod   = (modrm >> 6) & 3;
        uint8_t rm    = modrm & 7;
        const char *mnem[] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
        if (len >= max_len) return len;
        int8_t imm = (int8_t)p[len++];
        if (mod == 3) {
            sprintf(out_text, "%-6s $0x%x,%s", mnem[subop], (uint32_t)(int32_t)imm, reg32_names[rm]);
        } else {
            sprintf(out_text, "%-6s $0x%x,(%s)", mnem[subop], (uint32_t)(int32_t)imm, reg32_names[rm]);
        }
        return len;
    }

    // Fallback: raw byte
    sprintf(out_text, ".byte  0x%02x", op);
    return len;
}

static void dump_disassembly(const uint8_t *raw, const Elf32_Ehdr *eh) {
    int disassembled_any = 0;

    if (eh->e_shnum > 0 && eh->e_shoff > 0) {
        const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(raw + eh->e_shoff);
        const char *shstrtab = (eh->e_shstrndx < eh->e_shnum) ? (const char *)(raw + shdrs[eh->e_shstrndx].sh_offset) : "";

        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            const Elf32_Shdr *s = &shdrs[i];
            if (!(s->sh_flags & SHF_EXECINSTR) || s->sh_size == 0) continue;
            const char *name = (eh->e_shstrndx < eh->e_shnum && s->sh_name < shdrs[eh->e_shstrndx].sh_size) ? &shstrtab[s->sh_name] : "";
            printf("Disassembly of section %s:\n\n", name);

            const uint8_t *code = raw + s->sh_offset;
            uint32_t offset = 0;
            while (offset < s->sh_size) {
                char asm_buf[128];
                uint32_t cur_addr = s->sh_addr + offset;
                int inst_len = disassemble_instruction(code + offset, cur_addr, s->sh_size - offset, asm_buf);
                if (inst_len <= 0) break;

                printf(" %8x:\t", cur_addr);
                for (int b = 0; b < 7; b++) {
                    if (b < inst_len) printf("%02x ", code[offset + b]);
                    else printf("   ");
                }
                printf("\t%s\n", asm_buf);
                offset += inst_len;
            }
            printf("\n");
            disassembled_any = 1;
        }
    }

    if (!disassembled_any && eh->e_phnum > 0 && eh->e_phoff > 0) {
        const Elf32_Phdr *phdrs = (const Elf32_Phdr *)(raw + eh->e_phoff);
        for (uint16_t i = 0; i < eh->e_phnum; i++) {
            const Elf32_Phdr *p = &phdrs[i];
            if (p->p_type == PT_LOAD && (p->p_flags & PF_X) && p->p_filesz > 0) {
                printf("Disassembly of segment %u (.text fallback at 0x%08x):\n\n", i, p->p_vaddr);
                const uint8_t *code = raw + p->p_offset;
                uint32_t offset = 0;
                if (p->p_offset == 0 && eh->e_entry >= p->p_vaddr && eh->e_entry < p->p_vaddr + p->p_filesz) {
                    offset = eh->e_entry - p->p_vaddr;
                }
                while (offset < p->p_filesz) {
                    char asm_buf[128];
                    uint32_t cur_addr = p->p_vaddr + offset;
                    int inst_len = disassemble_instruction(code + offset, cur_addr, p->p_filesz - offset, asm_buf);
                    if (inst_len <= 0) break;

                    printf(" %8x:\t", cur_addr);
                    for (int b = 0; b < 7; b++) {
                        if (b < inst_len) printf("%02x ", code[offset + b]);
                        else printf("   ");
                    }
                    printf("\t%s\n", asm_buf);
                    offset += inst_len;
                }
                printf("\n");
                disassembled_any = 1;
            }
        }
    }
}

int main(int argc, char **argv) {
    const char *target_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file-headers") == 0) {
            flag_file_headers = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--section-headers") == 0 || strcmp(argv[i], "--headers") == 0) {
            flag_section_headers = 1;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--syms") == 0) {
            flag_syms = 1;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--full-contents") == 0) {
            flag_full_contents = 1;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--disassemble") == 0) {
            flag_disassemble = 1;
        } else if (strcmp(argv[i], "-x") == 0 || strcmp(argv[i], "--all-headers") == 0) {
            flag_file_headers = flag_section_headers = flag_syms = 1;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--archive-headers") == 0) {
            // archive headers
        } else if (argv[i][0] == '-') {
            // parse concatenated flags like -d -s
            for (char *c = &argv[i][1]; *c; c++) {
                if (*c == 'f') flag_file_headers = 1;
                else if (*c == 'h') flag_section_headers = 1;
                else if (*c == 't') flag_syms = 1;
                else if (*c == 's') flag_full_contents = 1;
                else if (*c == 'd') flag_disassemble = 1;
                else if (*c == 'x') flag_file_headers = flag_section_headers = flag_syms = 1;
            }
        } else {
            target_file = argv[i];
        }
    }

    if (!target_file) {
        fprintf(stderr, "Usage: objdump [-f] [-h] [-t] [-s] [-d] [-x] <elf-file>\n");
        return 1;
    }

    if (!flag_file_headers && !flag_section_headers && !flag_syms && !flag_full_contents && !flag_disassemble) {
        flag_section_headers = 1; // default
    }

    FILE *f = fopen(target_file, "rb");
    if (!f) {
        perror("objdump: fopen");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *raw = (uint8_t *)malloc(sz);
    if (!raw) {
        fprintf(stderr, "objdump: out of memory\n");
        fclose(f);
        return 1;
    }
    if (fread(raw, 1, sz, f) != (size_t)sz) {
        fprintf(stderr, "objdump: read failed\n");
        free(raw);
        fclose(f);
        return 1;
    }
    fclose(f);

    if (sz < (long)sizeof(Elf32_Ehdr)) {
        fprintf(stderr, "objdump: %s: file too small\n", target_file);
        free(raw);
        return 1;
    }

    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)raw;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "objdump: %s: file format not recognized\n", target_file);
        free(raw);
        return 1;
    }

    if (flag_file_headers) dump_file_header(eh, target_file);
    if (flag_section_headers) dump_section_headers(raw, eh);
    if (flag_syms) dump_symbols(raw, eh);
    if (flag_full_contents) dump_full_contents(raw, eh);
    if (flag_disassemble) dump_disassembly(raw, eh);

    free(raw);
    return 0;
}
