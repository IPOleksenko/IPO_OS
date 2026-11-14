#include <syscall.h>
#include <stdio.h>
#include <string.h>
#include <file_system/ipo_fs.h>
#include <kernel/process.h>
#include <kernel/smp.h>
#include <kernel/ipc.h>
#include <memory/kmalloc.h>
#include <memory/memory_map.h>
#include <memory/address_space.h>
#include <kernel/terminal.h>
#include <kernel/async.h>
#include <driver/input/keyboard.h>
#include <driver/input/mouse.h>
#include <driver/input/keymap/keymap.h>
#include <driver/input/keymap/dynamic_keymap.h>
#include <kernel/driver.h>
#include <kernel/irq.h>
#include <kernel/input_ext.h>
#include <kernel/device.h>
#include <driver/net/pci.h>
#include <system/state.h>
#include <system/timer.h>
#include <driver/sound.h>
#include <driver/audio_core.h>
#include <vga.h>
#include <vga_gfx.h>
#include <wm.h>
#include <ioport.h>
#include <net/dns.h>
#include <net/icmp.h>
#include <net/net.h>
#include <net/socket.h>

#define IPO_IDT_ENTRY_FLAGS 0xEEu
#define IPO_KERNEL_CODE_SEG 0x08u

typedef struct {
    uint16_t base_lo;
    uint16_t selector;
    uint8_t zero;
    uint8_t flags;
    uint16_t base_hi;
} __attribute__((packed)) ipo_idt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) ipo_idt_ptr_t;

static ipo_idt_entry_t ipo_idt_table[256];
static ipo_syscall_handler_t *ipo_syscall_table = NULL;
static uint32_t ipo_syscall_table_size = 0;

typedef struct {
    char *name;
    void *value;
    uint32_t value_len;
} ipo_shared_var_t;

static ipo_shared_var_t *ipo_shared_vars = NULL;
static uint32_t ipo_shared_var_count = 0u;
static uint32_t ipo_shared_var_capacity = 0u;

extern void syscall_isr_entry(void);
extern uint32_t isr_stub_table[32];
extern void irq0_isr(void);
extern uint32_t irq_stub_table[15];
void syscall_load_idt(void);

static uint32_t kernel_gs_tls_area[16] __attribute__((aligned(16))) = {
    0, 0, 0, 0, 0, 0x595e9fbdU, 0, 0
};

static uint64_t kernel_gdt[4] __attribute__((aligned(8))) = {
    0x0000000000000000ULL,
    0x00CF9B000000FFFFULL, /* 0x08: 32-bit Kernel Code, base=0, limit=4GB (Accessed=1) */
    0x00CF93000000FFFFULL, /* 0x10: 32-bit Kernel Data, base=0, limit=4GB (Accessed=1) */
    0x00CF93000000FFFFULL  /* 0x18: 32-bit TLS/GS Data, base=kernel_gs_tls_area, limit=4GB */
};

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

typedef struct {
    uint64_t gdt[7];
    ipo_idt_entry_t idt[256];
    x86_tss32_t current_tss;
    x86_tss32_t page_fault_tss;
    x86_tss32_t double_fault_tss;
    uint8_t page_fault_stack[16384];
    uint8_t double_fault_stack[8192];
} cpu_exception_tables_t;

#define X86_TSS_CURRENT_SELECTOR 0x20u
#define X86_TSS_PAGE_FAULT_SELECTOR 0x28u
#define X86_TSS_DOUBLE_FAULT_SELECTOR 0x30u

static cpu_exception_tables_t *cpu_exception_tables;
static uint32_t cpu_exception_table_count;

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
        default: return (vector < 32) ? "Reserved" : "Unknown Exception";
    }
}

extern void page_fault_task_entry(void);
extern void double_fault_task_entry(void);

static uint32_t x86_read_cr3(void) {
    uint32_t value;
    __asm__ volatile("movl %%cr3, %0" : "=r"(value));
    return value;
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

static void x86_tss_initialize(x86_tss32_t *tss, uint32_t cr3,
                               uint32_t eip, uint32_t esp) {
    memset(tss, 0, sizeof(*tss));
    tss->cr3 = cr3;
    tss->eip = eip;
    tss->eflags = 0x2u;
    tss->esp = esp;
    tss->esp0 = esp;
    tss->cs = 0x08u;
    tss->ss = 0x10u;
    tss->ss0 = 0x10u;
    tss->ds = 0x10u;
    tss->es = 0x10u;
    tss->fs = 0x10u;
    tss->gs = 0x18u;
    tss->iomap_base = sizeof(*tss);
}

static void x86_idt_set_task_gate(ipo_idt_entry_t *idt, uint8_t vector, uint16_t selector) {
    idt[vector].base_lo = 0u;
    idt[vector].selector = selector;
    idt[vector].zero = 0u;
    idt[vector].flags = 0x85u;
    idt[vector].base_hi = 0u;
}

void fpu_init(void) {
    uint32_t cr0;
    __asm__ volatile("movl %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1u << 2); /* Clear EM (bit 2) */
    cr0 &= ~(1u << 3); /* Clear TS (bit 3) */
    cr0 |= (1u << 1);  /* Set MP (bit 1) */
    cr0 |= (1u << 5);  /* Set NE (bit 5) */
    __asm__ volatile("movl %0, %%cr0" : : "r"(cr0));

    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    if (edx & (1u << 24)) { /* FXSR supported */
        uint32_t cr4;
        __asm__ volatile("movl %%cr4, %0" : "=r"(cr4));
        cr4 |= (1u << 9); /* CR4.OSFXSR */
        if (edx & (1u << 25)) { /* SSE supported */
            cr4 |= (1u << 10); /* CR4.OSXMMEXCPT */
        }
        __asm__ volatile("movl %0, %%cr4" : : "r"(cr4));
    }

    __asm__ volatile("fninit");
}

bool syscall_init_cpu(uint32_t cpu_id) {
    if (!cpu_exception_tables || cpu_id >= cpu_exception_table_count) {
        return false;
    }

    fpu_init();

    cpu_exception_tables_t *tables = &cpu_exception_tables[cpu_id];
    ipo_idt_ptr_t gdt_ptr = {
        .limit = (uint16_t)(sizeof(tables->gdt) - 1u),
        .base = (uint32_t)(uintptr_t)tables->gdt
    };
    ipo_idt_ptr_t idt_ptr = {
        .limit = (uint16_t)(sizeof(tables->idt) - 1u),
        .base = (uint32_t)(uintptr_t)tables->idt
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
    return true;
}

bool syscall_prepare_cpu_exceptions(uint32_t cpu_count) {
    if (cpu_count == 0u) return false;
    if (cpu_exception_tables && cpu_count <= cpu_exception_table_count) {
        return true;
    }

    cpu_exception_tables_t *old_tables = cpu_exception_tables;
    cpu_exception_tables_t *tables =
        (cpu_exception_tables_t *)kmalloc(cpu_count * sizeof(*tables));
    if (!tables) return false;
    memset(tables, 0, cpu_count * sizeof(*tables));

    uint32_t cr3 = x86_read_cr3();
    for (uint32_t i = 0u; i < cpu_count; i++) {
        cpu_exception_tables_t *cpu = &tables[i];
        memcpy(cpu->gdt, kernel_gdt, sizeof(kernel_gdt));
        cpu->gdt[4] = x86_tss_descriptor(&cpu->current_tss);
        cpu->gdt[5] = x86_tss_descriptor(&cpu->page_fault_tss);
        cpu->gdt[6] = x86_tss_descriptor(&cpu->double_fault_tss);
        x86_tss_initialize(&cpu->current_tss, cr3, 0u, 0u);
        x86_tss_initialize(&cpu->page_fault_tss, cr3,
                           (uint32_t)(uintptr_t)page_fault_task_entry,
                           (uint32_t)(uintptr_t)&cpu->page_fault_stack[
                               sizeof(cpu->page_fault_stack)]);
        x86_tss_initialize(&cpu->double_fault_tss, cr3,
                           (uint32_t)(uintptr_t)double_fault_task_entry,
                           (uint32_t)(uintptr_t)&cpu->double_fault_stack[
                               sizeof(cpu->double_fault_stack)]);

        /* Copy master IDT to per-CPU IDT table */
        memcpy(cpu->idt, ipo_idt_table, sizeof(ipo_idt_table));

        /* Vector 8 (#DF) -> Double fault per-CPU Task Gate (0x85) */
        x86_idt_set_task_gate(cpu->idt, 8, X86_TSS_DOUBLE_FAULT_SELECTOR);

        /* Vector 14 (#PF) -> Page fault per-CPU Task Gate (0x85) */
        x86_idt_set_task_gate(cpu->idt, 14, X86_TSS_PAGE_FAULT_SELECTOR);
    }

    cpu_exception_tables = tables;
    cpu_exception_table_count = cpu_count;
    if (!syscall_init_cpu(0u)) {
        cpu_exception_tables = old_tables;
        kfree(tables);
        return false;
    }

    if (old_tables) {
        kfree(old_tables);
    }

    return true;
}

void syscall_update_exception_page_directory(void) {
    if (!cpu_exception_tables) return;
    uint32_t cr3 = x86_read_cr3();
    for (uint32_t i = 0u; i < cpu_exception_table_count; i++) {
        cpu_exception_tables[i].current_tss.cr3 = cr3;
        cpu_exception_tables[i].page_fault_tss.cr3 = cr3;
        cpu_exception_tables[i].double_fault_tss.cr3 = cr3;
    }
}

static void __attribute__((noreturn)) page_fault_terminate_process(void) {
    process_emergency_crash_exit();
    for (;;) __asm__ volatile("cli; hlt");
}

void cpu_page_fault_task_handler(uint32_t error_code) {
    uint32_t cr2;
    __asm__ volatile("movl %%cr2, %0" : "=r"(cr2));

    uint32_t cpu_id = smp_get_current_cpu_id();
    process_t *proc = process_get_current();
    cpu_exception_tables_t *tables =
        (cpu_id < cpu_exception_table_count)
            ? &cpu_exception_tables[cpu_id]
            : NULL;

    if (tables &&
        address_space_handle_trampoline_fault(cr2, error_code, tables->current_tss.esp)) {
        tables->current_tss.cr3 = tables->page_fault_tss.cr3;
        return;
    }

    if (proc && proc->current_thread && tables &&
        process_thread_handle_stack_fault(
            proc, proc->current_thread, cr2, error_code,
            tables->current_tss.esp)) {
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
        for (;;) __asm__ volatile("cli; hlt");
    }

    process_thread_main_exit(proc);
    proc->is_running = 0u;
    tables->current_tss.eip =
        (uint32_t)(uintptr_t)page_fault_terminate_process;
    tables->current_tss.esp =
        (uint32_t)(uintptr_t)&tables->page_fault_stack[
            sizeof(tables->page_fault_stack) - 64u];
    tables->current_tss.ebp = 0u;
    tables->current_tss.eflags = 0x2u;
    tables->current_tss.cs = 0x08u;
    tables->current_tss.ss = 0x10u;
    tables->current_tss.ds = 0x10u;
    tables->current_tss.es = 0x10u;
    tables->current_tss.fs = 0x10u;
    tables->current_tss.gs = 0x18u;

    /* Re-arm page fault TSS state */
    tables->page_fault_tss.esp = (uint32_t)(uintptr_t)&tables->page_fault_stack[
        sizeof(tables->page_fault_stack)];
    tables->page_fault_tss.eip = (uint32_t)(uintptr_t)page_fault_task_entry;
    tables->page_fault_tss.cr3 = x86_read_cr3();
}

void cpu_double_fault_task_handler(void) {
    uint32_t cpu_id = smp_get_current_cpu_id();
    serial_printf("KERNEL PANIC: double fault on CPU %u\n", cpu_id);
    for (;;) __asm__ volatile("cli; hlt");
}

void cpu_exception_handler(uint32_t *frame) {
    uint32_t vector = frame[7];
    if (vector == 7) {
        /* Device Not Available (#NM):
         * Hardware task switch (e.g. from page fault handler) automatically sets CR0.TS = 1.
         * Clear TS flag so the thread can continue using FPU/SSE, and return. */
        __asm__ volatile("clts");
        return;
    }
    uint32_t err_code = frame[8];
    uint32_t eip = frame[9];
    uint32_t cr2 = 0;
    if (vector == 14) {
        __asm__ volatile("movl %%cr2, %0" : "=r"(cr2));
    }
    const char *name = x86_exception_name(vector);

    putchar_unlock();

    process_t *proc = process_get_current();
    if (proc != NULL) {
        if (vga_is_graphics_mode() && !wm_session_active()) {
            vga_set_mode_text_hardware();
        }
        printf("\n[Process %u] Terminated: CPU Exception %u (%s) at 0x%08x (err=0x%x)\n",
               proc->pid, vector, name, eip, err_code);
        serial_printf("[cpu_exception] Process %u crashed: vec=%u (%s) eip=0x%08x err=0x%x cr2=0x%08x\n",
                      proc->pid, vector, name, eip, err_code, cr2);
        process_emergency_crash_exit();
        return;
    }

    printf("\nKERNEL PANIC: CPU Exception %u (%s) at 0x%08x (err=0x%x)\n",
           vector, name, eip, err_code);
    serial_printf("KERNEL PANIC: CPU Exception %u (%s) at 0x%08x (err=0x%x cr2=0x%08x)\n",
                  vector, name, eip, err_code, cr2);
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

static ipo_shared_var_t *find_shared_var_locked(const char *name) {
    if (name == NULL) {
        return NULL;
    }

    for (uint32_t i = 0u; i < ipo_shared_var_count; i++) {
        if (ipo_shared_vars[i].name != NULL &&
            strcmp(ipo_shared_vars[i].name, name) == 0) {
            return &ipo_shared_vars[i];
        }
    }

    return NULL;
}

static int set_shared_var(const char *name, const void *value, uint32_t value_len) {
    if (name == NULL) {
        return -1;
    }

    ipo_shared_var_t *existing = find_shared_var_locked(name);
    if (existing != NULL) {
        void *new_value = kmalloc(value_len == 0u ? 1u : value_len);
        if (new_value == NULL) {
            return -1;
        }

        if (value != NULL && value_len > 0u) {
            memcpy(new_value, value, value_len);
        } else {
            memset(new_value, 0, value_len == 0u ? 1u : value_len);
        }

        kfree(existing->value);
        existing->value = new_value;
        existing->value_len = value_len;
        return 0;
    }

    if (ipo_shared_var_count >= ipo_shared_var_capacity) {
        uint32_t new_capacity = (ipo_shared_var_capacity == 0u) ? 8u : ipo_shared_var_capacity * 2u;
        ipo_shared_var_t *new_table = kmalloc(new_capacity * sizeof(ipo_shared_var_t));
        if (new_table == NULL) {
            return -1;
        }

        memset(new_table, 0, new_capacity * sizeof(ipo_shared_var_t));
        if (ipo_shared_vars != NULL) {
            memcpy(new_table, ipo_shared_vars, ipo_shared_var_count * sizeof(ipo_shared_var_t));
            kfree(ipo_shared_vars);
        }

        ipo_shared_vars = new_table;
        ipo_shared_var_capacity = new_capacity;
    }

    size_t name_len = strlen(name) + 1u;
    char *name_copy = kmalloc(name_len);
    if (name_copy == NULL) {
        return -1;
    }
    memcpy(name_copy, name, name_len);

    void *value_copy = kmalloc(value_len == 0u ? 1u : value_len);
    if (value_copy == NULL) {
        kfree(name_copy);
        return -1;
    }

    if (value != NULL && value_len > 0u) {
        memcpy(value_copy, value, value_len);
    } else {
        memset(value_copy, 0, value_len == 0u ? 1u : value_len);
    }

    ipo_shared_vars[ipo_shared_var_count].name = name_copy;
    ipo_shared_vars[ipo_shared_var_count].value = value_copy;
    ipo_shared_vars[ipo_shared_var_count].value_len = value_len;
    ipo_shared_var_count++;

    return 0;
}

static int get_shared_var(const char *name, void *out_buffer, uint32_t out_capacity, uint32_t *out_len) {
    if (name == NULL || out_buffer == NULL || out_capacity == 0u) {
        return -1;
    }

    ipo_shared_var_t *entry = find_shared_var_locked(name);
    if (entry == NULL) {
        return -1;
    }

    uint32_t copy_len = entry->value_len;
    if (copy_len > out_capacity) {
        copy_len = out_capacity;
    }

    if (entry->value != NULL && copy_len > 0u) {
        memcpy(out_buffer, entry->value, copy_len);
    }

    if (out_len != NULL) {
        *out_len = entry->value_len;
    }

    return (int)copy_len;
}

static int delete_shared_var(const char *name) {
    if (name == NULL) {
        return -1;
    }

    for (uint32_t i = 0u; i < ipo_shared_var_count; i++) {
        if (ipo_shared_vars[i].name != NULL &&
            strcmp(ipo_shared_vars[i].name, name) == 0) {
            kfree(ipo_shared_vars[i].name);
            kfree(ipo_shared_vars[i].value);

            for (uint32_t j = i + 1u; j < ipo_shared_var_count; j++) {
                ipo_shared_vars[j - 1u] = ipo_shared_vars[j];
            }

            ipo_shared_var_count--;
            memset(&ipo_shared_vars[ipo_shared_var_count], 0, sizeof(ipo_shared_var_t));
            return 0;
        }
    }

    return -1;
}

static void ipo_idt_set_gate(uint8_t num, uint32_t base, uint16_t selector, uint8_t flags) {
    ipo_idt_table[num].base_lo = (uint16_t)(base & 0xFFFFu);
    ipo_idt_table[num].selector = selector;
    ipo_idt_table[num].zero = 0;
    ipo_idt_table[num].flags = flags;
    ipo_idt_table[num].base_hi = (uint16_t)((base >> 16) & 0xFFFFu);

    if (cpu_exception_tables) {
        for (uint32_t i = 0u; i < cpu_exception_table_count; i++) {
            if (num != 8u && num != 14u) {
                cpu_exception_tables[i].idt[num] = ipo_idt_table[num];
            }
        }
    }
}

void ipo_register_syscall(uint32_t num, ipo_syscall_handler_t handler) {
    if (num >= 65536u) {
        return;
    }

    if (ipo_syscall_table == NULL || num >= ipo_syscall_table_size) {
        uint32_t new_size = ipo_syscall_table_size ? ipo_syscall_table_size : 64u;
        while (new_size <= num) {
            new_size *= 2u;
        }

        ipo_syscall_handler_t *new_table =
            (ipo_syscall_handler_t *)kmalloc(
                new_size * sizeof(ipo_syscall_handler_t));

        if (new_table == NULL) {
            return;
        }

        if (ipo_syscall_table != NULL) {
            memcpy(new_table,
                   ipo_syscall_table,
                   ipo_syscall_table_size * sizeof(ipo_syscall_handler_t));
            kfree(ipo_syscall_table);
        }

        memset(new_table + ipo_syscall_table_size,
               0,
               (new_size - ipo_syscall_table_size) *
                   sizeof(ipo_syscall_handler_t));

        ipo_syscall_table = new_table;
        ipo_syscall_table_size = new_size;
    }

    ipo_syscall_table[num] = handler;
}

static uint32_t syscall_builtin_register(uint32_t num,
                                         uint32_t argc,
                                         uint32_t *argv) {
    (void)num;

    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t syscall_num = argv[0];
    uint32_t handler_ptr = argv[1];

    if (syscall_num < 0x2000u || syscall_num >= 65536u || handler_ptr == 0u) {
        return IPO_SYSCALL_ENOSYS;
    }

    if (process_in_process_context() &&
        !ipc_user_range((const void *)(uintptr_t)handler_ptr, 1u)) {
        return IPO_SYSCALL_ENOSYS;
    }

    ipo_register_syscall(
        syscall_num,
        (ipo_syscall_handler_t)(uintptr_t)handler_ptr);

    return IPO_SYSCALL_OK;
}

static uint32_t syscall_builtin_call(uint32_t num,
                                     uint32_t argc,
                                     uint32_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t syscall_num = argv[0];

    if (syscall_num >= ipo_syscall_table_size) {
        return IPO_SYSCALL_ENOSYS;
    }

    ipo_syscall_handler_t fn = ipo_syscall_table[syscall_num];

    if (fn == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return fn(syscall_num,
              argc > 0u ? argc - 1u : 0u,
              argc > 1u ? &argv[1] : NULL);
}

static uint32_t syscall_builtin_print(uint32_t num,
                                      uint32_t argc,
                                      uint32_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *str = (const char *)(uintptr_t)argv[0];
    if (str == NULL ||
        (process_in_process_context() && !ipc_user_range(str, 1u))) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)printf("%s", str);
}

static uint32_t syscall_builtin_write(uint32_t num,
                                      uint32_t argc,
                                      uint32_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *text = (const char *)(uintptr_t)argv[0];

    if (text == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t count = 0;
    if (argc >= 2u) {
        uint32_t len = argv[1];
        while (count < len) {
            putchar(text[count]);
            count++;
        }
    } else {
        while (text[count] != '\0') {
            putchar(text[count]);
            count++;
        }
    }

    return count;
}

static uint32_t syscall_builtin_fs_open(uint32_t num,
                                        uint32_t argc,
                                        uint32_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *path = (const char *)(uintptr_t)argv[0];
    serial_printf("[fs_open] path='%s'\n", path ? path : "NULL");

    return (uint32_t)ipo_fs_open(path);
}

static uint32_t syscall_builtin_fs_close(uint32_t num,
                                          uint32_t argc,
                                          uint32_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)ipo_fs_close((int)argv[0]);
}

static uint32_t syscall_builtin_fs_read(uint32_t num,
                                        uint32_t argc,
                                        uint32_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    int fd = (int)argv[0];
    void *buf = (void *)(uintptr_t)argv[1];
    uint32_t count = argv[2];
    uint32_t offset = 0;

    if (argc >= 4u) {
        offset = argv[3];
    } else {
        if (fd < 0 || (uint32_t)fd >= fds_capacity || !fds || !fds[fd].used) return (uint32_t)-1;
        offset = fds[fd].offset;
    }

    int res = ipo_fs_read(fd, buf, count, offset);
    if (res > 0 && argc < 4u) {
        fds[fd].offset += (uint32_t)res;
    }
    return (uint32_t)res;
}

static uint32_t syscall_builtin_fs_write(uint32_t num,
                                         uint32_t argc,
                                         uint32_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    int fd = (int)argv[0];
    const void *buf = (const void *)(uintptr_t)argv[1];
    uint32_t count = argv[2];
    uint32_t offset = 0;

    if (argc >= 4u) {
        offset = argv[3];
    } else {
        if (fd < 0 || (uint32_t)fd >= fds_capacity || !fds || !fds[fd].used) return (uint32_t)-1;
        offset = fds[fd].offset;
    }

    int res = ipo_fs_write(fd, buf, count, offset);
    if (res > 0 && argc < 4u) {
        fds[fd].offset += (uint32_t)res;
    }
    return (uint32_t)res;
}

static uint32_t syscall_builtin_fs_seek(uint32_t num,
                                        uint32_t argc,
                                        uint32_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return (uint32_t)-1;
    }

    int fd = (int)argv[0];
    int32_t offset = (int32_t)argv[1];
    int whence = (int)argv[2];

    if (fd < 0 || (uint32_t)fd >= fds_capacity || !fds || !fds[fd].used) {
        return (uint32_t)-1;
    }

    struct ipo_inode inode;
    if (!read_inode(fds[fd].inode, &inode)) {
        return (uint32_t)-1;
    }

    int32_t new_pos = 0;
    if (whence == 0) { // SEEK_SET
        new_pos = offset;
    } else if (whence == 1) { // SEEK_CUR
        new_pos = (int32_t)fds[fd].offset + offset;
    } else if (whence == 2) { // SEEK_END
        new_pos = (int32_t)inode.size + offset;
    } else {
        return (uint32_t)-1;
    }

    if (new_pos < 0) {
        return (uint32_t)-1;
    }

    fds[fd].offset = (uint32_t)new_pos;
    return (uint32_t)new_pos;
}

static uint32_t syscall_builtin_fs_delete(uint32_t num,
                                          uint32_t argc,
                                          uint32_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)(
        ipo_fs_delete((const char *)(uintptr_t)argv[0]) ? 0u : 1u);
}

static uint32_t syscall_builtin_fs_stat(uint32_t num,
                                        uint32_t argc,
                                        uint32_t *argv) {
    (void)num;

    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    struct ipo_inode *st =
        (struct ipo_inode *)(uintptr_t)argv[1];

    if (st == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *path = (const char *)(uintptr_t)argv[0];
    bool res = ipo_fs_stat(path, st);
    serial_printf("[fs_stat] path='%s' res=%d\n", path ? path : "NULL", res);

    return (uint32_t)(res ? 0u : 1u);
}

static uint32_t syscall_builtin_fs_statvfs(uint32_t num,
                                           uint32_t argc,
                                           uint32_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return IPO_SYSCALL_ENOSYS;
    uint32_t *out = (uint32_t *)(uintptr_t)argv[0];
    if (out == NULL ||
        (process_in_process_context() && !ipc_user_range(out, 7u * sizeof(uint32_t))) ||
        !fs_mounted) {
        return 1u;
    }

    uint32_t data_start = (uint32_t)sb.data_blocks_start;
    uint32_t total = (sb.fs_size_blocks > data_start)
                     ? (uint32_t)(sb.fs_size_blocks - data_start) : 0;
    uint32_t used = total ? bitmap_count_set(sb.block_bitmap_start, total) : 0;
    uint64_t ino_total = 0, ino_used = 0, ino_free = 0, dyn_chunks = 0;
    ipo_fs_get_inode_stats(&ino_total, &ino_used, &ino_free, &dyn_chunks);

    out[0] = sb.block_size;                 /* f_bsize */
    out[1] = sb.block_size;                 /* f_frsize */
    out[2] = total;                         /* f_blocks */
    out[3] = total > used ? total - used : 0; /* f_bfree */
    out[4] = out[3];                        /* f_bavail */
    out[5] = (uint32_t)ino_total;           /* f_files */
    out[6] = (uint32_t)ino_free;            /* f_ffree */
    return 0u;
}

static uint32_t syscall_builtin_fs_list(uint32_t num,
                                        uint32_t argc,
                                        uint32_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)ipo_fs_list_dir(
        (const char *)(uintptr_t)argv[0],
        (char *)(uintptr_t)argv[1],
        (int)argv[2]);
}

static uint32_t syscall_builtin_fs_rename(uint32_t num,
                                          uint32_t argc,
                                          uint32_t *argv) {
    (void)num;

    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)(
        ipo_fs_rename(
            (const char *)(uintptr_t)argv[0],
            (const char *)(uintptr_t)argv[1]) ? 0u : 1u);
}

static uint32_t syscall_builtin_exec(uint32_t num,
                                     uint32_t argc,
                                     uint32_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)process_exec(
        (const char *)(uintptr_t)argv[0],
        (int)argv[1],
        (char **)(uintptr_t)argv[2]);
}

