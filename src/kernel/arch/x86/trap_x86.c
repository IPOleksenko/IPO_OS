#include <syscall.h>
#include <kernel/process.h>
#include <kernel/smp.h>
#include <kernel/terminal.h>
#include <memory/address_space.h>
#include <memory/kmalloc.h>
#include <arch/cpu_regs.h>
#include <ioport.h>
#include <vga.h>
#include <vga_gfx.h>
#include <wm.h>
#include <stdio.h>
#include <string.h>
#include "x86_intrinsics.h"

#define IPO_IDT_ENTRY_FLAGS    0xEEu
#define IPO_KERNEL_CODE_SEG    0x08u
#define IPO_KERNEL_CODE64_SEG  0x38u

typedef struct {
    uint16_t base_lo;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t base_hi;
} __attribute__((packed)) ipo_idt_entry32_t;

typedef struct {
    uint16_t base_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t base_mid;
    uint32_t base_hi;
    uint32_t reserved;
} __attribute__((packed)) ipo_idt_entry64_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) ipo_idt_ptr32_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) ipo_idt_ptr64_t;

#define X86_IDT_ENTRY_COUNT        256u
#define X86_GDT_ENTRY_COUNT        8u
#define X86_TLS_WORDS_COUNT        16u
#define X86_PF_STACK_SIZE          16384u
#define X86_DF_STACK_SIZE          8192u

static ipo_idt_entry32_t *ipo_idt_table32 = NULL;
static ipo_idt_entry64_t *ipo_idt_table64 = NULL;

extern void syscall_isr_entry(void);
extern void lm64_syscall_isr_entry(void);
extern ipo_vaddr_t isr_stub_table[];
extern ipo_vaddr_t lm64_isr_stub_table[];
extern void irq0_isr(void);
extern void lm64_irq0_isr(void);
extern ipo_vaddr_t irq_stub_table[];
extern ipo_vaddr_t lm64_irq_stub_table[];
extern void page_fault_task_entry(void);
extern void lm64_page_fault_entry(void);
extern void double_fault_task_entry(void);
extern void lm64_double_fault_entry(void);
extern void smp_ipi_isr(void);
extern void lm64_smp_ipi_isr(void);
extern void x86_load_tables_lm64(const void *gdt_ptr10, const void *idt_ptr10, uint32_t load_tr);

static uint32_t *kernel_gs_tls_area = NULL;
static uint64_t *kernel_gdt = NULL;

typedef struct __attribute__((packed)) {
    uint16_t backlink;
    uint16_t reserved0;
    uint32_t esp0;
    uint16_t ss0;
    uint16_t reserved1;
    uint32_t esp1;
    uint16_t ss1;
    uint16_t reserved2;
    uint32_t esp2;
    uint16_t ss2;
    uint16_t reserved3;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint16_t es;
    uint16_t reserved4;
    uint16_t cs;
    uint16_t reserved5;
    uint16_t ss;
    uint16_t reserved6;
    uint16_t ds;
    uint16_t reserved7;
    uint16_t fs;
    uint16_t reserved8;
    uint16_t gs;
    uint16_t reserved9;
    uint16_t ldt;
    uint16_t reserved10;
    uint16_t trap;
    uint16_t iomap_base;
} x86_tss32_t;

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} x86_tss64_t;

typedef struct {
    uint64_t          *gdt;
    ipo_idt_entry32_t *idt32;
    ipo_idt_entry64_t *idt64;
    x86_tss32_t        current_tss;
    x86_tss32_t        page_fault_tss;
    x86_tss32_t        double_fault_tss;
    x86_tss64_t        tss64;
    uint8_t           *page_fault_stack;
    size_t             page_fault_stack_size;
    uint8_t           *double_fault_stack;
    size_t             double_fault_stack_size;
} cpu_exception_tables_t;

#define X86_TSS_CURRENT_SELECTOR      0x20u
#define X86_TSS_PAGE_FAULT_SELECTOR   0x28u
#define X86_TSS_DOUBLE_FAULT_SELECTOR 0x30u

static cpu_exception_tables_t *cpu_exception_tables = NULL;
static uint32_t cpu_exception_table_count = 0u;

