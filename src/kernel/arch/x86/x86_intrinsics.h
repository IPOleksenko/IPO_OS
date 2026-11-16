#ifndef IPO_X86_INTRINSICS_H
#define IPO_X86_INTRINSICS_H

#include <types.h>

/*
 * Private inline assembly helpers for this arch backend only.
 * Do not include from src/include/ or arch-independent subsystems.
 */

#if defined(__x86_64__)
#define X86_REG_OP(insn32, insn64) insn64
#else
#define X86_REG_OP(insn32, insn64) insn32
#endif

static inline ipo_reg_t x86_read_cr0(void) {
    ipo_reg_t v;
    __asm__ volatile(X86_REG_OP("movl %%cr0, %0", "movq %%cr0, %0") : "=r"(v));
    return v;
}

static inline void x86_write_cr0(ipo_reg_t v) {
    __asm__ volatile(X86_REG_OP("movl %0, %%cr0", "movq %0, %%cr0") : : "r"(v) : "memory");
}

static inline ipo_vaddr_t x86_read_cr2(void) {
    ipo_vaddr_t v;
    __asm__ volatile(X86_REG_OP("movl %%cr2, %0", "movq %%cr2, %0") : "=r"(v));
    return v;
}

static inline ipo_reg_t x86_read_cr3(void) {
    ipo_reg_t v;
    __asm__ volatile(X86_REG_OP("movl %%cr3, %0", "movq %%cr3, %0") : "=r"(v));
    return v;
}

static inline void x86_write_cr3(ipo_reg_t v) {
    __asm__ volatile(X86_REG_OP("movl %0, %%cr3", "movq %0, %%cr3") : : "r"(v) : "memory");
}

static inline ipo_reg_t x86_read_cr4(void) {
    ipo_reg_t v;
    __asm__ volatile(X86_REG_OP("movl %%cr4, %0", "movq %%cr4, %0") : "=r"(v));
    return v;
}

static inline void x86_write_cr4(ipo_reg_t v) {
    __asm__ volatile(X86_REG_OP("movl %0, %%cr4", "movq %0, %%cr4") : : "r"(v) : "memory");
}

static inline ipo_reg_t x86_read_flags(void) {
    ipo_reg_t v;
    __asm__ volatile(X86_REG_OP("pushfl; popl %0", "pushfq; popq %0") : "=r"(v));
    return v;
}

static inline void x86_write_flags(ipo_reg_t v) {
    __asm__ volatile(X86_REG_OP("pushl %0; popfl", "pushq %0; popfq") : : "r"(v) : "memory", "cc");
}

static inline ipo_vaddr_t x86_read_sp(void) {
    ipo_vaddr_t v;
    __asm__ volatile(X86_REG_OP("movl %%esp, %0", "movq %%rsp, %0") : "=r"(v));
    return v;
}

static inline void x86_write_sp(ipo_vaddr_t v) {
    __asm__ volatile(X86_REG_OP("movl %0, %%esp", "movq %0, %%rsp") : : "r"(v) : "memory");
}

static inline ipo_reg_t x86_irq_save(void) {
    ipo_reg_t flags;
    __asm__ volatile(X86_REG_OP("pushfl; popl %0; cli", "pushfq; popq %0; cli")
                     : "=r"(flags) : : "memory");
    return flags;
}

static inline void x86_irq_restore(ipo_reg_t flags) {
    x86_write_flags(flags);
}

static inline void x86_irq_enable(void) {
    __asm__ volatile("sti" ::: "memory");
}

static inline void x86_irq_disable(void) {
    __asm__ volatile("cli" ::: "memory");
}

static inline void x86_clts(void) {
    __asm__ volatile("clts" ::: "memory");
}

static inline void x86_fninit(void) {
    __asm__ volatile("fninit");
}

static inline void x86_pause(void) {
    __asm__ volatile("pause" ::: "memory");
}

static inline void x86_halt(void) {
    __asm__ volatile("hlt" ::: "memory");
}

static inline void x86_invlpg(ipo_vaddr_t vaddr) {
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

static inline void x86_cpuid(uint32_t leaf, uint32_t subleaf,
                             uint32_t *eax, uint32_t *ebx,
                             uint32_t *ecx, uint32_t *edx) {
    uint32_t a = leaf, b = 0u, c = subleaf, d = 0u;
    __asm__ volatile("cpuid"
                     : "+a"(a), "=b"(b), "+c"(c), "=d"(d)
                     :
                     : "memory");
    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

static inline bool x86_has_cpuid(void) {
    ipo_reg_t f1 = x86_read_flags();
    ipo_reg_t toggled = f1 ^ (ipo_reg_t)0x00200000u;
    x86_write_flags(toggled);
    ipo_reg_t f2 = x86_read_flags();
    x86_write_flags(f1);
    return ((f1 ^ f2) & (ipo_reg_t)0x00200000u) != 0u;
}

static inline uint64_t x86_rdmsr(uint32_t msr) {
    uint32_t lo = 0u, hi = 0u;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline void x86_wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFFULL);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

static inline uint16_t x86_read_cs(void) {
    uint16_t cs = 0u;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    return cs;
}

static inline uint64_t x86_rdtsc(void) {
    uint32_t lo = 0u, hi = 0u;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

#endif /* IPO_X86_INTRINSICS_H */