static uint32_t syscall_builtin_getcwd(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 2u || argv == NULL) return (uint32_t)0;
    char *buf = (char *)(uintptr_t)argv[0];
    uint32_t size = argv[1];
    if (!buf || size == 0) return (uint32_t)0;
    const char *cwd = terminal_get_cwd();
    if (!cwd) cwd = "/";
    strncpy(buf, cwd, size - 1);
    buf[size - 1] = '\0';
    return (uint32_t)(uintptr_t)buf;
}

static uint32_t syscall_builtin_chdir(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return (uint32_t)-1;
    const char *path = (const char *)(uintptr_t)argv[0];
    return (uint32_t)terminal_set_cwd(path);
}

static uint32_t syscall_builtin_get_exit_code(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)process_get_exit_code();
}

static uint32_t syscall_builtin_set_return_value(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 2u || argv == NULL) return (uint32_t)-1;
    process_return_type_t type = (process_return_type_t)argv[0];
    const char *type_name = "";
    const char *str_val = NULL;
    int32_t int_val = 0;
    const void *data = NULL;
    uint32_t size = 0;

    if (type == PROC_RET_INT) {
        int_val = (int32_t)argv[1];
        type_name = "int";
    } else if (type == PROC_RET_CHAR) {
        int_val = (int32_t)(char)argv[1];
        type_name = "char";
    } else if (type == PROC_RET_STRING) {
        str_val = (const char *)(uintptr_t)argv[1];
        if (argc > 2u && argv[2] != 0) {
            type_name = (const char *)(uintptr_t)argv[2];
        } else {
            type_name = "char*";
        }
    } else if (type == PROC_RET_STRUCT) {
        data = (const void *)(uintptr_t)argv[1];
        size = argc > 2u ? argv[2] : 0;
        if (argc > 3u && argv[3] != 0) {
            type_name = (const char *)(uintptr_t)argv[3];
        } else {
            type_name = "struct";
        }
    } else if (type == PROC_RET_POINTER) {
        data = (const void *)(uintptr_t)argv[1];
        type_name = "void*";
    } else if ((uint32_t)type == 99u) {
        if (argc > 1u && argv[1] != 0) {
            process_set_return_format(process_get_current(), (const char *)(uintptr_t)argv[1]);
        }
        return 0;
    }

    process_set_current_return_value(type, type_name, str_val, int_val, data, size);
    return 0;
}

static uint32_t syscall_builtin_get_return_value(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 2u || argv == NULL) return (uint32_t)-1;
    char *buf = (char *)(uintptr_t)argv[0];
    uint32_t max_len = argv[1];
    if (!buf || max_len == 0) return (uint32_t)-1;

    const char *val_str = process_get_last_return_string();
    if (!val_str) val_str = "";
    strncpy(buf, val_str, max_len - 1);
    buf[max_len - 1] = '\0';
    return (uint32_t)strlen(buf);
}

static uint32_t syscall_builtin_terminal_input(uint32_t num,
                                               uint32_t argc,
                                               uint32_t *argv) {
    (void)num;

    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *text = (const char *)(uintptr_t)argv[0];
    int auto_exec = (int)argv[1];
    terminal_inject_input(text, auto_exec != 0);
    return IPO_SYSCALL_OK;
}

static uint32_t syscall_builtin_keymap_set(uint32_t num,
                                           uint32_t argc,
                                           uint32_t *argv) {
    (void)num;
    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    const char *name = (const char *)(uintptr_t)argv[0];
    const keymap_entry_t *entries = (const keymap_entry_t *)(uintptr_t)argv[1];
    uint32_t count = argv[2];
    const dynamic_glyph_def_t *glyphs = (argc >= 5) ? (const dynamic_glyph_def_t *)(uintptr_t)argv[3] : NULL;
    uint32_t glyph_count = (argc >= 5) ? argv[4] : 0u;

    int res = dynamic_keymap_register_with_font(name, entries, count, glyphs, glyph_count);
    terminal_render_language_bar();
    return (uint32_t)res;
}