static bool trap_is_long_mode(void) {
    return arch_mmu_get_paging_levels() >= 4u;
}

static const char *x86_exception_name(uint32_t vector) {
    switch (vector) {
        case 0:  return "Divide-by-zero (#DE)";
        case 1:  return "Debug (#DB)";
        case 2:  return "Non-maskable Interrupt (#NMI)";
        case 3:  return "Breakpoint (#BP)";
        case 4:  return "Overflow (#OF)";
        case 5:  return "Bound Range Exceeded (#BR)";
        case 6:  return "Invalid Opcode (#UD)";
        case 7:  return "Device Not Available (#NM)";
        case 8:  return "Double Fault (#DF)";
        case 9:  return "Coprocessor Segment Overrun";
        case 10: return "Invalid TSS (#TS)";
        case 11: return "Segment Not Present (#NP)";
        case 12: return "Stack-Segment Fault (#SS)";
        case 13: return "General Protection Fault (#GP)";
        case 14: return "Page Fault (#PF)";
        case 16: return "x87 FPU Error (#MF)";
        case 17: return "Alignment Check (#AC)";
        case 18: return "Machine Check (#MC)";
        case 19: return "SIMD Floating-Point (#XM)";
        case 20: return "Virtualization (#VE)";
        case 21: return "Control Protection (#CP)";
        case 28: return "Hypervisor Injection";
        case 29: return "VMM Communication";
        case 30: return "Security Exception";
        default: return (vector < 32u) ? "Reserved" : "Unknown Exception";
    }
}

static void ipo_idt64_encode_gate(ipo_idt_entry64_t *entry, ipo_vaddr_t base,
                                  uint16_t selector, uint8_t flags, uint8_t ist) {
    uint64_t addr = (uint64_t)base;
    entry->base_lo = (uint16_t)(addr & 0xFFFFu);
    entry->selector = selector;
    entry->ist = (uint8_t)(ist & 0x07u);
    entry->flags = flags;
    entry->base_mid = (uint16_t)((addr >> 16) & 0xFFFFu);
    entry->base_hi = (uint32_t)((addr >> 32) & 0xFFFFFFFFu);
    entry->reserved = 0u;
}

static void x86_tss64_descriptor(uint64_t *slot, const x86_tss64_t *tss) {
    uint64_t base = (uint64_t)(uintptr_t)tss;
    uint64_t limit = (uint64_t)(sizeof(*tss) - 1u);
    slot[0] = (limit & 0xFFFFULL) |
              ((base & 0xFFFFULL) << 16) |
              (((base >> 16) & 0xFFULL) << 32) |
              (0x89ULL << 40) |
              (((limit >> 16) & 0x0FULL) << 48) |
              (((base >> 24) & 0xFFULL) << 56);
    slot[1] = (base >> 32) & 0xFFFFFFFFULL;
}

static void x86_tss64_initialize(x86_tss64_t *tss, ipo_vaddr_t pf_stack_top, ipo_vaddr_t df_stack_top) {
    memset(tss, 0, sizeof(*tss));
    tss->ist1 = (uint64_t)pf_stack_top;
    tss->ist2 = (uint64_t)df_stack_top;
    tss->iomap_base = (uint16_t)sizeof(*tss);
}

static uint64_t x86_tss_descriptor(const x86_tss32_t *tss) {
    uint32_t base = (uint32_t)(uintptr_t)tss;
    uint32_t limit = sizeof(*tss) - 1u;
    return (uint64_t)(limit & 0xFFFFu) |
           ((uint64_t)(base & 0xFFFFu) << 16) |
           ((uint64_t)((base >> 16) & 0xFFu) << 32) |
           ((uint64_t)0x89u << 40) |
           ((uint64_t)((limit >> 16) & 0x0Fu) << 48) |
           ((uint64_t)((base >> 24) & 0xFFu) << 56);
}