static uint32_t syscall_builtin_keymap_get(uint32_t num,
                                           uint32_t argc,
                                           uint32_t *argv) {
    (void)num;
    if (argc == 0 || argv == NULL) {
        return (uint32_t)(uintptr_t)dynamic_keymap_get_name_local();
    }

    uint32_t cmd = argv[0];
    switch (cmd) {
        case 0: { /* Get active layout name */
            const char *name = dynamic_keymap_get_name_local();
            if (argc >= 3u && argv[1] != 0 && argv[2] > 0) {
                char *buf = (char *)(uintptr_t)argv[1];
                uint32_t max_len = argv[2];
                strncpy(buf, name ? name : "Unknown", max_len - 1);
                buf[max_len - 1] = '\0';
                return 0;
            }
            return (uint32_t)(uintptr_t)name;
        }
        case 1: { /* Get character translation table for scancodes 0..max_len-1 */
            if (argc >= 3u && argv[1] != 0 && argv[2] > 0) {
                char *out_table = (char *)(uintptr_t)argv[1];
                uint32_t max_len = argv[2];
                bool shift = (argc >= 4u) ? (bool)argv[3] : false;
                memset(out_table, 0, max_len);
                for (uint32_t sc = 0; sc < max_len; sc++) {
                    const char *s = dynamic_keymap_translate_local((uint8_t)sc, shift);
                    if (s != NULL && s[0] != '\0') {
                        out_table[sc] = s[0];
                    }
                }
                return 0;
            }
            return IPO_SYSCALL_ENOSYS;
        }
        case 2: { /* Copy entries: argv[1]=slot, argv[2]=out_entries, argv[3]=max_count */
            if (argc >= 4u && argv[2] != 0 && argv[3] > 0) {
                uint32_t slot = argv[1];
                keymap_entry_t *out = (keymap_entry_t *)(uintptr_t)argv[2];
                uint32_t max_count = argv[3];
                return dynamic_keymap_copy_entries(slot, out, max_count);
            }
            return 0;
        }
        case 3: { /* Get total layout count */
            return dynamic_keymap_get_count();
        }
        case 4: { /* Get entries count for slot: argv[1]=slot */
            if (argc >= 2u) {
                return dynamic_keymap_get_entries_count(argv[1]);
            }
            return 0;
        }
        case 5: { /* Get slot name: argv[1]=slot, argv[2]=buf, argv[3]=max_len */
            if (argc >= 4u && argv[2] != 0 && argv[3] > 0) {
                uint32_t slot = argv[1];
                char *buf = (char *)(uintptr_t)argv[2];
                uint32_t max_len = argv[3];
                const char *sname = dynamic_keymap_get_slot_name(slot);
                strncpy(buf, sname ? sname : "Unknown", max_len - 1);
                buf[max_len - 1] = '\0';
                return 0;
            }
            return IPO_SYSCALL_ENOSYS;
        }
        case 6: { /* Is slot enabled: argv[1]=slot */
            if (argc >= 2u) {
                return dynamic_keymap_is_slot_enabled(argv[1]) ? 1u : 0u;
            }
            return 0;
        }
        default:
            break;
    }

    return (uint32_t)(uintptr_t)dynamic_keymap_get_name_local();
}

static uint32_t syscall_builtin_font_load(uint32_t num,
                                          uint32_t argc,
                                          uint32_t *argv) {
    (void)num;
    const char *path = (argc >= 1u && argv != NULL) ? (const char *)(uintptr_t)argv[0] : NULL;
    int res = vga_font_load_database(path);
    return (res == 0) ? IPO_SYSCALL_OK : IPO_SYSCALL_ENOSYS;
}

static uint32_t syscall_builtin_font_get_info(uint32_t num,
                                              uint32_t argc,
                                              uint32_t *argv) {
    (void)num;
    char *name_buf = (argc >= 1u && argv != NULL) ? (char *)(uintptr_t)argv[0] : NULL;
    uint32_t max_len = (argc >= 2u && argv != NULL) ? argv[1] : 0;
    uint32_t *out_count = (argc >= 3u && argv != NULL) ? (uint32_t *)(uintptr_t)argv[2] : NULL;
    return (uint32_t)vga_font_get_info(name_buf, max_len, out_count);
}

static uint32_t syscall_builtin_vga_glyph(uint32_t num,
                                         uint32_t argc,
                                         uint32_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return (uint32_t)-1;
    if (argc >= 2u && argv[0] != 0) {
        const char *str = (const char *)(uintptr_t)argv[0];
        size_t str_len = (size_t)argv[1];
        size_t consumed = 0;
        uint8_t glyph = utf8_to_vga_glyph(str, str_len, &consumed);
        if (argc >= 3u && argv[2] != 0) {
            *(uint32_t *)(uintptr_t)argv[2] = (uint32_t)consumed;
        }
        return (uint32_t)glyph;
    }
    uint32_t codepoint = argv[0];
    int slot = vga_font_register_glyph(codepoint, NULL);
    return (uint32_t)slot;
}

static uint32_t syscall_builtin_keymap_disable(uint32_t num,
                                              uint32_t argc,
                                              uint32_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return IPO_SYSCALL_ENOSYS;
    const char *target = (const char *)(uintptr_t)argv[0];
    return (uint32_t)dynamic_keymap_disable(target);
}

static uint32_t syscall_builtin_keymap_enable(uint32_t num,
                                             uint32_t argc,
                                             uint32_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return IPO_SYSCALL_ENOSYS;
    const char *target = (const char *)(uintptr_t)argv[0];
    return (uint32_t)dynamic_keymap_enable(target);
}

static uint32_t syscall_builtin_keymap_remove(uint32_t num,
                                             uint32_t argc,
                                             uint32_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return IPO_SYSCALL_ENOSYS;
    const char *target = (const char *)(uintptr_t)argv[0];
    return (uint32_t)dynamic_keymap_remove(target);
}

static uint32_t syscall_builtin_keymap_translate(uint32_t num,
                                                uint32_t argc,
                                                uint32_t *argv) {
    (void)num;
    if (argc < 2u || argv == NULL) return 0;
    uint8_t scancode = (uint8_t)argv[0];
    bool shift = (bool)argv[1];
    const char *s = dynamic_keymap_translate_local(scancode, shift);
    return (uint32_t)(uintptr_t)s;
}

static uint32_t syscall_builtin_keymap_is_active(uint32_t num,
                                                uint32_t argc,
                                                uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)dynamic_keymap_is_active_local();
}

static uint32_t syscall_builtin_keymap_cycle_next(uint32_t num,
                                                 uint32_t argc,
                                                 uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    dynamic_keymap_cycle_next_local();
    return 0;
}

static uint32_t syscall_builtin_keymap_cycle_prev(uint32_t num,
                                                 uint32_t argc,
                                                 uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    dynamic_keymap_cycle_prev_local();
    return 0;
}

static uint32_t syscall_builtin_keymap_get_name(uint32_t num,
                                                uint32_t argc,
                                                uint32_t *argv) {
    (void)num;
    const char *name = dynamic_keymap_get_name_local();
    if (argc >= 2u && argv != NULL && argv[0] != 0 && argv[1] > 0) {
        char *user_buf = (char *)(uintptr_t)argv[0];
        uint32_t max_len = argv[1];
        strncpy(user_buf, name ? name : default_keymap_name, max_len - 1);
        user_buf[max_len - 1] = '\0';
        return 0;
    }
    return (uint32_t)(uintptr_t)name;
}

static uint32_t syscall_builtin_driver_register(uint32_t num,
                                                uint32_t argc,
                                                uint32_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    driver_t *drv = (driver_t *)(uintptr_t)argv[0];
    int res = driver_register(drv);
    return (uint32_t)res;
}

static uint32_t syscall_builtin_driver_unregister(uint32_t num,
                                                  uint32_t argc,
                                                  uint32_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    const char *name = (const char *)(uintptr_t)argv[0];
    int res = driver_unregister(name);
    return (uint32_t)res;
}

static uint32_t syscall_builtin_driver_list(uint32_t num,
                                            uint32_t argc,
                                            uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    driver_print_list();
    return IPO_SYSCALL_OK;
}

static uint32_t syscall_builtin_driver_set_desc(uint32_t num,
                                                uint32_t argc,
                                                uint32_t *argv) {
    (void)num;
    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    const char *name = (const char *)(uintptr_t)argv[0];
    const char *desc = (const char *)(uintptr_t)argv[1];
    int res = driver_set_description(name, desc);
    return (uint32_t)res;
}

static uint32_t syscall_builtin_port_io(uint32_t num,
                                        uint32_t argc,
                                        uint32_t *argv) {
    if (argv == NULL) return IPO_SYSCALL_ENOSYS;

    switch (num) {
        case IPO_SYSCALL_PORT_IN8:
            if (argc < 1u) return IPO_SYSCALL_ENOSYS;
            return inb((uint16_t)argv[0]);
        case IPO_SYSCALL_PORT_OUT8:
            if (argc < 2u) return IPO_SYSCALL_ENOSYS;
            outb((uint16_t)argv[0], (uint8_t)argv[1]);
            return IPO_SYSCALL_OK;
        case IPO_SYSCALL_PORT_IN16:
            if (argc < 1u) return IPO_SYSCALL_ENOSYS;
            return inw((uint16_t)argv[0]);
        case IPO_SYSCALL_PORT_OUT16:
            if (argc < 2u) return IPO_SYSCALL_ENOSYS;
            outw((uint16_t)argv[0], (uint16_t)argv[1]);
            return IPO_SYSCALL_OK;
        case IPO_SYSCALL_PORT_IN32:
            if (argc < 1u) return IPO_SYSCALL_ENOSYS;
            return inl((uint16_t)argv[0]);
        case IPO_SYSCALL_PORT_OUT32:
            if (argc < 2u) return IPO_SYSCALL_ENOSYS;
            outl((uint16_t)argv[0], argv[1]);
            return IPO_SYSCALL_OK;
        default:
            return IPO_SYSCALL_ENOSYS;
    }
}

static uint32_t syscall_builtin_binfmt_register(uint32_t num,
                                                uint32_t argc,
                                                uint32_t *argv) {
    (void)num;
    if (argc < 5u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    const char *name = (const char *)(uintptr_t)argv[0];
    const uint8_t *magic = (const uint8_t *)(uintptr_t)argv[1];
    uint32_t magic_len = argv[2];
    uint32_t offset = argv[3];
    const char *runner_path = (const char *)(uintptr_t)argv[4];

    int res = binfmt_register(name, magic, magic_len, offset, runner_path);
    return (uint32_t)res;
}

static uint32_t syscall_builtin_binfmt_unregister(uint32_t num,
                                                  uint32_t argc,
                                                  uint32_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    const char *name = (const char *)(uintptr_t)argv[0];
    int res = binfmt_unregister(name);
    return (uint32_t)res;
}

static uint32_t syscall_builtin_binfmt_list(uint32_t num,
                                            uint32_t argc,
                                            uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    binfmt_list();
    return IPO_SYSCALL_OK;
}

static uint32_t utf8_prev_char(const char *buf, uint32_t pos) {
    if (pos == 0u) return 0u;
    pos--;
    while (pos > 0u && ((unsigned char)buf[pos] & 0xC0u) == 0x80u) {
        pos--;
    }
    return pos;
}

static uint32_t utf8_next_char(const char *buf, uint32_t len, uint32_t pos) {
    if (pos >= len) return len;
    pos++;
    while (pos < len && ((unsigned char)buf[pos] & 0xC0u) == 0x80u) {
        pos++;
    }
    return pos;
}

static uint32_t syscall_builtin_read(uint32_t num,
                                     uint32_t argc,
                                     uint32_t *argv) {
    (void)num;

    if (argc < 2u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }
    if (process_in_process_context() &&
        !ipc_user_range(argv, 2u * sizeof(uint32_t))) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t max_len = argv[1];
    char **out_ptr = NULL;
    char *buffer = NULL;
    uint32_t capacity = 0u;

    if (max_len == 0u) {
        out_ptr = (char **)(uintptr_t)argv[0];
        if (out_ptr == NULL ||
            (process_in_process_context() && !ipc_user_range(out_ptr, sizeof(char *)))) {
            return IPO_SYSCALL_ENOSYS;
        }
        capacity = 256u;
        buffer = (char *)kmalloc(capacity);
        if (buffer == NULL) {
            return IPO_SYSCALL_ENOSYS;
        }
    } else {
        buffer = (char *)(uintptr_t)argv[0];
        if (buffer == NULL ||
            (process_in_process_context() && !ipc_user_range(buffer, max_len))) {
            return IPO_SYSCALL_ENOSYS;
        }
        capacity = max_len;
    }

    process_t *proc = process_get_current();
    uint32_t pid = proc ? proc->pid : 0u;

    /* If this process is not foreground, wait until it gains foreground before reading from console */
    while (!process_is_foreground()) {
        if (proc) {
            proc->waiting_for_input = true;
        }
        process_yield();
        if (proc && !proc->is_running) {
            if (max_len == 0u && out_ptr != NULL) {
                kfree(buffer);
                *out_ptr = NULL;
            }
            return (uint32_t)(-2);
        }
    }
    if (proc) {
        proc->waiting_for_input = false;
    }

    system_state_t prev_state = system_get_state();
    system_set_state(SYSTEM_STATE_TEXT_INPUT);
    keyboard_set_app_input_mode(true);

    serial_printf("[syscall_read] started: pid=%u max_len=%u (state=TEXT_INPUT)\n", pid, max_len);

    uint32_t len = 0u;
    uint32_t cursor_pos = 0u;
    size_t prev_vis_len = 0u;
    int32_t start_origin = (int32_t)vga_get_cursor_position();
    if (start_origin < VGA_START_CURSOR_POSITION || start_origin >= VGA_WIDTH * VGA_HEIGHT) {
        start_origin = VGA_START_CURSOR_POSITION;
        vga_set_cursor((uint16_t)start_origin);
    }
    int32_t start_cursor = start_origin;
    buffer[0] = '\0';

    for (;;) {
        if (!process_is_foreground()) {
            if (proc) {
                proc->waiting_for_input = true;
            }
            process_yield();
            if (proc && !proc->is_running) {
                keyboard_set_app_input_mode(false);
                system_set_state(proc ? SYSTEM_STATE_PROCESS_RUNNING : prev_state);
                if (max_len == 0u && out_ptr != NULL) {
                    kfree(buffer);
                    *out_ptr = NULL;
                }
                return (uint32_t)(-2);
            }
            continue;
        }
        if (proc) {
            proc->waiting_for_input = false;
        }

        uint8_t scancode = keyboard_wait_scancode();

        if (scancode == 0x00u) {
            if (system_is_interrupted() && process_is_foreground()) {
                keyboard_set_app_input_mode(false);
                system_set_state(proc ? SYSTEM_STATE_PROCESS_RUNNING : prev_state);
                if (max_len == 0u && out_ptr != NULL) {
                    kfree(buffer);
                    *out_ptr = NULL;
                }
                return (uint32_t)(-2);
            }
            continue;
        }

        update_hot_key_state(scancode);

        if (scancode & 0x80u) {
            continue;
        }

        if (keyboard_is_ctrl_pressed() && scancode == 0x2E) {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            size_t vis_len = terminal_visual_offset_range(buffer, len, len);
            int32_t cur = start_cursor + (int32_t)vis_len;
            if (cur < VGA_START_CURSOR_POSITION) cur = VGA_START_CURSOR_POSITION;
            if (cur >= VGA_WIDTH * VGA_HEIGHT) cur = VGA_WIDTH * VGA_HEIGHT - 1;
            vga_set_cursor((uint16_t)cur);
            printf("^C\n");
            system_request_interrupt();
            keyboard_set_app_input_mode(false);
            system_set_state(proc ? SYSTEM_STATE_PROCESS_RUNNING : prev_state);
            if (max_len == 0u && out_ptr != NULL) {
                kfree(buffer);
                *out_ptr = NULL;
            }
            return (uint32_t)(-2);
        }

        if (scancode == 0x20 && keyboard_is_ctrl_pressed()) {
            if (len == 0u) {
                keyboard_set_app_input_mode(false);
                system_set_state(proc ? SYSTEM_STATE_PROCESS_RUNNING : prev_state);
                if (max_len == 0u && out_ptr != NULL) {
                    kfree(buffer);
                    *out_ptr = NULL;
                }
                return (uint32_t)(-3);
            }
        }

        if (scancode == 0x49) {
            if (terminal_get_top_buffer_count() > 0) {
                terminal_scroll_up();
                start_cursor += VGA_WIDTH;
            }
            continue;
        }
        if (scancode == 0x51) {
            if (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            continue;
        }
        if (scancode == 0x48) {
            if (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_up();
                start_cursor += VGA_WIDTH;
                continue;
            }
            if (cursor_pos >= VGA_WIDTH) {
                cursor_pos -= VGA_WIDTH;
                while (cursor_pos > 0u && ((unsigned char)buffer[cursor_pos] & 0xC0u) == 0x80u) {
                    cursor_pos--;
                }
                terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                          &prev_vis_len, cursor_pos,
                                          proc ? proc->text_vram_backup : NULL, true);
            } else if (cursor_pos > 0) {
                cursor_pos = 0u;
                terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                          &prev_vis_len, cursor_pos,
                                          proc ? proc->text_vram_backup : NULL, true);
            }
            continue;
        }
        if (scancode == 0x50) {
            if (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
                continue;
            }
            if (cursor_pos + VGA_WIDTH <= len) {
                cursor_pos += VGA_WIDTH;
                while (cursor_pos < len && ((unsigned char)buffer[cursor_pos] & 0xC0u) == 0x80u) {
                    cursor_pos++;
                }
                terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                          &prev_vis_len, cursor_pos,
                                          proc ? proc->text_vram_backup : NULL, true);
            } else if (cursor_pos < len) {
                cursor_pos = len;
                terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                          &prev_vis_len, cursor_pos,
                                          proc ? proc->text_vram_backup : NULL, true);
            }
            continue;
        }
        if (scancode == 0x4B) {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            if (cursor_pos > 0u) {
                cursor_pos = utf8_prev_char(buffer, cursor_pos);
                terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                          &prev_vis_len, cursor_pos,
                                          proc ? proc->text_vram_backup : NULL, true);
            }
            continue;
        }
        if (scancode == 0x4D) {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            if (cursor_pos < len) {
                cursor_pos = utf8_next_char(buffer, len, cursor_pos);
                terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                          &prev_vis_len, cursor_pos,
                                          proc ? proc->text_vram_backup : NULL, true);
            }
            continue;
        }
        if (scancode == 0x47) {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            cursor_pos = 0u;
            terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                      &prev_vis_len, cursor_pos,
                                      proc ? proc->text_vram_backup : NULL, true);
            continue;
        }
        if (scancode == 0x4F) {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            cursor_pos = len;
            terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                      &prev_vis_len, cursor_pos,
                                      proc ? proc->text_vram_backup : NULL, true);
            continue;
        }
        if (scancode == 0x53) {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            if (cursor_pos < len) {
                uint32_t next_pos = utf8_next_char(buffer, len, cursor_pos);
                uint32_t del_bytes = next_pos - cursor_pos;
                memmove(&buffer[cursor_pos], &buffer[next_pos], len - next_pos + 1u);
                len -= del_bytes;
                buffer[len] = '\0';
                terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                          &prev_vis_len, cursor_pos,
                                          proc ? proc->text_vram_backup : NULL, true);
            }
            continue;
        }

        char ch = get_char(scancode);

        if (ch == '\r' || ch == '\n') {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            size_t vis_len = terminal_visual_offset_range(buffer, len, len);
            int32_t cur = start_cursor + (int32_t)vis_len;
            if (cur < VGA_START_CURSOR_POSITION) cur = VGA_START_CURSOR_POSITION;
            if (cur >= VGA_WIDTH * VGA_HEIGHT) cur = VGA_WIDTH * VGA_HEIGHT - 1;
            vga_set_cursor((uint16_t)cur);
            putchar('\n');
            buffer[len] = '\0';
            break;
        }

        if (ch == '\b' || ch == 127) {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            if (cursor_pos > 0u) {
                uint32_t prev_pos = utf8_prev_char(buffer, cursor_pos);
                uint32_t del_bytes = cursor_pos - prev_pos;
                memmove(&buffer[prev_pos], &buffer[cursor_pos], len - cursor_pos + 1u);
                cursor_pos = prev_pos;
                len -= del_bytes;
                buffer[len] = '\0';
                terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                          &prev_vis_len, cursor_pos,
                                          proc ? proc->text_vram_backup : NULL, true);
            }
            continue;
        }

        if (ch == '\t') {
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            if (max_len == 0u) {
                if (len + 1u >= capacity) {
                    if (capacity > UINT32_MAX / 2u - 16u) {
                        continue;
                    }
                    uint32_t new_cap = (capacity + 16u) * 2u;
                    char *new_buf = (char *)kmalloc(new_cap);
                    if (new_buf == NULL) {
                        continue;
                    }
                    memcpy(new_buf, buffer, len + 1);
                    kfree(buffer);
                    buffer = new_buf;
                    capacity = new_cap;
                }
            } else {
                if (len + 1u >= capacity) continue;
            }

            memmove(&buffer[cursor_pos + 1], &buffer[cursor_pos], len - cursor_pos + 1);
            buffer[cursor_pos] = '\t';
            cursor_pos++;
            len++;
            buffer[len] = '\0';
            terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                      &prev_vis_len, cursor_pos,
                                      proc ? proc->text_vram_backup : NULL, true);
            continue;
        }

        const char *out_str = keyboard_get_key_string(scancode);
        if (out_str != NULL && out_str[0] != '\0') {
            size_t slen = strlen(out_str);
            while (terminal_get_bottom_buffer_count() > 0) {
                terminal_scroll_down();
                start_cursor -= VGA_WIDTH;
            }
            if (max_len == 0u) {
                if (slen >= (size_t)(capacity - len)) {
                    if (slen > UINT32_MAX / 2u || capacity > UINT32_MAX / 2u - slen - 32u) {
                        continue;
                    }
                    uint32_t new_cap = (capacity + (uint32_t)slen + 32u) * 2u;
                    char *new_buf = (char *)kmalloc(new_cap);
                    if (new_buf == NULL) {
                        continue;
                    }
                    memcpy(new_buf, buffer, len + 1);
                    kfree(buffer);
                    buffer = new_buf;
                    capacity = new_cap;
                }
            } else {
                if (slen >= (size_t)(capacity - len)) {
                    continue;
                }
            }

            memmove(&buffer[cursor_pos + slen], &buffer[cursor_pos], len - cursor_pos + 1);
            memcpy(&buffer[cursor_pos], out_str, slen);
            cursor_pos += (uint32_t)slen;
            len += (uint32_t)slen;
            buffer[len] = '\0';
            terminal_render_text_line(&start_cursor, buffer, len, VGA_COLOR_LIGHT_GREY,
                                      &prev_vis_len, cursor_pos,
                                      proc ? proc->text_vram_backup : NULL, true);
        }
    }

    buffer[len] = '\0';

    if (max_len == 0u && out_ptr != NULL) {
        *out_ptr = buffer;
    }

    keyboard_set_app_input_mode(false);
    system_set_state(proc ? SYSTEM_STATE_PROCESS_RUNNING : prev_state);

    serial_printf("[syscall_read] completed: pid=%u bytes_read=%u text=\"%s\"\n",
                  pid, len, buffer);

    return len;
}