static void x86_tss_initialize(x86_tss32_t *tss, uint32_t cr3, uint32_t eip, uint32_t esp) {
    memset(tss, 0, sizeof(*tss));
    tss->cr3 = cr3;
    tss->eip = eip;
    tss->esp = esp;
    tss->eflags = 0x2u;
    tss->cs = 0x08u;
    tss->ss = 0x10u;
    tss->ss0 = 0x10u;
    tss->esp0 = esp;
    tss->ds = 0x10u;
    tss->es = 0x10u;
    tss->fs = 0x10u;
    tss->gs = 0x18u;
    tss->iomap_base = sizeof(*tss);
}

static void x86_idt_set_task_gate(ipo_idt_entry32_t *idt, uint8_t vector, uint16_t selector) {
    idt[vector].base_lo = 0u;
    idt[vector].selector = selector;
    idt[vector].zero = 0u;
    idt[vector].flags = 0x85u;
    idt[vector].base_hi = 0u;
}

static void ipo_idt_set_gate_pair(uint8_t num, ipo_vaddr_t base32, ipo_vaddr_t base64, uint8_t flags) {
    uint32_t addr = (uint32_t)base32;
    ipo_idt_table32[num].base_lo = (uint16_t)(addr & 0xFFFFu);
    ipo_idt_table32[num].selector = IPO_KERNEL_CODE_SEG;
    ipo_idt_table32[num].zero = 0u;
    ipo_idt_table32[num].flags = flags;
    ipo_idt_table32[num].base_hi = (uint16_t)((addr >> 16) & 0xFFFFu);

    ipo_idt64_encode_gate(&ipo_idt_table64[num], base64, IPO_KERNEL_CODE64_SEG, flags, 0u);

    if (cpu_exception_tables) {
        for (uint32_t i = 0u; i < cpu_exception_table_count; i++) {
            if (num != 8u && num != 14u) {
                cpu_exception_tables[i].idt32[num] = ipo_idt_table32[num];
                cpu_exception_tables[i].idt64[num] = ipo_idt_table64[num];
            }
        }
    }
}

void fpu_init(void) {
    arch_cpu_enable_fpu_simd();
}

void syscall_load_idt(void) {
    if (!ipo_idt_table64 || !ipo_idt_table32) return;
    if (trap_is_long_mode()) {
        ipo_idt_ptr64_t idt_ptr = {
            .limit = (uint16_t)(X86_IDT_ENTRY_COUNT * sizeof(ipo_idt_entry64_t) - 1u),
            .base = (uint64_t)(uintptr_t)ipo_idt_table64
        };
        x86_load_tables_lm64(NULL, &idt_ptr, 0u);
    } else {
        ipo_idt_ptr32_t idt_ptr = {
            .limit = (uint16_t)(X86_IDT_ENTRY_COUNT * sizeof(ipo_idt_entry32_t) - 1u),
            .base = (uint32_t)(uintptr_t)ipo_idt_table32
        };
        __asm__ volatile("lidt %0" : : "m"(idt_ptr));
    }
}

bool syscall_init_cpu(uint32_t cpu_id) {
    if (!cpu_exception_tables || cpu_id >= cpu_exception_table_count) {
        return false;
    }

    fpu_init();

    cpu_exception_tables_t *tables = &cpu_exception_tables[cpu_id];
    if (!tables->gdt || !tables->idt64 || !tables->idt32) {
        return false;
    }
    if (trap_is_long_mode()) {
        x86_tss64_descriptor(&tables->gdt[4], &tables->tss64);
        tables->gdt[6] = 0ULL;
        ipo_idt_ptr64_t gdt_ptr = {
            .limit = (uint16_t)(X86_GDT_ENTRY_COUNT * sizeof(uint64_t) - 1u),
            .base = (uint64_t)(uintptr_t)tables->gdt
        };
        ipo_idt_ptr64_t idt_ptr = {
            .limit = (uint16_t)(X86_IDT_ENTRY_COUNT * sizeof(ipo_idt_entry64_t) - 1u),
            .base = (uint64_t)(uintptr_t)tables->idt64
        };
        x86_load_tables_lm64(&gdt_ptr, &idt_ptr, 1u);
        arch_cpu_set_tls_base((ipo_vaddr_t)(uintptr_t)kernel_gs_tls_area);
    } else {
        tables->gdt[4] = x86_tss_descriptor(&tables->current_tss);
        tables->gdt[5] = x86_tss_descriptor(&tables->page_fault_tss);
        tables->gdt[6] = x86_tss_descriptor(&tables->double_fault_tss);
        ipo_idt_ptr32_t gdt_ptr = {
            .limit = (uint16_t)(X86_GDT_ENTRY_COUNT * sizeof(uint64_t) - 1u),
            .base = (uint32_t)(uintptr_t)tables->gdt
        };
        ipo_idt_ptr32_t idt_ptr = {
            .limit = (uint16_t)(X86_IDT_ENTRY_COUNT * sizeof(ipo_idt_entry32_t) - 1u),
            .base = (uint32_t)(uintptr_t)tables->idt32
        };

        __asm__ volatile(
            "lgdt %0\n\t"
            "ljmp $0x08, $1f\n\t"
            "1:\n\t"
            "movw $0x10, %%ax\n\t"
            "movw %%ax, %%ds\n\t"
            "movw %%ax, %%es\n\t"
            "movw %%ax, %%fs\n\t"
            "movw %%ax, %%ss\n\t"
            "movw $0x18, %%ax\n\t"
            "movw %%ax, %%gs\n\t"
            "lidt %1\n\t"
            "movw $0x20, %%ax\n\t"
            "ltr %%ax"
            :
            : "m"(gdt_ptr), "m"(idt_ptr)
            : "eax", "memory"
        );
    }

    return true;
}

static void free_cpu_exception_tables_array(cpu_exception_tables_t *tables, uint32_t count) {
    if (!tables) return;
    for (uint32_t i = 0u; i < count; i++) {
        if (tables[i].gdt) kfree(tables[i].gdt);
        if (tables[i].idt32) kfree(tables[i].idt32);
        if (tables[i].idt64) kfree(tables[i].idt64);
        if (tables[i].page_fault_stack) kfree(tables[i].page_fault_stack);
        if (tables[i].double_fault_stack) kfree(tables[i].double_fault_stack);
    }
    kfree(tables);
}

bool syscall_prepare_cpu_exceptions(uint32_t cpu_count) {
    if (cpu_count == 0u) return false;
    if (cpu_exception_tables && cpu_count <= cpu_exception_table_count) {
        return true;
    }

    cpu_exception_tables_t *old_tables = cpu_exception_tables;
    uint32_t old_count = cpu_exception_table_count;
    cpu_exception_tables_t *tables =
        (cpu_exception_tables_t *)kmalloc(cpu_count * sizeof(*tables));
    if (!tables) return false;
    memset(tables, 0, cpu_count * sizeof(*tables));

    uint32_t cr3 = (uint32_t)x86_read_cr3();
    for (uint32_t i = 0u; i < cpu_count; i++) {
        cpu_exception_tables_t *cpu = &tables[i];
        cpu->gdt = (uint64_t *)kmalloc(X86_GDT_ENTRY_COUNT * sizeof(uint64_t));
        cpu->idt32 = (ipo_idt_entry32_t *)kmalloc(X86_IDT_ENTRY_COUNT * sizeof(ipo_idt_entry32_t));
        cpu->idt64 = (ipo_idt_entry64_t *)kmalloc(X86_IDT_ENTRY_COUNT * sizeof(ipo_idt_entry64_t));
        cpu->page_fault_stack_size = X86_PF_STACK_SIZE;
        cpu->double_fault_stack_size = X86_DF_STACK_SIZE;
        cpu->page_fault_stack = (uint8_t *)kmalloc(cpu->page_fault_stack_size);
        cpu->double_fault_stack = (uint8_t *)kmalloc(cpu->double_fault_stack_size);
        if (!cpu->gdt || !cpu->idt32 || !cpu->idt64 ||
            !cpu->page_fault_stack || !cpu->double_fault_stack) {
            free_cpu_exception_tables_array(tables, cpu_count);
            return false;
        }
        memset(cpu->page_fault_stack, 0, cpu->page_fault_stack_size);
        memset(cpu->double_fault_stack, 0, cpu->double_fault_stack_size);
        memcpy(cpu->gdt, kernel_gdt, X86_GDT_ENTRY_COUNT * sizeof(uint64_t));

        ipo_vaddr_t pf_top = (ipo_vaddr_t)(uintptr_t)&cpu->page_fault_stack[cpu->page_fault_stack_size];
        ipo_vaddr_t df_top = (ipo_vaddr_t)(uintptr_t)&cpu->double_fault_stack[cpu->double_fault_stack_size];

        cpu->gdt[4] = x86_tss_descriptor(&cpu->current_tss);
        cpu->gdt[5] = x86_tss_descriptor(&cpu->page_fault_tss);
        cpu->gdt[6] = x86_tss_descriptor(&cpu->double_fault_tss);
        x86_tss_initialize(&cpu->current_tss, cr3, 0u, 0u);
        x86_tss_initialize(&cpu->page_fault_tss, cr3,
                           (uint32_t)(uintptr_t)page_fault_task_entry,
                           (uint32_t)pf_top);
        x86_tss_initialize(&cpu->double_fault_tss, cr3,
                           (uint32_t)(uintptr_t)double_fault_task_entry,
                           (uint32_t)df_top);
        x86_tss64_initialize(&cpu->tss64, pf_top, df_top);

        memcpy(cpu->idt32, ipo_idt_table32, X86_IDT_ENTRY_COUNT * sizeof(ipo_idt_entry32_t));
        x86_idt_set_task_gate(cpu->idt32, 8, X86_TSS_DOUBLE_FAULT_SELECTOR);
        x86_idt_set_task_gate(cpu->idt32, 14, X86_TSS_PAGE_FAULT_SELECTOR);

        memcpy(cpu->idt64, ipo_idt_table64, X86_IDT_ENTRY_COUNT * sizeof(ipo_idt_entry64_t));
        ipo_idt64_encode_gate(&cpu->idt64[8], (ipo_vaddr_t)(uintptr_t)lm64_double_fault_entry,
                              IPO_KERNEL_CODE64_SEG, 0x8Eu, 2u);
        ipo_idt64_encode_gate(&cpu->idt64[14], (ipo_vaddr_t)(uintptr_t)lm64_page_fault_entry,
                              IPO_KERNEL_CODE64_SEG, 0x8Eu, 1u);
    }

    cpu_exception_tables = tables;
    cpu_exception_table_count = cpu_count;
    if (!syscall_init_cpu(0u)) {
        cpu_exception_tables = old_tables;
        cpu_exception_table_count = old_count;
        free_cpu_exception_tables_array(tables, cpu_count);
        return false;
    }

    if (old_tables) {
        free_cpu_exception_tables_array(old_tables, old_count);
    }

    return true;
}