static uint32_t syscall_builtin_async_start(uint32_t num,
                                            uint32_t argc,
                                            uint32_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *task_name =
        (const char *)(uintptr_t)argv[0];

    uint32_t interval_ms =
        argv[1] ? argv[1] : 10000u;

    void (*task_fn)(void) =
        (void (*)(void))(uintptr_t)argv[2];

    if (task_fn == NULL) {
        serial_printf(
            "[syscall] async start rejected: null fn for '%s'\n",
            task_name ? task_name : "(null)");

        return IPO_SYSCALL_ENOSYS;
    }

    int result =
        async_start_task(task_name, interval_ms, task_fn);

    if (result < 0) {
        serial_printf(
            "[syscall] async start failed for '%s'\n",
            task_name ? task_name : "(null)");

        return IPO_SYSCALL_ENOSYS;
    }

    return (uint32_t)result;
}

static uint32_t syscall_builtin_async_stop(uint32_t num,
                                           uint32_t argc,
                                           uint32_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *task_name =
        (const char *)(uintptr_t)argv[0];

    return (uint32_t)async_stop_task(task_name);
}

static uint32_t syscall_builtin_var_set(uint32_t num,
                                       uint32_t argc,
                                       uint32_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *name = (const char *)(uintptr_t)argv[0];
    const void *value = (const void *)(uintptr_t)argv[1];
    uint32_t value_len = argv[2];

    if (name == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    if (set_shared_var(name, value, value_len) < 0) {
        serial_printf("[syscall] var set failed for '%s'\n", name);
        return IPO_SYSCALL_ENOSYS;
    }

    serial_printf("[syscall] var set: name='%s' bytes=%u\n", name, value_len);
    return IPO_SYSCALL_OK;
}

static uint32_t syscall_builtin_var_get(uint32_t num,
                                       uint32_t argc,
                                       uint32_t *argv) {
    (void)num;

    if (argc < 3u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *name = (const char *)(uintptr_t)argv[0];
    void *out_buffer = (void *)(uintptr_t)argv[1];
    uint32_t out_capacity = argv[2];

    if (name == NULL || out_buffer == NULL || out_capacity == 0u) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t out_len = 0u;
    int copied = get_shared_var(name, out_buffer, out_capacity, &out_len);
    if (copied < 0) {
        serial_printf("[syscall] var get failed for '%s'\n", name);
        return IPO_SYSCALL_ENOSYS;
    }

    serial_printf("[syscall] var get: name='%s' copied=%d total=%u\n",
                 name, copied, out_len);
    return copied;
}

static uint32_t syscall_builtin_var_delete(uint32_t num,
                                          uint32_t argc,
                                          uint32_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    const char *name = (const char *)(uintptr_t)argv[0];
    if (name == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    if (delete_shared_var(name) < 0) {
        serial_printf("[syscall] var delete failed for '%s'\n", name);
        return IPO_SYSCALL_ENOSYS;
    }

    serial_printf("[syscall] var delete: name='%s'\n", name);
    return IPO_SYSCALL_OK;
}

static uint32_t syscall_builtin_stack_grow(uint32_t num,
                                          uint32_t argc,
                                          uint32_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    process_t *proc = process_get_current();
    if (proc == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t before = proc->stack_size;
    int32_t delta = (int32_t)argv[0];
    int result = process_adjust_stack_size(proc, delta);
    if (result < 0) {
        serial_printf("[syscall] stack grow failed: before=%u delta=%d\n",
                     before, delta);
        return IPO_SYSCALL_ENOSYS;
    }

    serial_printf("[syscall] stack grow: before=%u added=%d now=%u\n",
                 before, delta, (uint32_t)result);
    return (uint32_t)result;
}

static uint32_t syscall_builtin_stack_shrink(uint32_t num,
                                            uint32_t argc,
                                            uint32_t *argv) {
    (void)num;

    if (argc < 1u || argv == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    process_t *proc = process_get_current();
    if (proc == NULL) {
        return IPO_SYSCALL_ENOSYS;
    }

    uint32_t before = proc->stack_size;
    uint32_t shrink = argv[0];
    int32_t delta = -(int32_t)shrink;
    int result = process_adjust_stack_size(proc, delta);
    if (result < 0) {
        serial_printf("[syscall] stack shrink failed: before=%u removed=%u\n",
                     before, shrink);
        return IPO_SYSCALL_ENOSYS;
    }

    serial_printf("[syscall] stack shrink: before=%u removed=%u now=%u\n",
                 before, shrink, (uint32_t)result);
    return (uint32_t)result;
}

static uint32_t user_heap_break = USER_HEAP_START;

static uint32_t syscall_builtin_sbrk(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    int32_t inc = 0;
    if (argc >= 1u && argv != NULL) {
        inc = (int32_t)argv[0];
    }
    process_t *proc = process_get_current();

    if (proc && proc->user_heap_base != 0) {
        if (proc->heap_chunk_count == 0) {
            if (!process_heap_ensure_capacity(proc, 1)) {
                return (uint32_t)-1;
            }
            proc->heap_chunk_count = 1;
            proc->heap_chunks[0].base = proc->user_heap_base;
            proc->heap_chunks[0].limit = proc->user_heap_limit;
            proc->heap_chunks[0].brk = proc->user_heap_break;
        }
        if (!proc->heap_chunks) {
            return (uint32_t)-1;
        }
        uint32_t active_idx = proc->heap_chunk_count - 1;
        uint32_t prev = proc->heap_chunks[active_idx].brk;
        if (inc == 0) {
            return prev;
        }
        if (inc > 0) {
            uint32_t current_brk = proc->heap_chunks[active_idx].brk;
            uint32_t current_limit = proc->heap_chunks[active_idx].limit;
            uint64_t needed64 = (uint64_t)current_brk + (uint32_t)inc;
            if (needed64 > 0xB0000000ULL) {
                return (uint32_t)-1;
            }
            uint32_t needed = (uint32_t)needed64;

            bool can_expand = false;
            uint32_t aligned_limit = 0;
            if (needed <= current_limit) {
                can_expand = true;
                aligned_limit = current_limit;
            } else {
                uint64_t aligned = ((uint64_t)needed + 0xFFFu) & ~0xFFFULL;
                if (aligned <= 0xB0000000ULL) {
                    aligned_limit = (uint32_t)aligned;
                    can_expand = process_heap_chunk_can_expand(proc, active_idx, aligned_limit);
                }
            }

            if (can_expand) {
                if (aligned_limit > current_limit) {
                    uint32_t mapped_start = current_limit & ~0xFFFu;
                    uint32_t mapped_end = aligned_limit;
                    if (mapped_end > mapped_start &&
                        !process_update_memory_mappings(
                            proc, mapped_start,
                            mapped_end - mapped_start,
                            true)) {
                        return (uint32_t)-1;
                    }
                    proc->heap_chunks[active_idx].limit = aligned_limit;
                    proc->user_heap_limit = aligned_limit;
                    serial_printf("[process] Dynamic on-demand heap expanded for pid=%u chunk %u: limit=0x%x (+%u KiB)\n",
                                  proc->pid, active_idx, aligned_limit, (uint32_t)inc / 1024);
                }
                if ((uint32_t)inc <= 65536u) {
                    memset((void *)(uintptr_t)current_brk, 0, (uint32_t)inc);
                } else {
                    memset((void *)(uintptr_t)current_brk, 0, 4096u);
                }
                proc->heap_chunks[active_idx].brk = needed;
                proc->user_heap_break = needed;
                return current_brk;
            } else {
                /* Current chunk is blocked by another process or boundary!
                 * Allocate a NEW discontinuous chunk (jumping over the other process)! */
                uint32_t new_chunk_base = process_heap_allocate_new_chunk(proc, (uint32_t)inc);
                if (new_chunk_base == 0) {
                    serial_printf("[process] Out of physical RAM for pid=%u heap: failed to allocate %u KiB\n",
                                  proc->pid, (uint32_t)inc / 1024);
                    return (uint32_t)-1;
                }
                if ((uint32_t)inc <= 65536u) {
                    memset((void *)(uintptr_t)new_chunk_base, 0, (uint32_t)inc);
                } else {
                    memset((void *)(uintptr_t)new_chunk_base, 0, 4096u);
                }
                return new_chunk_base;
            }
        } else {
            uint32_t dec = 0u - (uint32_t)inc;
            uint32_t current_brk = proc->heap_chunks[active_idx].brk;
            uint32_t current_base = proc->heap_chunks[active_idx].base;

            if (dec > current_brk - current_base) {
                return (uint32_t)-1;
            }
            uint32_t new_break = current_brk - dec;
            if (new_break == current_base && proc->heap_chunk_count > 1) {
                uint32_t mapped_start = current_base & ~0xFFFu;
                uint32_t mapped_end = (proc->heap_chunks[active_idx].limit + 0xFFFu) & ~0xFFFu;
                if (mapped_end > mapped_start) {
                    process_update_memory_mappings(proc, mapped_start, mapped_end - mapped_start, false);
                }
                proc->heap_chunk_count--;
                active_idx = proc->heap_chunk_count - 1;
                proc->user_heap_break = proc->heap_chunks[active_idx].brk;
                proc->user_heap_limit = proc->heap_chunks[active_idx].limit;
                return current_brk;
            }
            uint64_t aligned_limit = ((uint64_t)new_break + 0xFFFu) & ~0xFFFULL;
            if (aligned_limit > 0xB0000000ULL) {
                return (uint32_t)-1;
            }
            uint32_t new_limit = (uint32_t)aligned_limit;
            if (new_limit < proc->heap_chunks[active_idx].limit) {
                uint32_t mapped_start = new_limit & ~0xFFFu;
                uint32_t mapped_end = ((uint64_t)proc->heap_chunks[active_idx].limit + 0xFFFu) & ~0xFFFULL;
                if (mapped_end > mapped_start &&
                    !process_update_memory_mappings(
                        proc, mapped_start,
                        (uint32_t)(mapped_end - mapped_start),
                        false)) {
                    return (uint32_t)-1;
                }
                proc->heap_chunks[active_idx].limit = new_limit;
                proc->user_heap_limit = new_limit;
            }
            proc->heap_chunks[active_idx].brk = new_break;
            proc->user_heap_break = new_break;
            return current_brk;
        }
    }

    /* Fallback for system / non-process context */
    uint32_t prev = user_heap_break;
    if (inc == 0) {
        return prev;
    }
    if (inc > 0) {
        uint64_t total_ram = memory_map_get_total_ram();
        uint64_t limit_ram = total_ram > 0 ? total_ram : 0xB0000000ULL;
        if ((uint64_t)user_heap_break + (uint32_t)inc > limit_ram) {
            return (uint32_t)-1;
        }
        memset((void *)(uintptr_t)prev, 0, (uint32_t)inc);
        user_heap_break += (uint32_t)inc;
        return prev;
    } else {
        uint32_t dec = 0u - (uint32_t)inc;
        if (dec > user_heap_break - USER_HEAP_START) {
            return (uint32_t)-1;
        }
        user_heap_break -= dec;
        return prev;
    }
}

static uint32_t syscall_builtin_getpid(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    process_t *proc = process_get_current();
    return proc ? proc->pid : 1;
}

static uint32_t syscall_builtin_time(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    timer_update();
    uint32_t secs = timer_seconds();
    if (argc >= 1u && argv != NULL && argv[0] != 0u) {
        uint32_t *out = (uint32_t *)(uintptr_t)argv[0];
        *out = secs;
    }
    return secs;
}

static uint32_t syscall_builtin_millis(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    timer_update();
    return timer_millis();
}

static uint32_t syscall_builtin_elapsed_ms(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) {
        return 0u;
    }
    timer_update();
    return timer_elapsed_ms(argv[0]);
}

static uint32_t syscall_builtin_timer_after(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 2u || argv == NULL) {
        return 0u;
    }
    timer_update();
    return (uint32_t)timer_after(argv[0], argv[1]);
}

static uint32_t syscall_builtin_net_dns_resolve(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv || !argv[0] || !argv[1]) {
        return 0u;
    }
    const char *user_hostname = (const char *)(uintptr_t)argv[0];
    size_t host_len = strlen(user_hostname);
    char *hostname = (char *)kmalloc(host_len + 1u);
    if (!hostname) return 0u;
    memcpy(hostname, user_hostname, host_len + 1u);

    uint32_t timeout_ms = argv[2];
    uint32_t max_retries = (argc >= 4u) ? argv[3] : 0u;
    uint32_t retry_interval_ms = (argc >= 5u) ? argv[4] : 0u;

    ip4_addr_t resolved_ip;
    bool ok = dns_resolve_ex(hostname, &resolved_ip, timeout_ms, max_retries, retry_interval_ms);
    kfree(hostname);
    if (!ok) {
        return 0u;
    }
    *(ip4_addr_t *)(uintptr_t)argv[1] = resolved_ip;
    return 1u;
}

static uint32_t syscall_builtin_net_icmp_send(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 6u || !argv || (argv[5] > 0u && !argv[4]) || argv[5] > UINT16_MAX) {
        return (uint32_t)-1;
    }
    return (uint32_t)icmp_send_echo(argv[0], (uint16_t)argv[1], (uint16_t)argv[2],
                                    (uint8_t)argv[3], (const void *)(uintptr_t)argv[4],
                                    (uint16_t)argv[5]);
}

static uint32_t syscall_builtin_net_icmp_poll(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 4u || !argv || !argv[2]) {
        return 0u;
    }
    return icmp_poll_reply((uint16_t)argv[0], (uint16_t)argv[1],
                           (icmp_echo_reply_t *)(uintptr_t)argv[2], argv[3]) ? 1u : 0u;
}