void syscall_update_exception_page_directory(void) {
    if (!cpu_exception_tables) return;
    if (trap_is_long_mode()) return;
    uint32_t cr3 = (uint32_t)x86_read_cr3();
    for (uint32_t i = 0u; i < cpu_exception_table_count; i++) {
        cpu_exception_tables[i].current_tss.cr3 = cr3;
        cpu_exception_tables[i].page_fault_tss.cr3 = cr3;
        cpu_exception_tables[i].double_fault_tss.cr3 = cr3;
    }
}

static void __attribute__((noreturn)) page_fault_terminate_process(void) {
    process_emergency_crash_exit();
    arch_cpu_halt_forever();
}

void cpu_page_fault_lm64_handler(uint32_t error_code, uint64_t *iret_frame) {
    ipo_vaddr_t cr2 = x86_read_cr2();
    ipo_vaddr_t rip = iret_frame ? (ipo_vaddr_t)iret_frame[0] : 0u;
    ipo_vaddr_t rsp = iret_frame ? (ipo_vaddr_t)iret_frame[3] : 0u;
    uint32_t cpu_id = smp_get_current_cpu_id();
    process_t *proc = process_get_current();
    cpu_exception_tables_t *tables =
        (cpu_id < cpu_exception_table_count) ? &cpu_exception_tables[cpu_id] : NULL;

    if (address_space_handle_trampoline_fault(cr2, error_code, rsp)) {
        return;
    }
    if (proc && proc->current_thread &&
        process_thread_handle_stack_fault(proc, proc->current_thread, cr2, error_code, rsp)) {
        return;
    }

    uint32_t tid = (proc && proc->current_thread) ? proc->current_thread->id : 0u;
    serial_printf("[page-fault] DIAGNOSTIC: cpu=%u pid=%u tid=%u cr2=0x%08x eip=0x%08x esp=0x%08x err=0x%x\n",
                  cpu_id, proc ? proc->pid : 0u, tid, (uint32_t)cr2, (uint32_t)rip, (uint32_t)rsp, error_code);
    if (!proc || !iret_frame) {
        arch_cpu_halt_forever();
    }

    process_thread_main_exit(proc);
    proc->is_running = 0u;
    iret_frame[0] = (uint64_t)(uintptr_t)page_fault_terminate_process;
    iret_frame[1] = 0x08ULL;
    iret_frame[2] = 0x02ULL;
    if (tables && tables->page_fault_stack && tables->page_fault_stack_size >= 64u) {
        iret_frame[3] = (uint64_t)(uintptr_t)&tables->page_fault_stack[tables->page_fault_stack_size - 64u];
        iret_frame[4] = 0x10ULL;
    }
}

void cpu_page_fault_task_handler(uint32_t error_code) {
    uint32_t cr2 = (uint32_t)x86_read_cr2();
    uint32_t cpu_id = smp_get_current_cpu_id();
    process_t *proc = process_get_current();
    cpu_exception_tables_t *tables =
        (cpu_id < cpu_exception_table_count) ? &cpu_exception_tables[cpu_id] : NULL;

    if (tables &&
        address_space_handle_trampoline_fault(cr2, error_code, tables->current_tss.esp)) {
        tables->current_tss.cr3 = tables->page_fault_tss.cr3;
        return;
    }

    if (proc && proc->current_thread && tables &&
        process_thread_handle_stack_fault(
            proc, proc->current_thread, cr2, error_code, tables->current_tss.esp)) {
        tables->current_tss.cr3 = tables->page_fault_tss.cr3;
        return;
    }

    uint32_t eip = tables ? tables->current_tss.eip : 0u;
    uint32_t esp = tables ? tables->current_tss.esp : 0u;
    uint32_t tid = (proc && proc->current_thread) ? proc->current_thread->id : 0u;
    serial_printf("[page-fault] DIAGNOSTIC: cpu=%u pid=%u tid=%u cr2=0x%08x eip=0x%08x esp=0x%08x err=0x%x\n",
                   cpu_id, proc ? proc->pid : 0u, tid, cr2, eip, esp, error_code);
    if (tables) {
        serial_printf("[page-fault] REGS: eax=0x%08x ebx=0x%08x ecx=0x%08x edx=0x%08x esi=0x%08x edi=0x%08x ebp=0x%08x eflags=0x%08x cr3=0x%08x cs=0x%x ss=0x%x\n",
                      tables->current_tss.eax, tables->current_tss.ebx, tables->current_tss.ecx,
                      tables->current_tss.edx, tables->current_tss.esi, tables->current_tss.edi,
                      tables->current_tss.ebp, tables->current_tss.eflags, tables->current_tss.cr3,
                      tables->current_tss.cs, tables->current_tss.ss);
    }
    if (!proc || !tables) {
        arch_cpu_halt_forever();
    }

    process_thread_main_exit(proc);
    proc->is_running = 0u;
    tables->current_tss.eip = (uint32_t)(uintptr_t)page_fault_terminate_process;
    tables->current_tss.esp =
        (uint32_t)(uintptr_t)&tables->page_fault_stack[tables->page_fault_stack_size - 64u];
    tables->current_tss.ebp = 0u;
    tables->current_tss.eflags = 0x2u;
    tables->current_tss.cs = 0x08u;
    tables->current_tss.ss = 0x10u;
    tables->current_tss.ds = 0x10u;
    tables->current_tss.es = 0x10u;
    tables->current_tss.fs = 0x10u;
    tables->current_tss.gs = 0x18u;

    tables->page_fault_tss.esp = (uint32_t)(uintptr_t)&tables->page_fault_stack[tables->page_fault_stack_size];
    tables->page_fault_tss.eip = (uint32_t)(uintptr_t)page_fault_task_entry;
    tables->page_fault_tss.cr3 = (uint32_t)x86_read_cr3();
}