static uint32_t syscall_builtin_net_socket(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv) {
        return (uint32_t)-1;
    }
    return (uint32_t)socket((int)argv[0], (int)argv[1], (int)argv[2]);
}

static uint32_t syscall_builtin_net_bind(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv || !argv[1] || argv[2] < sizeof(struct sockaddr_in)) {
        return (uint32_t)-1;
    }
    struct sockaddr_in address;
    memcpy(&address, (const void *)(uintptr_t)argv[1], sizeof(address));
    return (uint32_t)bind((int)argv[0], (const struct sockaddr *)&address, sizeof(address));
}

static uint32_t syscall_builtin_net_listen(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 2u || !argv) {
        return (uint32_t)-1;
    }
    return (uint32_t)listen((int)argv[0], (int)argv[1]);
}

static uint32_t syscall_builtin_net_accept(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 4u || !argv) {
        return (uint32_t)-1;
    }
    struct sockaddr_in address;
    socklen_t address_len = sizeof(address);
    int client_fd = accept_timeout((int)argv[0],
                                   argv[1] ? (struct sockaddr *)&address : NULL,
                                   argv[1] ? &address_len : NULL,
                                   argv[3]);
    if (client_fd >= 0 && argv[1]) {
        memcpy((void *)(uintptr_t)argv[1], &address, sizeof(address));
        if (argv[2]) {
            *(socklen_t *)(uintptr_t)argv[2] = address_len;
        }
    }
    return (uint32_t)client_fd;
}

static uint32_t syscall_builtin_net_connect(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv || !argv[1] || argv[2] < sizeof(struct sockaddr_in)) {
        return (uint32_t)-1;
    }
    struct sockaddr_in address;
    memcpy(&address, (const void *)(uintptr_t)argv[1], sizeof(address));
    return (uint32_t)connect((int)argv[0], (const struct sockaddr *)&address, sizeof(address));
}

static uint32_t syscall_builtin_net_connect_endpoint(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv || !argv[1] || argv[2] > UINT16_MAX) {
        return (uint32_t)-1;
    }
    const char *user_host = (const char *)(uintptr_t)argv[1];
    size_t host_len = strlen(user_host);
    char *host = (char *)kmalloc(host_len + 1u);
    if (!host) {
        return (uint32_t)-1;
    }
    memcpy(host, user_host, host_len + 1u);
    uint32_t ret = (uint32_t)connect_endpoint((int)argv[0], host, (uint16_t)argv[2]);
    kfree(host);
    return ret;
}

static uint32_t syscall_builtin_net_send(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 4u || !argv || !argv[1] || argv[2] == 0u) {
        return (uint32_t)-1;
    }
    void *buffer = kmalloc(argv[2]);
    if (!buffer) {
        return (uint32_t)-1;
    }
    memcpy(buffer, (const void *)(uintptr_t)argv[1], argv[2]);
    ssize_t result = send((int)argv[0], buffer, argv[2], (int)argv[3]);
    kfree(buffer);
    return (uint32_t)result;
}

static uint32_t syscall_builtin_net_recv(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 4u || !argv || !argv[1] || argv[2] == 0u) {
        return (uint32_t)-1;
    }
    void *buffer = kmalloc(argv[2]);
    if (!buffer) {
        return (uint32_t)-1;
    }
    ssize_t result = recv((int)argv[0], buffer, argv[2], (int)argv[3]);
    if (result > 0) {
        memcpy((void *)(uintptr_t)argv[1], buffer, (size_t)result);
    }
    kfree(buffer);
    return (uint32_t)result;
}

static uint32_t syscall_builtin_net_close(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv) {
        return (uint32_t)-1;
    }
    return (uint32_t)socket_close((int)argv[0]);
}

static uint32_t syscall_builtin_net_poll(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    (void)argc;
    (void)argv;
    net_poll();
    return 0u;
}

static uint32_t syscall_builtin_net_get_interface(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) {
        return 0u;
    }
    net_if_t *netif = net_get_interface();
    if (!netif) {
        return 0u;
    }
    memcpy((void *)(uintptr_t)argv[0], netif, sizeof(*netif));
    return 1u;
}

static uint32_t syscall_builtin_net_init(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    (void)argc;
    (void)argv;
    return net_init() ? 1u : 0u;
}

static uint32_t syscall_builtin_net_set_ip(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 4u || !argv) {
        return (uint32_t)-1;
    }
    net_set_ip(argv[0], argv[1], argv[2], argv[3]);
    return 0u;
}

static uint32_t syscall_builtin_free(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc >= 1u && argv != NULL && argv[0] != 0u) {
        kfree((void *)(uintptr_t)argv[0]);
    }
    return 0;
}

static uint32_t syscall_builtin_exit(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    int code = 0;
    if (argc > 0 && argv != NULL) {
        code = (int)argv[0];
    }
    sound_stop();
    process_t *proc = process_get_current();
    if (proc != NULL) {
        wm_destroy_raw_gfx_window_for_pid(proc->pid);
        proc->is_running = 0;
        proc->exit_code = code;
        if (!process_get_separate_windows() ||
            (proc->is_wm_app && !wm_has_windows_for_pid(proc->pid)) ||
            (proc->wants_graphics && !proc->is_wm_app)) {
            proc->completed_and_acknowledged = true;
        }
        if (!proc->return_val_explicit) {
            process_set_current_return_value(PROC_RET_INT, "int", NULL, code, NULL, 0);
        }
    }
    process_set_last_exit_code(code);
    process_yield_kernel();
    while (1) {
        process_yield_kernel();
    }
    return 0;
}

void syscall_reset_user_heap(void) {
    user_heap_break = USER_HEAP_START;
}

static uint32_t syscall_builtin_wm_create_window(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc;
    if (!argv || !argv[0]) return 0;
    const wm_window_options_t *opts = (const wm_window_options_t *)(uintptr_t)argv[0];
    return (uint32_t)(uintptr_t)wm_create_window(opts);
}

static uint32_t syscall_builtin_wm_destroy_window(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc;
    if (!argv || !argv[0]) return 0;
    wm_destroy_window((wm_window_t *)(uintptr_t)argv[0]);
    return 0;
}

static uint32_t syscall_builtin_wm_session_start(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    wm_session_start();
    return 0;
}

static uint32_t syscall_builtin_wm_session_stop(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    wm_session_stop();
    return 0;
}

static uint32_t syscall_builtin_wm_session_active(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)wm_session_active();
}

static uint32_t syscall_builtin_wm_get_count(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)wm_get_window_count();
}

static uint32_t syscall_builtin_wm_get_focused(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)(uintptr_t)wm_get_focused();
}

static uint32_t syscall_builtin_wm_get_list(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)(uintptr_t)wm_get_window_list();
}

static uint32_t syscall_builtin_wm_set_focus(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc;
    if (!argv || !argv[0]) return 0;
    wm_set_focus((wm_window_t *)(uintptr_t)argv[0]);
    return 0;
}

static uint32_t syscall_builtin_wm_focus_next(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    wm_focus_next();
    return 0;
}

static uint32_t syscall_builtin_wm_focus_prev(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    wm_focus_prev();
    return 0;
}

static uint32_t syscall_builtin_wm_invalidate(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc;
    if (!argv || !argv[0]) return 0;
    wm_invalidate((wm_window_t *)(uintptr_t)argv[0]);
    return 0;
}

static uint32_t syscall_builtin_wm_is_valid(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc;
    if (!argv || !argv[0]) return 0;
    return wm_is_window_valid((wm_window_t *)(uintptr_t)argv[0]) ? 1u : 0u;
}

static uint32_t syscall_builtin_wm_toggle_maximize(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc;
    if (!argv || !argv[0]) return 0;
    wm_toggle_maximize((wm_window_t *)(uintptr_t)argv[0]);
    return 0;
}

static uint32_t syscall_builtin_wm_resize(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc;
    if (!argv || argc < 3 || !argv[0]) return 0;
    return wm_resize((wm_window_t *)(uintptr_t)argv[0], (uint16_t)argv[1], (uint16_t)argv[2]) ? 1u : 0u;
}

static wm_window_t *syscall_wm_window_arg(uint32_t argc, uint32_t *argv) {
    if (argc < 1u || !argv || !argv[0]) return NULL;
    wm_window_t *win = (wm_window_t *)(uintptr_t)argv[0];
    return wm_is_window_valid(win) ? win : NULL;
}

static uint32_t syscall_builtin_wm_init(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    if (wm_get_window_count() == 0) wm_init();
    return 0;
}

static uint32_t syscall_builtin_wm_invalidate_all(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    wm_invalidate_all();
    return 0;
}

static uint32_t syscall_builtin_wm_set_fullscreen(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    wm_window_t *win = syscall_wm_window_arg(argc, argv);
    if (win && argc >= 2u) wm_set_fullscreen(win, argv[1] != 0u);
    return 0;
}

static uint32_t syscall_builtin_wm_minimize(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    wm_window_t *win = syscall_wm_window_arg(argc, argv);
    if (win) wm_minimize(win);
    return 0;
}

static uint32_t syscall_builtin_wm_restore(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    wm_window_t *win = syscall_wm_window_arg(argc, argv);
    if (win) wm_restore(win);
    return 0;
}

static uint32_t syscall_builtin_wm_move(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    wm_window_t *win = syscall_wm_window_arg(argc, argv);
    if (win && argc >= 3u) {
        wm_move(win, (int16_t)argv[1], (int16_t)argv[2]);
    }
    return 0;
}

static uint32_t syscall_builtin_wm_set_title(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    wm_window_t *win = syscall_wm_window_arg(argc, argv);
    if (win && argc >= 2u) {
        wm_set_title(win, (const char *)(uintptr_t)argv[1]);
    }
    return 0;
}

static uint32_t syscall_builtin_wm_set_shape(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    wm_window_t *win = syscall_wm_window_arg(argc, argv);
    if (win && argc >= 4u) {
        wm_set_shape(win, (const uint8_t *)(uintptr_t)argv[1],
                     (uint16_t)argv[2], (uint16_t)argv[3]);
    }
    return 0;
}

static uint32_t syscall_builtin_wm_dispatch_key(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return 0;
    return wm_dispatch_key((uint8_t)argv[0]) ? 1u : 0u;
}

static uint32_t syscall_builtin_wm_compose(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc >= 1u && argv && argv[0]) {
        wm_compose((uint8_t *)(uintptr_t)argv[0]);
    }
    return 0;
}

static uint32_t syscall_builtin_wm_compositor_tick(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    wm_compositor_tick();
    return 0;
}

static uint32_t syscall_builtin_wm_session_run(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    wm_session_run();
    return 0;
}

static uint32_t syscall_builtin_wm_get_count_for_pid(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return 0;
    return (uint32_t)wm_get_window_count_for_pid(argv[0]);
}

static uint32_t syscall_builtin_wm_has_windows_for_pid(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return 0;
    return wm_has_windows_for_pid(argv[0]) ? 1u : 0u;
}

static uint32_t syscall_builtin_wm_focus_window_for_pid(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc >= 1u && argv) wm_focus_window_for_pid(argv[0]);
    return 0;
}

static uint32_t syscall_builtin_process_yield(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    process_t *cur = process_get_current();
    if (cur && argc > 0 && argv) {
        cur->waiting_for_input = (argv[0] != 0);
    }
    process_yield_kernel();
    return 0;
}

static uint32_t syscall_builtin_process_is_foreground(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    if (!process_in_process_context()) {
        return 1;
    }
    process_t *cur = process_get_current();
    process_t *fg = process_get_foreground();
    if (!fg || !cur) return 1;
    bool is_fg = (cur == fg);
    return is_fg ? 1 : 0;
}

static uint32_t syscall_builtin_process_is_batch_active(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return process_is_batch_active() ? 1 : 0;
}

static uint32_t syscall_builtin_process_switch_fg(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (!process_is_batch_active()) return 0;
    if (argc > 0 && argv) {
        if (argv[0] == 1) {
            process_switch_foreground_prev();
        } else if (argv[0] >= 10) {
            process_switch_foreground_to_workspace((int)(argv[0] - 10));
        } else {
            process_switch_foreground_next();
        }
    } else {
        process_switch_foreground_next();
    }
    return 0;
}

static bool syscall_cur_owns_foreground_vga(process_t *cur);

static uint32_t syscall_builtin_vga_set_mode(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc == 0 || !argv) return 0;
    if (argv[0] == 2 && argc >= 2 && argv[1] != 0) {
        vga_gfx_flip((const uint8_t *)(uintptr_t)argv[1]);
        return 0;
    }
    if (argv[0] == 3 && argc >= 2 && argv[1] != 0) {
        if (argc >= 4u) {
            vga_gfx_flip_scaled_buffer((const uint8_t *)(uintptr_t)argv[1],
                                       (int)argv[2],
                                       (int)argv[3]);
        } else {
            vga_gfx_flip_desktop((const uint8_t *)(uintptr_t)argv[1]);
        }
        return 0;
    }
    if (argv[0] == 4 && argc >= 3) {
        return vga_gfx_set_resolution((int)argv[1], (int)argv[2]) ? 1u : 0u;
    }
    process_t *cur = process_get_current();
    if (argv[0] == 1) {
        if (cur) cur->wants_graphics = true;
        if (cur && (wm_has_raw_gfx_window_for_pid(cur->pid) ||
                    process_workspace_get_proc_count(cur->window_id) > 1 ||
                    (wm_session_active() && !process_get_separate_windows()))) {
            wm_ensure_raw_gfx_window(cur);
        } else if (syscall_cur_owns_foreground_vga(cur)) {
            vga_set_mode_13h_hardware();
        }
    } else {
        if (cur) {
            cur->wants_graphics = false;
            wm_destroy_raw_gfx_window_for_pid(cur->pid);
        }
        if (syscall_cur_owns_foreground_vga(cur) && !wm_session_active()) {
            if (vga_is_graphics_mode()) {
                vga_set_mode_text_hardware();
            }
        }
    }
    return 0;
}

static uint32_t syscall_builtin_vga_get_mode(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return vga_is_graphics_mode() ? 1 : 0;
}

static uint32_t syscall_builtin_vga_get_bounds(uint32_t num,
                                                uint32_t argc,
                                                uint32_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[0] || !argv[1]) {
        return IPO_SYSCALL_ENOSYS;
    }

    int32_t *w_ptr = (int32_t *)(uintptr_t)argv[0];
    int32_t *h_ptr = (int32_t *)(uintptr_t)argv[1];
    if (process_in_process_context() &&
        (!ipc_user_range(argv, 2u * sizeof(uint32_t)) ||
         !ipc_user_range(w_ptr, sizeof(int32_t)) ||
         !ipc_user_range(h_ptr, sizeof(int32_t)))) {
        return IPO_SYSCALL_ENOSYS;
    }

    int width;
    int height;
    vga_gfx_get_screen_bounds(&width, &height);
    *w_ptr = width;
    *h_ptr = height;
    return 0;
}

static uint32_t syscall_builtin_system_interrupt(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc > 0 && argv) {
        if (argv[0] == 1u) {
            system_request_interrupt();
            serial_printf("[syscall] system_interrupt requested\n");
            return 1u;
        }
        if (argv[0] == 0u) {
            system_clear_interrupt();
            return 0u;
        }
    }
    return system_is_interrupted() ? 1u : 0u;
}

static uint32_t syscall_builtin_kbd_get_scancode(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)keyboard_get_scancode();
}

static uint32_t syscall_builtin_kbd_wait_scancode(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)keyboard_wait_scancode();
}

static uint32_t syscall_builtin_kbd_set_app_mode(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (process_is_batch_active() && process_in_process_context()) {
        process_t *cur = process_get_current();
        process_t *fg = process_get_foreground();
        if (cur && fg && cur != fg) return 0;
    }
    if (argc > 0 && argv) {
        keyboard_set_app_input_mode(argv[0] != 0);
    }
    return 0;
}

static uint32_t syscall_builtin_kbd_flush(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    if (process_is_batch_active() && process_in_process_context()) {
        process_t *cur = process_get_current();
        process_t *fg = process_get_foreground();
        if (cur && fg && cur != fg) return 0;
    }
    keyboard_flush_queue();
    keyboard_flush_app_queue();
    keyboard_flush_hardware();
    keyboard_clear_key_state();
    return 0;
}

static uint32_t syscall_builtin_kbd_get_modifiers(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return (uint32_t)keyboard_get_modifiers();
}

static uint32_t syscall_builtin_kbd_poll(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    keyboard_poll();
    return 0;
}

static uint32_t syscall_builtin_kbd_get_key_string(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || argv == NULL) return 0;
    uint8_t scancode = (uint8_t)argv[0];
    const char *s = keyboard_get_key_string(scancode);
    if (!s) return 0;
    if (argc >= 3u && argv[1] != 0 && argv[2] > 0) {
        char *user_buf = (char *)(uintptr_t)argv[1];
        uint32_t max_len = argv[2];
        strncpy(user_buf, s, max_len - 1);
        user_buf[max_len - 1] = '\0';
        return 1;
    }
    return (uint32_t)(uintptr_t)s;
}

static uint32_t syscall_builtin_mouse_get_state(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc > 0 && argv && argv[0]) {
        mouse_get_state((mouse_state_t *)(uintptr_t)argv[0]);
    }
    return 0;
}

static uint32_t syscall_builtin_mouse_get_motion(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[0] || !argv[1]) {
        return IPO_SYSCALL_ENOSYS;
    }
    mouse_get_cumulative_motion((int64_t *)(uintptr_t)argv[0],
                                (int64_t *)(uintptr_t)argv[1]);
    return 0;
}

static bool syscall_cur_owns_foreground_vga(process_t *cur) {
    if (!cur || !process_is_batch_active() || !process_get_separate_windows()) {
        return true;
    }
    process_t *fg = process_get_displayed_foreground();
    return (!fg || fg->window_id == cur->window_id);
}

static uint32_t syscall_builtin_vga_set_cursor(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc > 0 && argv) {
        uint16_t pos = (uint16_t)argv[0];
        process_t *cur = process_get_current();
        if (cur) {
            cur->text_cursor_pos = pos;
            process_sync_workspace_text_state(cur);
        }
        if (syscall_cur_owns_foreground_vga(cur)) {
            vga_set_cursor(pos);
        }
    }
    return 0;
}

static uint32_t syscall_builtin_vga_get_cursor(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    process_t *cur = process_get_current();
    if (cur && !syscall_cur_owns_foreground_vga(cur)) {
        return (uint32_t)cur->text_cursor_pos;
    }
    return (uint32_t)vga_get_cursor_position();
}

static uint32_t syscall_builtin_vga_show_cursor(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    process_t *cur = process_get_current();
    if (cur) {
        cur->text_cursor_visible = true;
        process_sync_workspace_text_state(cur);
    }
    if (syscall_cur_owns_foreground_vga(cur)) {
        vga_show_cursor();
    }
    return 0;
}

static uint32_t syscall_builtin_vga_hide_cursor(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    process_t *cur = process_get_current();
    if (cur) {
        cur->text_cursor_visible = false;
        process_sync_workspace_text_state(cur);
    }
    if (syscall_cur_owns_foreground_vga(cur)) {
        vga_hide_cursor();
    }
    return 0;
}

static uint32_t syscall_builtin_vga_set_palette(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc >= 4 && argv) {
        vga_gfx_set_palette((uint8_t)argv[0], (uint8_t)argv[1], (uint8_t)argv[2], (uint8_t)argv[3]);
    }
    return 0;
}

static uint32_t syscall_builtin_vga_set_overlay_flags(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    process_t *cur = process_get_current();
    if (cur && argc > 0 && argv) {
        cur->gfx_overlay_flags = argv[0];
    }
    return 0;
}

static uint32_t syscall_builtin_vga_get_overlay_flags(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    process_t *cur = process_get_current();
    return cur ? cur->gfx_overlay_flags : 0;
}

static uint32_t syscall_builtin_vga_get_text_info(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv) return IPO_SYSCALL_ENOSYS;
    vga_get_text_mode_info((uint16_t *)(uintptr_t)argv[0],
                           (uint16_t *)(uintptr_t)argv[1],
                           (uint32_t *)(uintptr_t)argv[2]);
    return 0;
}

static uint32_t syscall_builtin_audio_init(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return audio_init() ? 1u : 0u;
}

static uint32_t syscall_builtin_audio_tick(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    audio_tick();
    return 0;
}

static uint32_t syscall_builtin_audio_stop_all(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    if (process_is_batch_active()) {
        process_t *cur = process_get_current();
        if (cur) {
            audio_cleanup_streams_for_owner(cur);
        }
        return 0;
    }
    audio_stop_all();
    sb16_stop();
    return 0;
}

static uint32_t syscall_builtin_audio_stream_create(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc >= 5 && argv) {
        return (uint32_t)(uintptr_t)audio_stream_create((int)argv[0], (int)argv[1], (uint32_t)argv[2], (const void *)(uintptr_t)argv[3], (size_t)argv[4]);
    }
    return 0;
}

static uint32_t syscall_builtin_audio_stream_create_ring(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc >= 6 && argv) {
        return (uint32_t)(uintptr_t)audio_stream_create_ring((int)argv[0], (int)argv[1], (uint32_t)argv[2],
                                                             (const void *)(uintptr_t)argv[3], (size_t)argv[4], (size_t)argv[5]);
    }
    return 0;
}

static uint32_t syscall_builtin_audio_stream_destroy(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc >= 1 && argv && argv[0]) {
        audio_stream_destroy((audio_stream_t *)(uintptr_t)argv[0]);
    }
    return 0;
}

static uint32_t syscall_builtin_audio_stream_play(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc >= 1 && argv && argv[0]) {
        audio_stream_play((audio_stream_t *)(uintptr_t)argv[0]);
    }
    return 0;
}

static uint32_t syscall_builtin_audio_stream_pause(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc >= 1 && argv && argv[0]) {
        audio_stream_pause((audio_stream_t *)(uintptr_t)argv[0]);
    }
    return 0;
}

static uint32_t syscall_builtin_audio_stream_stop(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc >= 1 && argv && argv[0]) {
        audio_stream_stop((audio_stream_t *)(uintptr_t)argv[0]);
    }
    return 0;
}

static uint32_t syscall_builtin_audio_stream_seek(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc >= 2 && argv && argv[0]) {
        double frac = *(const double *)(uintptr_t)argv[1];
        audio_stream_seek((audio_stream_t *)(uintptr_t)argv[0], frac);
    }
    return 0;
}

static uint32_t syscall_builtin_audio_stream_set_vol(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc >= 2 && argv && argv[0]) {
        float vol = *(const float *)(uintptr_t)argv[1];
        audio_stream_set_volume((audio_stream_t *)(uintptr_t)argv[0], vol);
    }
    return 0;
}

static uint32_t syscall_builtin_audio_stream_progress(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc >= 2 && argv && argv[0] && argv[1]) {
        double prog = audio_stream_get_progress((audio_stream_t *)(uintptr_t)argv[0]);
        *(double *)(uintptr_t)argv[1] = prog;
    }
    return 0;
}

static uint32_t syscall_builtin_audio_stream_update(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc >= 3 && argv && argv[0]) {
        audio_stream_update_buffered((audio_stream_t *)(uintptr_t)argv[0], (size_t)argv[1], (bool)(argv[2] != 0));
    }
    return 0;
}

static uint32_t syscall_caller_pid(void) {
    process_t *p = process_get_current();
    return p ? p->pid : 0u;
}

static uint32_t syscall_builtin_input_poll_key_ext(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return (uint32_t)-1;
    return ipo_input_poll_key_event(syscall_caller_pid(), (ipo_key_event_t *)(uintptr_t)argv[0]) ? 1u : 0u;
}

static uint32_t syscall_builtin_input_get_key_state(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return 0u;
    return ipo_input_is_key_down(argv[0]) ? 1u : 0u;
}

static uint32_t syscall_builtin_input_get_modifiers(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return ipo_input_get_modifiers();
}

static uint32_t syscall_builtin_shortcut_register(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return 0u;
    return ipo_shortcut_register((const ipo_shortcut_spec_t *)(uintptr_t)argv[0], syscall_caller_pid());
}

static uint32_t syscall_builtin_shortcut_unregister(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)-1;
    return (uint32_t)ipo_shortcut_unregister(argv[0], syscall_caller_pid());
}

static uint32_t syscall_builtin_shortcut_poll(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return (uint32_t)-1;
    return ipo_shortcut_poll(syscall_caller_pid(), (ipo_shortcut_event_t *)(uintptr_t)argv[0]) ? 1u : 0u;
}

static uint32_t syscall_builtin_input_dev_register(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return 0u;
    return ipo_input_dev_register((const ipo_input_dev_desc_t *)(uintptr_t)argv[0], syscall_caller_pid());
}

static uint32_t syscall_builtin_input_dev_unregister(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)-1;
    return (uint32_t)ipo_input_dev_unregister(argv[0], syscall_caller_pid());
}

static uint32_t syscall_builtin_input_dev_emit(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[1]) return (uint32_t)-1;
    return (uint32_t)ipo_input_dev_emit(argv[0], (const ipo_input_event_t *)(uintptr_t)argv[1], syscall_caller_pid());
}

static uint32_t syscall_builtin_input_dev_list(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 2u || !argv) return 0u;
    return ipo_input_dev_list((ipo_input_dev_info_t *)(uintptr_t)argv[0], argv[1]);
}

static uint32_t syscall_builtin_input_dev_poll_event(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[1]) return (uint32_t)-1;
    return ipo_input_dev_poll_event(syscall_caller_pid(), argv[0], (ipo_input_event_t *)(uintptr_t)argv[1]) ? 1u : 0u;
}

static uint32_t syscall_builtin_input_dev_get_state(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 4u || !argv || !argv[3]) return (uint32_t)-1;
    return (uint32_t)ipo_input_dev_get_state(argv[0], (uint16_t)argv[1], argv[2], (int32_t *)(uintptr_t)argv[3]);
}

static uint32_t syscall_builtin_mouse_inject(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 5u || !argv) return (uint32_t)-1;
    mouse_inject_event((int32_t)argv[0], (int32_t)argv[1], (int32_t)argv[2], argv[3], argv[4] != 0u);
    return 0u;
}

static uint32_t syscall_builtin_ps2_hook_register(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 2u || !argv) return 0u;
    return ipo_ps2_hook_register(argv[0], argv[1], syscall_caller_pid());
}

static uint32_t syscall_builtin_ps2_hook_unregister(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)-1;
    return (uint32_t)ipo_ps2_hook_unregister(argv[0], syscall_caller_pid());
}

static uint32_t syscall_builtin_ps2_hook_poll(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv || !argv[1]) return 0u;
    return ipo_ps2_hook_poll(argv[0], syscall_caller_pid(), (ipo_ps2_byte_event_t *)(uintptr_t)argv[1], argv[2]);
}

static uint32_t syscall_builtin_ps2_transact(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return (uint32_t)-1;
    return (uint32_t)ipo_ps2_transact((const ipo_ps2_transact_req_t *)(uintptr_t)argv[0]);
}

static uint32_t syscall_builtin_irq_subscribe(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return 0u;
    return irq_subscribe((uint8_t)argv[0], syscall_caller_pid());
}

static uint32_t syscall_builtin_irq_unsubscribe(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)-1;
    return (uint32_t)irq_unsubscribe(argv[0], syscall_caller_pid());
}

static uint32_t syscall_builtin_irq_poll(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv) return 0u;
    return irq_poll(argv[0], syscall_caller_pid(), (uint32_t *)(uintptr_t)argv[1], (uint64_t *)(uintptr_t)argv[2]);
}