void cpu_double_fault_task_handler(void) {
    uint32_t cpu_id = smp_get_current_cpu_id();
    serial_printf("KERNEL PANIC: double fault on CPU %u\n", cpu_id);
    arch_cpu_halt_forever();
}

void cpu_exception_handler(ipo_reg_t *frame) {
    uint32_t vector = (uint32_t)frame[7];
    if (vector == 7u) {
        arch_cpu_clear_fpu_task_switched();
        return;
    }
    ipo_reg_t err_code = frame[8];
    ipo_vaddr_t eip = frame[9];
    ipo_vaddr_t cr2 = (vector == 14u) ? x86_read_cr2() : 0u;
    const char *name = x86_exception_name(vector);

    putchar_unlock();

    process_t *proc = process_get_current();
    if (proc != NULL) {
        if (vga_is_graphics_mode() && !wm_session_active()) {
            vga_set_mode_text_hardware();
        }
        printf("\n[Process %u] Terminated: CPU Exception %u (%s) at 0x%llx (err=0x%llx)\n",
               proc->pid, vector, name,
               (unsigned long long)eip, (unsigned long long)err_code);
        serial_printf("[cpu_exception] Process %u crashed: vec=%u (%s) ip=0x%llx err=0x%llx cr2=0x%llx\n",
                      proc->pid, vector, name,
                      (unsigned long long)eip, (unsigned long long)err_code,
                      (unsigned long long)cr2);
        process_emergency_crash_exit();
        return;
    }

    printf("\nKERNEL PANIC: CPU Exception %u (%s) at 0x%llx (err=0x%llx)\n",
           vector, name, (unsigned long long)eip, (unsigned long long)err_code);
    serial_printf("KERNEL PANIC: CPU Exception %u (%s) at 0x%llx (err=0x%llx cr2=0x%llx)\n",
                  vector, name, (unsigned long long)eip,
                  (unsigned long long)err_code, (unsigned long long)cr2);
    arch_cpu_halt_forever();
}