static uint32_t syscall_builtin_irq_get_count(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return 0u;
    return irq_get_count((uint8_t)argv[0]);
}

static uint32_t syscall_builtin_irq_register_cb(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv || !argv[1]) return 0u;
    return irq_register_handler((uint8_t)argv[0], (irq_handler_fn_t)(uintptr_t)argv[1],
                                (void *)(uintptr_t)argv[2], syscall_caller_pid());
}

static uint32_t syscall_builtin_irq_unregister_cb(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)-1;
    return (uint32_t)irq_unregister_handler(argv[0], syscall_caller_pid());
}

static uint32_t syscall_builtin_hw_map_mmio(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv) return 0u;
    return (uint32_t)(uintptr_t)ipo_hw_map_mmio(argv[0], argv[1], argv[2], syscall_caller_pid());
}

static uint32_t syscall_builtin_hw_unmap_mmio(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 2u || !argv) return (uint32_t)-1;
    return (uint32_t)ipo_hw_unmap_mmio((void *)(uintptr_t)argv[0], argv[1], syscall_caller_pid());
}

static uint32_t syscall_builtin_hw_alloc_dma(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv) return 0u;
    return (uint32_t)(uintptr_t)ipo_hw_alloc_dma(argv[0], argv[1], (uint32_t *)(uintptr_t)argv[2], syscall_caller_pid());
}

static uint32_t syscall_builtin_hw_free_dma(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)-1;
    return (uint32_t)ipo_hw_free_dma((void *)(uintptr_t)argv[0], syscall_caller_pid());
}

static uint32_t syscall_builtin_hw_virt_to_phys(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return 0u;
    return ipo_hw_virt_to_phys((const void *)(uintptr_t)argv[0], syscall_caller_pid());
}

static uint32_t syscall_builtin_pci_get_count(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return pci_get_count();
}

static uint32_t syscall_builtin_pci_get_device(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[1]) return (uint32_t)-1;
    return (uint32_t)ipo_pci_fill_info(argv[0], (ipo_pci_dev_info_t *)(uintptr_t)argv[1]);
}

static uint32_t syscall_builtin_pci_config_read(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 5u || !argv) return 0xFFFFFFFFu;
    uint8_t bus = (uint8_t)argv[0], slot = (uint8_t)argv[1], func = (uint8_t)argv[2], off = (uint8_t)argv[3];
    if (argv[4] == 1u) return (uint32_t)pci_read_byte(bus, slot, func, off);
    if (argv[4] == 2u) return (uint32_t)pci_read_word(bus, slot, func, off);
    return pci_read_dword(bus, slot, func, off);
}

static uint32_t syscall_builtin_pci_config_write(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 6u || !argv) return (uint32_t)-1;
    uint8_t bus = (uint8_t)argv[0], slot = (uint8_t)argv[1], func = (uint8_t)argv[2], off = (uint8_t)argv[3];
    if (argv[4] == 1u) pci_write_byte(bus, slot, func, off, (uint8_t)argv[5]);
    else if (argv[4] == 2u) pci_write_word(bus, slot, func, off, (uint16_t)argv[5]);
    else pci_write_dword(bus, slot, func, off, argv[5]);
    return 0u;
}

static uint32_t syscall_builtin_pci_enable_bus_master(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv) return (uint32_t)-1;
    pci_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.bus = (uint8_t)argv[0];
    dev.slot = (uint8_t)argv[1];
    dev.func = (uint8_t)argv[2];
    pci_enable_bus_mastering(&dev);
    return 0u;
}

static uint32_t syscall_builtin_dev_get_count(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return device_get_count();
}

static uint32_t syscall_builtin_dev_get_info(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 2u || !argv || !argv[1]) return (uint32_t)-1;
    return (uint32_t)ipo_device_fill_info(argv[0], (ipo_device_info_t *)(uintptr_t)argv[1]);
}

static uint32_t syscall_builtin_dev_register(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return 0u;
    return ipo_device_register_user((const ipo_device_info_t *)(uintptr_t)argv[0], syscall_caller_pid());
}

static uint32_t syscall_builtin_dev_unregister(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)-1;
    return (uint32_t)ipo_device_unregister_user(argv[0], syscall_caller_pid());
}

void syscall_load_idt(void) {

    ipo_idt_ptr_t idt_ptr = {
        .limit = (uint16_t)(sizeof(ipo_idt_table) - 1),
        .base = (uint32_t)ipo_idt_table
    };

    __asm__ volatile(
        "lidt %0"
        :
        : "m"(idt_ptr));
}

void syscall_init(void) {
    memset(ipo_idt_table, 0, sizeof(ipo_idt_table));
    if (!ipc_init()) {
        serial_printf("[syscall] IPC subsystem unavailable\n");
    }


    ipo_register_syscall(
        IPO_SYSCALL_REGISTER,
        syscall_builtin_register);

    ipo_register_syscall(
        IPO_SYSCALL_CALL,
        syscall_builtin_call);

    ipo_register_syscall(
        IPO_SYSCALL_PRINT,
        syscall_builtin_print);

    ipo_register_syscall(
        IPO_SYSCALL_WRITE,
        syscall_builtin_write);

    ipo_register_syscall(
        IPO_SYSCALL_FS_OPEN,
        syscall_builtin_fs_open);

    ipo_register_syscall(
        IPO_SYSCALL_FS_CLOSE,
        syscall_builtin_fs_close);

    ipo_register_syscall(
        IPO_SYSCALL_FS_SEEK,
        syscall_builtin_fs_seek);

    ipo_register_syscall(
        IPO_SYSCALL_FS_READ,
        syscall_builtin_fs_read);

    ipo_register_syscall(
        IPO_SYSCALL_FS_WRITE,
        syscall_builtin_fs_write);

    ipo_register_syscall(
        IPO_SYSCALL_FS_DELETE,
        syscall_builtin_fs_delete);

    ipo_register_syscall(
        IPO_SYSCALL_FS_STAT,
        syscall_builtin_fs_stat);

    ipo_register_syscall(
        IPO_SYSCALL_FS_STATVFS,
        syscall_builtin_fs_statvfs);

    ipo_register_syscall(
        IPO_SYSCALL_FS_LIST,
        syscall_builtin_fs_list);

    ipo_register_syscall(
        IPO_SYSCALL_FS_RENAME,
        syscall_builtin_fs_rename);

    ipo_register_syscall(
        IPO_SYSCALL_EXEC,
        syscall_builtin_exec);

    ipo_register_syscall(
        IPO_SYSCALL_READ,
        syscall_builtin_read);

    ipo_register_syscall(
        IPO_SYSCALL_TERMINAL_INPUT,
        syscall_builtin_terminal_input);

    ipo_register_syscall(
        IPO_SYSCALL_PROCESS_YIELD,
        syscall_builtin_process_yield);

    ipo_register_syscall(
        IPO_SYSCALL_PROCESS_IS_FOREGROUND,
        syscall_builtin_process_is_foreground);

    ipo_register_syscall(
        IPO_SYSCALL_PROCESS_IS_BATCH_ACTIVE,
        syscall_builtin_process_is_batch_active);

    ipo_register_syscall(
        IPO_SYSCALL_PROCESS_SWITCH_FG,
        syscall_builtin_process_switch_fg);

    ipo_register_syscall(
        IPO_SYSCALL_GETCWD,
        syscall_builtin_getcwd);

    ipo_register_syscall(
        IPO_SYSCALL_CHDIR,
        syscall_builtin_chdir);

    ipo_register_syscall(
        IPO_SYSCALL_GET_EXIT_CODE,
        syscall_builtin_get_exit_code);

    ipo_register_syscall(
        IPO_SYSCALL_SET_RETURN_VALUE,
        syscall_builtin_set_return_value);

    ipo_register_syscall(
        IPO_SYSCALL_GET_RETURN_VALUE,
        syscall_builtin_get_return_value);

    ipo_register_syscall(
        IPO_SYSCALL_ASYNC_START,
        syscall_builtin_async_start);

    ipo_register_syscall(
        IPO_SYSCALL_ASYNC_STOP,
        syscall_builtin_async_stop);

    ipo_register_syscall(
        IPO_SYSCALL_STACK_GROW,
        syscall_builtin_stack_grow);

    ipo_register_syscall(
        IPO_SYSCALL_STACK_SHRINK,
        syscall_builtin_stack_shrink);

    ipo_register_syscall(
        IPO_SYSCALL_SBRK,
        syscall_builtin_sbrk);

    ipo_register_syscall(
        IPO_SYSCALL_TIME,
        syscall_builtin_time);

    ipo_register_syscall(
        IPO_SYSCALL_MILLIS,
        syscall_builtin_millis);

    ipo_register_syscall(
        IPO_SYSCALL_ELAPSED_MS,
        syscall_builtin_elapsed_ms);

    ipo_register_syscall(
        IPO_SYSCALL_TIMER_AFTER,
        syscall_builtin_timer_after);

    ipo_register_syscall(
        IPO_SYSCALL_NET_DNS_RESOLVE,
        syscall_builtin_net_dns_resolve);

    ipo_register_syscall(
        IPO_SYSCALL_NET_ICMP_SEND,
        syscall_builtin_net_icmp_send);

    ipo_register_syscall(
        IPO_SYSCALL_NET_ICMP_POLL,
        syscall_builtin_net_icmp_poll);

    ipo_register_syscall(IPO_SYSCALL_NET_SOCKET, syscall_builtin_net_socket);
    ipo_register_syscall(IPO_SYSCALL_NET_BIND, syscall_builtin_net_bind);
    ipo_register_syscall(IPO_SYSCALL_NET_LISTEN, syscall_builtin_net_listen);
    ipo_register_syscall(IPO_SYSCALL_NET_ACCEPT, syscall_builtin_net_accept);
    ipo_register_syscall(IPO_SYSCALL_NET_CONNECT, syscall_builtin_net_connect);
    ipo_register_syscall(IPO_SYSCALL_NET_SEND, syscall_builtin_net_send);
    ipo_register_syscall(IPO_SYSCALL_NET_RECV, syscall_builtin_net_recv);
    ipo_register_syscall(IPO_SYSCALL_NET_CLOSE, syscall_builtin_net_close);
    ipo_register_syscall(IPO_SYSCALL_NET_CONNECT_ENDPOINT,
                         syscall_builtin_net_connect_endpoint);
    ipo_register_syscall(IPO_SYSCALL_NET_POLL, syscall_builtin_net_poll);
    ipo_register_syscall(IPO_SYSCALL_NET_GET_INTERFACE,
                         syscall_builtin_net_get_interface);
    ipo_register_syscall(IPO_SYSCALL_NET_INIT, syscall_builtin_net_init);
    ipo_register_syscall(IPO_SYSCALL_NET_SET_IP, syscall_builtin_net_set_ip);
    ipo_register_syscall(IPO_SYSCALL_IPC_CREATE, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_OPEN, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_CLOSE, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_DESTROY, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_SEND, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_RECEIVE, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_PIPE_CREATE, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_PIPE_OPEN, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_PIPE_CLOSE, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_PIPE_DESTROY, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_PIPE_READ, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_PIPE_WRITE, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_NOTIFY_SEND, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_IPC_NOTIFY_WAIT, ipc_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_SHM_CREATE,
                         ipc_shared_memory_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_SHM_OPEN,
                         ipc_shared_memory_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_SHM_ATTACH,
                         ipc_shared_memory_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_SHM_DETACH,
                         ipc_shared_memory_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_SHM_DESTROY,
                         ipc_shared_memory_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_SIGACTION,
                         process_signal_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_SIGPROCMASK,
                         process_signal_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_KILL,
                         process_signal_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_SIGALTSTACK,
                         process_signal_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_THREAD_CREATE,
                         process_thread_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_THREAD_EXIT,
                         process_thread_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_THREAD_JOIN,
                         process_thread_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_THREAD_SELF,
                         process_thread_syscall_handler);
    ipo_register_syscall(IPO_SYSCALL_THREAD_SYNC,
                         process_thread_syscall_handler);

    ipo_register_syscall(
        IPO_SYSCALL_FREE,
        syscall_builtin_free);

    ipo_register_syscall(
        IPO_SYSCALL_EXIT,
        syscall_builtin_exit);

    ipo_register_syscall(
        IPO_SYSCALL_VAR_SET,
        syscall_builtin_var_set);

    ipo_register_syscall(
        IPO_SYSCALL_VAR_GET,
        syscall_builtin_var_get);

    ipo_register_syscall(
        IPO_SYSCALL_VAR_DELETE,
        syscall_builtin_var_delete);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_SET,
        syscall_builtin_keymap_set);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_GET,
        syscall_builtin_keymap_get);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_DISABLE,
        syscall_builtin_keymap_disable);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_ENABLE,
        syscall_builtin_keymap_enable);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_REMOVE,
        syscall_builtin_keymap_remove);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_TRANSLATE,
        syscall_builtin_keymap_translate);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_IS_ACTIVE,
        syscall_builtin_keymap_is_active);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_CYCLE_NEXT,
        syscall_builtin_keymap_cycle_next);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_CYCLE_PREV,
        syscall_builtin_keymap_cycle_prev);

    ipo_register_syscall(
        IPO_SYSCALL_KEYMAP_GET_NAME,
        syscall_builtin_keymap_get_name);

    ipo_register_syscall(
        IPO_SYSCALL_FONT_LOAD,
        syscall_builtin_font_load);

    ipo_register_syscall(
        IPO_SYSCALL_FONT_GET_INFO,
        syscall_builtin_font_get_info);

    ipo_register_syscall(
        IPO_SYSCALL_VGA_GLYPH,
        syscall_builtin_vga_glyph);

    ipo_register_syscall(
        IPO_SYSCALL_VGA_SET_MODE,
        syscall_builtin_vga_set_mode);

    ipo_register_syscall(
        IPO_SYSCALL_VGA_GET_MODE,
        syscall_builtin_vga_get_mode);

    ipo_register_syscall(
        IPO_SYSCALL_VGA_GET_BOUNDS,
        syscall_builtin_vga_get_bounds);

    ipo_register_syscall(
        IPO_SYSCALL_SYSTEM_INTERRUPT,
        syscall_builtin_system_interrupt);

    ipo_register_syscall(
        IPO_SYSCALL_DRIVER_REGISTER,
        syscall_builtin_driver_register);

    ipo_register_syscall(
        IPO_SYSCALL_DRIVER_UNREGISTER,
        syscall_builtin_driver_unregister);

    ipo_register_syscall(
        IPO_SYSCALL_DRIVER_LIST,
        syscall_builtin_driver_list);

    ipo_register_syscall(
        IPO_SYSCALL_DRIVER_SET_DESC,
        syscall_builtin_driver_set_desc);

    ipo_register_syscall(IPO_SYSCALL_PORT_IN8, syscall_builtin_port_io);
    ipo_register_syscall(IPO_SYSCALL_PORT_OUT8, syscall_builtin_port_io);
    ipo_register_syscall(IPO_SYSCALL_PORT_IN16, syscall_builtin_port_io);
    ipo_register_syscall(IPO_SYSCALL_PORT_OUT16, syscall_builtin_port_io);
    ipo_register_syscall(IPO_SYSCALL_PORT_IN32, syscall_builtin_port_io);
    ipo_register_syscall(IPO_SYSCALL_PORT_OUT32, syscall_builtin_port_io);

    ipo_register_syscall(
        IPO_SYSCALL_WM_CREATE_WINDOW,
        syscall_builtin_wm_create_window);

    ipo_register_syscall(
        IPO_SYSCALL_WM_DESTROY_WINDOW,
        syscall_builtin_wm_destroy_window);

    ipo_register_syscall(
        IPO_SYSCALL_WM_SESSION_START,
        syscall_builtin_wm_session_start);

    ipo_register_syscall(
        IPO_SYSCALL_WM_SESSION_STOP,
        syscall_builtin_wm_session_stop);

    ipo_register_syscall(
        IPO_SYSCALL_WM_SESSION_ACTIVE,
        syscall_builtin_wm_session_active);

    ipo_register_syscall(
        IPO_SYSCALL_WM_GET_COUNT,
        syscall_builtin_wm_get_count);

    ipo_register_syscall(
        IPO_SYSCALL_WM_GET_FOCUSED,
        syscall_builtin_wm_get_focused);

    ipo_register_syscall(
        IPO_SYSCALL_WM_GET_LIST,
        syscall_builtin_wm_get_list);

    ipo_register_syscall(
        IPO_SYSCALL_WM_SET_FOCUS,
        syscall_builtin_wm_set_focus);

    ipo_register_syscall(
        IPO_SYSCALL_WM_FOCUS_NEXT,
        syscall_builtin_wm_focus_next);

    ipo_register_syscall(
        IPO_SYSCALL_WM_FOCUS_PREV,
        syscall_builtin_wm_focus_prev);

    ipo_register_syscall(
        IPO_SYSCALL_WM_INVALIDATE,
        syscall_builtin_wm_invalidate);

    ipo_register_syscall(
        IPO_SYSCALL_WM_IS_VALID,
        syscall_builtin_wm_is_valid);

    ipo_register_syscall(
        IPO_SYSCALL_WM_TOGGLE_MAXIMIZE,
        syscall_builtin_wm_toggle_maximize);

    ipo_register_syscall(
        IPO_SYSCALL_WM_RESIZE,
        syscall_builtin_wm_resize);

    ipo_register_syscall(IPO_SYSCALL_WM_INIT, syscall_builtin_wm_init);
    ipo_register_syscall(IPO_SYSCALL_WM_INVALIDATE_ALL, syscall_builtin_wm_invalidate_all);
    ipo_register_syscall(IPO_SYSCALL_WM_SET_FULLSCREEN, syscall_builtin_wm_set_fullscreen);
    ipo_register_syscall(IPO_SYSCALL_WM_MINIMIZE, syscall_builtin_wm_minimize);
    ipo_register_syscall(IPO_SYSCALL_WM_RESTORE, syscall_builtin_wm_restore);
    ipo_register_syscall(IPO_SYSCALL_WM_MOVE, syscall_builtin_wm_move);
    ipo_register_syscall(IPO_SYSCALL_WM_SET_TITLE, syscall_builtin_wm_set_title);
    ipo_register_syscall(IPO_SYSCALL_WM_SET_SHAPE, syscall_builtin_wm_set_shape);
    ipo_register_syscall(IPO_SYSCALL_WM_DISPATCH_KEY, syscall_builtin_wm_dispatch_key);
    ipo_register_syscall(IPO_SYSCALL_WM_COMPOSE, syscall_builtin_wm_compose);
    ipo_register_syscall(IPO_SYSCALL_WM_COMPOSITOR_TICK, syscall_builtin_wm_compositor_tick);
    ipo_register_syscall(IPO_SYSCALL_WM_SESSION_RUN, syscall_builtin_wm_session_run);
    ipo_register_syscall(IPO_SYSCALL_WM_GET_COUNT_PID, syscall_builtin_wm_get_count_for_pid);
    ipo_register_syscall(IPO_SYSCALL_WM_HAS_WINDOWS_PID, syscall_builtin_wm_has_windows_for_pid);
    ipo_register_syscall(IPO_SYSCALL_WM_FOCUS_PID, syscall_builtin_wm_focus_window_for_pid);

    ipo_register_syscall(
        IPO_SYSCALL_GETPID,
        syscall_builtin_getpid);

    ipo_register_syscall(IPO_SYSCALL_KBD_GET_SCANCODE, syscall_builtin_kbd_get_scancode);
    ipo_register_syscall(IPO_SYSCALL_KBD_WAIT_SCANCODE, syscall_builtin_kbd_wait_scancode);
    ipo_register_syscall(IPO_SYSCALL_KBD_SET_APP_MODE, syscall_builtin_kbd_set_app_mode);
    ipo_register_syscall(IPO_SYSCALL_KBD_FLUSH, syscall_builtin_kbd_flush);
    ipo_register_syscall(IPO_SYSCALL_KBD_GET_MODIFIERS, syscall_builtin_kbd_get_modifiers);
    ipo_register_syscall(IPO_SYSCALL_KBD_POLL, syscall_builtin_kbd_poll);
    ipo_register_syscall(IPO_SYSCALL_KBD_GET_KEY_STRING, syscall_builtin_kbd_get_key_string);

    ipo_register_syscall(IPO_SYSCALL_MOUSE_GET_STATE, syscall_builtin_mouse_get_state);
    ipo_register_syscall(IPO_SYSCALL_MOUSE_GET_MOTION, syscall_builtin_mouse_get_motion);

    ipo_register_syscall(IPO_SYSCALL_VGA_SET_CURSOR, syscall_builtin_vga_set_cursor);
    ipo_register_syscall(IPO_SYSCALL_VGA_GET_CURSOR, syscall_builtin_vga_get_cursor);
    ipo_register_syscall(IPO_SYSCALL_VGA_SHOW_CURSOR, syscall_builtin_vga_show_cursor);
    ipo_register_syscall(IPO_SYSCALL_VGA_HIDE_CURSOR, syscall_builtin_vga_hide_cursor);
    ipo_register_syscall(IPO_SYSCALL_VGA_SET_PALETTE, syscall_builtin_vga_set_palette);
    ipo_register_syscall(IPO_SYSCALL_VGA_SET_OVERLAY_FLAGS, syscall_builtin_vga_set_overlay_flags);
    ipo_register_syscall(IPO_SYSCALL_VGA_GET_OVERLAY_FLAGS, syscall_builtin_vga_get_overlay_flags);
    ipo_register_syscall(IPO_SYSCALL_VGA_GET_TEXT_INFO, syscall_builtin_vga_get_text_info);

    ipo_register_syscall(IPO_SYSCALL_AUDIO_INIT, syscall_builtin_audio_init);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_TICK, syscall_builtin_audio_tick);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STOP_ALL, syscall_builtin_audio_stop_all);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_CREATE, syscall_builtin_audio_stream_create);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_CREATE_RING, syscall_builtin_audio_stream_create_ring);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_DESTROY, syscall_builtin_audio_stream_destroy);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_PLAY, syscall_builtin_audio_stream_play);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_PAUSE, syscall_builtin_audio_stream_pause);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_STOP, syscall_builtin_audio_stream_stop);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_SEEK, syscall_builtin_audio_stream_seek);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_SET_VOL, syscall_builtin_audio_stream_set_vol);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_PROGRESS, syscall_builtin_audio_stream_progress);
    ipo_register_syscall(IPO_SYSCALL_AUDIO_STREAM_UPDATE, syscall_builtin_audio_stream_update);

    ipo_register_syscall(IPO_SYSCALL_BINFMT_REGISTER, syscall_builtin_binfmt_register);
    ipo_register_syscall(IPO_SYSCALL_BINFMT_UNREGISTER, syscall_builtin_binfmt_unregister);
    ipo_register_syscall(IPO_SYSCALL_BINFMT_LIST, syscall_builtin_binfmt_list);

    ipo_register_syscall(IPO_SYSCALL_INPUT_POLL_KEY_EXT, syscall_builtin_input_poll_key_ext);
    ipo_register_syscall(IPO_SYSCALL_INPUT_GET_KEY_STATE, syscall_builtin_input_get_key_state);
    ipo_register_syscall(IPO_SYSCALL_INPUT_GET_MODIFIERS, syscall_builtin_input_get_modifiers);
    ipo_register_syscall(IPO_SYSCALL_SHORTCUT_REGISTER, syscall_builtin_shortcut_register);
    ipo_register_syscall(IPO_SYSCALL_SHORTCUT_UNREGISTER, syscall_builtin_shortcut_unregister);
    ipo_register_syscall(IPO_SYSCALL_SHORTCUT_POLL, syscall_builtin_shortcut_poll);
    ipo_register_syscall(IPO_SYSCALL_INPUT_DEV_REGISTER, syscall_builtin_input_dev_register);
    ipo_register_syscall(IPO_SYSCALL_INPUT_DEV_UNREGISTER, syscall_builtin_input_dev_unregister);
    ipo_register_syscall(IPO_SYSCALL_INPUT_DEV_EMIT, syscall_builtin_input_dev_emit);
    ipo_register_syscall(IPO_SYSCALL_INPUT_DEV_LIST, syscall_builtin_input_dev_list);
    ipo_register_syscall(IPO_SYSCALL_INPUT_DEV_POLL_EVENT, syscall_builtin_input_dev_poll_event);
    ipo_register_syscall(IPO_SYSCALL_INPUT_DEV_GET_STATE, syscall_builtin_input_dev_get_state);
    ipo_register_syscall(IPO_SYSCALL_MOUSE_INJECT, syscall_builtin_mouse_inject);

    ipo_register_syscall(IPO_SYSCALL_PS2_HOOK_REGISTER, syscall_builtin_ps2_hook_register);
    ipo_register_syscall(IPO_SYSCALL_PS2_HOOK_UNREGISTER, syscall_builtin_ps2_hook_unregister);
    ipo_register_syscall(IPO_SYSCALL_PS2_HOOK_POLL, syscall_builtin_ps2_hook_poll);
    ipo_register_syscall(IPO_SYSCALL_PS2_TRANSACT, syscall_builtin_ps2_transact);

    ipo_register_syscall(IPO_SYSCALL_IRQ_SUBSCRIBE, syscall_builtin_irq_subscribe);
    ipo_register_syscall(IPO_SYSCALL_IRQ_UNSUBSCRIBE, syscall_builtin_irq_unsubscribe);
    ipo_register_syscall(IPO_SYSCALL_IRQ_POLL, syscall_builtin_irq_poll);
    ipo_register_syscall(IPO_SYSCALL_IRQ_GET_COUNT, syscall_builtin_irq_get_count);
    ipo_register_syscall(IPO_SYSCALL_IRQ_REGISTER_CB, syscall_builtin_irq_register_cb);
    ipo_register_syscall(IPO_SYSCALL_IRQ_UNREGISTER_CB, syscall_builtin_irq_unregister_cb);

    ipo_register_syscall(IPO_SYSCALL_HW_MAP_MMIO, syscall_builtin_hw_map_mmio);
    ipo_register_syscall(IPO_SYSCALL_HW_UNMAP_MMIO, syscall_builtin_hw_unmap_mmio);
    ipo_register_syscall(IPO_SYSCALL_HW_ALLOC_DMA, syscall_builtin_hw_alloc_dma);
    ipo_register_syscall(IPO_SYSCALL_HW_FREE_DMA, syscall_builtin_hw_free_dma);
    ipo_register_syscall(IPO_SYSCALL_HW_VIRT_TO_PHYS, syscall_builtin_hw_virt_to_phys);

    ipo_register_syscall(IPO_SYSCALL_PCI_GET_COUNT, syscall_builtin_pci_get_count);
    ipo_register_syscall(IPO_SYSCALL_PCI_GET_DEVICE, syscall_builtin_pci_get_device);
    ipo_register_syscall(IPO_SYSCALL_PCI_CONFIG_READ, syscall_builtin_pci_config_read);
    ipo_register_syscall(IPO_SYSCALL_PCI_CONFIG_WRITE, syscall_builtin_pci_config_write);
    ipo_register_syscall(IPO_SYSCALL_PCI_ENABLE_BUS_MASTER, syscall_builtin_pci_enable_bus_master);

    ipo_register_syscall(IPO_SYSCALL_DEV_GET_COUNT, syscall_builtin_dev_get_count);
    ipo_register_syscall(IPO_SYSCALL_DEV_GET_INFO, syscall_builtin_dev_get_info);
    ipo_register_syscall(IPO_SYSCALL_DEV_REGISTER, syscall_builtin_dev_register);
    ipo_register_syscall(IPO_SYSCALL_DEV_UNREGISTER, syscall_builtin_dev_unregister);

    for (int i = 0; i < 32; i++) {
        ipo_idt_set_gate(
            (uint8_t)i,
            isr_stub_table[i],
            IPO_KERNEL_CODE_SEG,
            IPO_IDT_ENTRY_FLAGS);
    }

    /* Remap 8259 PIC to vectors 0x20..0x2F and enable IRQ0 (PIT timer) for preemptive scheduling */
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();
    outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xFE); io_wait(); /* Unmask only IRQ0 (PIT timer); keep keyboard/mouse/ATA polled */
    outb(0xA1, 0xFF); io_wait();

    ipo_idt_set_gate(0x20, (uint32_t)irq0_isr, IPO_KERNEL_CODE_SEG, 0x8Eu);
    for (int i = 0x21; i <= 0x2F; i++) {
        ipo_idt_set_gate((uint8_t)i, irq_stub_table[i - 0x21], IPO_KERNEL_CODE_SEG, 0x8Eu);
    }

    ipo_idt_set_gate(
        0x80,
        (uint32_t)syscall_isr_entry,
        IPO_KERNEL_CODE_SEG,
        IPO_IDT_ENTRY_FLAGS);

    extern void smp_ipi_isr(void);
    ipo_idt_set_gate(
        0xE0,
        (uint32_t)smp_ipi_isr,
        IPO_KERNEL_CODE_SEG,
        0x8Eu);

    uint32_t tls_base = (uint32_t)(uintptr_t)kernel_gs_tls_area;
    kernel_gdt[3] = 0x00CF93000000FFFFULL
                  | ((uint64_t)(tls_base & 0x00FFFFFFu) << 16)
                  | ((uint64_t)(tls_base & 0xFF000000u) << 32);

    ipo_idt_ptr_t gdt_ptr = {
        .limit = (uint16_t)(sizeof(kernel_gdt) - 1),
        .base = (uint32_t)(uintptr_t)kernel_gdt
    };
    __asm__ volatile(
        "lgdt %0\n\t"
        "movw $0x18, %%ax\n\t"
        "movw %%ax, %%gs"
        : : "m"(gdt_ptr) : "eax", "memory"
    );

    x86_idt_set_task_gate(ipo_idt_table, 8, X86_TSS_DOUBLE_FAULT_SELECTOR);
    x86_idt_set_task_gate(ipo_idt_table, 14, X86_TSS_PAGE_FAULT_SELECTOR);

    syscall_load_idt();
    if (!syscall_prepare_cpu_exceptions(1u)) {
        serial_printf("[syscall] Warning: Failed to prepare BSP per-CPU exceptions\n");
    }
    printf("[syscall] System call dispatch table initialized (IDT vector 0x80 registered).\n");
}

uint64_t syscall_dispatch(uint32_t num,
                          uint32_t argc,
                          uint32_t *argv)
{
    process_t *curr = process_get_current();
    if (curr && curr->demand_loading) {
        process_demand_load_step(curr);
    }
    uint64_t result;
    if (num == IPO_SYSCALL_FS_CREATE) {
        if (argc < 2u || argv == NULL ||
            (process_in_process_context() && !ipc_user_range(argv, 2u * sizeof(uint32_t)))) {
            result = (uint64_t)(int64_t)-1;
        } else {
            const char *path = (const char *)(uintptr_t)argv[0];
            if (path == NULL ||
                (process_in_process_context() && !ipc_user_range(path, 1u))) {
                result = (uint64_t)(int64_t)-1;
            } else {
                result = (uint64_t)ipo_fs_create(path, (uint8_t)argv[1]);
            }
        }
    } else {
        ipo_syscall_handler_t fn = (ipo_syscall_table && num < ipo_syscall_table_size)
                                       ? ipo_syscall_table[num]
                                       : NULL;
        if (fn == NULL) {
            return IPO_SYSCALL_ENOSYS;
        }
        result = fn(num, argc, argv);
    }
    process_signal_deliver_pending();
    if (curr && !curr->is_running && process_in_process_context()) {
        process_yield_kernel();
    } else if (process_is_batch_active() && process_in_process_context() && !putchar_is_locked()) {
        timer_tick();
    }
    return result;
}

int ipo_syscall(uint32_t num,
                uint32_t argc,
                uint32_t *argv)
{
    int ret;

    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num),
          "b"(argc),
          "c"(argv)
        : "memory"
    );

    return ret;
}