void syscall_trap_init(void) {
    ipo_idt_table32 = (ipo_idt_entry32_t *)kmalloc(X86_IDT_ENTRY_COUNT * sizeof(ipo_idt_entry32_t));
    ipo_idt_table64 = (ipo_idt_entry64_t *)kmalloc(X86_IDT_ENTRY_COUNT * sizeof(ipo_idt_entry64_t));
    kernel_gs_tls_area = (uint32_t *)kmalloc(X86_TLS_WORDS_COUNT * sizeof(uint32_t));
    kernel_gdt = (uint64_t *)kmalloc(X86_GDT_ENTRY_COUNT * sizeof(uint64_t));
    if (!ipo_idt_table32 || !ipo_idt_table64 || !kernel_gs_tls_area || !kernel_gdt) {
        serial_printf("[syscall] FATAL: Failed to allocate trap descriptor tables\n");
        arch_cpu_halt_forever();
    }

    memset(ipo_idt_table32, 0, X86_IDT_ENTRY_COUNT * sizeof(ipo_idt_entry32_t));
    memset(ipo_idt_table64, 0, X86_IDT_ENTRY_COUNT * sizeof(ipo_idt_entry64_t));
    memset(kernel_gs_tls_area, 0, X86_TLS_WORDS_COUNT * sizeof(uint32_t));
    kernel_gs_tls_area[5] = 0x595e9fbdU;

    kernel_gdt[0] = 0x0000000000000000ULL;
    kernel_gdt[1] = 0x00CF9B000000FFFFULL;
    kernel_gdt[2] = 0x00CF93000000FFFFULL;
    kernel_gdt[3] = 0x00CF93000000FFFFULL;
    kernel_gdt[4] = 0x0000000000000000ULL;
    kernel_gdt[5] = 0x0000000000000000ULL;
    kernel_gdt[6] = 0x0000000000000000ULL;
    kernel_gdt[7] = 0x00AF9B000000FFFFULL;

    for (int i = 0; i < 32; i++) {
        ipo_idt_set_gate_pair((uint8_t)i, isr_stub_table[i], lm64_isr_stub_table[i], IPO_IDT_ENTRY_FLAGS);
    }

    /* Remap PIC to vectors 0x20..0x2F and unmask IRQ0 (PIT) */
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();
    outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xFE); io_wait();
    outb(0xA1, 0xFF); io_wait();

    ipo_idt_set_gate_pair(0x20,
                          (ipo_vaddr_t)(uintptr_t)irq0_isr,
                          (ipo_vaddr_t)(uintptr_t)lm64_irq0_isr,
                          0x8Eu);
    for (int i = 0x21; i <= 0x2F; i++) {
        ipo_idt_set_gate_pair((uint8_t)i,
                              irq_stub_table[i - 0x21],
                              lm64_irq_stub_table[i - 0x21],
                              0x8Eu);
    }

    ipo_idt_set_gate_pair(0x80,
                          (ipo_vaddr_t)(uintptr_t)syscall_isr_entry,
                          (ipo_vaddr_t)(uintptr_t)lm64_syscall_isr_entry,
                          IPO_IDT_ENTRY_FLAGS);
    ipo_idt_set_gate_pair(0xE0,
                          (ipo_vaddr_t)(uintptr_t)smp_ipi_isr,
                          (ipo_vaddr_t)(uintptr_t)lm64_smp_ipi_isr,
                          0x8Eu);

    uint32_t tls_base = (uint32_t)(uintptr_t)kernel_gs_tls_area;
    kernel_gdt[3] = 0x00CF93000000FFFFULL
                  | ((uint64_t)(tls_base & 0x00FFFFFFu) << 16)
                  | ((uint64_t)(tls_base & 0xFF000000u) << 32);

    x86_idt_set_task_gate(ipo_idt_table32, 8, X86_TSS_DOUBLE_FAULT_SELECTOR);
    x86_idt_set_task_gate(ipo_idt_table32, 14, X86_TSS_PAGE_FAULT_SELECTOR);

    ipo_idt64_encode_gate(&ipo_idt_table64[8], (ipo_vaddr_t)(uintptr_t)lm64_double_fault_entry,
                          IPO_KERNEL_CODE64_SEG, 0x8Eu, 2u);
    ipo_idt64_encode_gate(&ipo_idt_table64[14], (ipo_vaddr_t)(uintptr_t)lm64_page_fault_entry,
                          IPO_KERNEL_CODE64_SEG, 0x8Eu, 1u);

    if (trap_is_long_mode()) {
        ipo_idt_ptr64_t gdt_ptr = {
            .limit = (uint16_t)(X86_GDT_ENTRY_COUNT * sizeof(uint64_t) - 1u),
            .base = (uint64_t)(uintptr_t)kernel_gdt
        };
        x86_load_tables_lm64(&gdt_ptr, NULL, 0u);
        arch_cpu_set_tls_base((ipo_vaddr_t)(uintptr_t)kernel_gs_tls_area);
    } else {
        ipo_idt_ptr32_t gdt_ptr = {
            .limit = (uint16_t)(X86_GDT_ENTRY_COUNT * sizeof(uint64_t) - 1u),
            .base = (uint32_t)(uintptr_t)kernel_gdt
        };
        __asm__ volatile(
            "lgdt %0\n\t"
            "movw $0x18, %%ax\n\t"
            "movw %%ax, %%gs"
            : : "m"(gdt_ptr) : "eax", "memory"
        );
    }

    syscall_load_idt();
    if (!syscall_prepare_cpu_exceptions(1u)) {
        serial_printf("[syscall] Warning: Failed to prepare BSP per-CPU exceptions\n");
    }
}

ipo_sarg_t ipo_syscall(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv) {
    ipo_sarg_t ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(argc), "c"(argv)
        : "memory"
    );
    return ret;
}

int ipo_syscall_wide(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv, uint64_t *out_val) {
    if (!out_val) return -1;
    uint32_t low = 0u, high = 0u;
    __asm__ volatile(
        "int $0x80"
        : "=a"(low), "=d"(high)
        : "a"((uint32_t)num), "b"((uint32_t)argc), "c"(argv)
        : "memory"
    );
    *out_val = ((uint64_t)high << 32) | (uint64_t)low;
    return 0;
}
