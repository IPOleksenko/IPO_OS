; AP startup trampoline (16->32 and 16->32->64 with 4-level or LA57 paging).
; Copied at runtime to physical 0x8000. Mailbox lives at 0x8180..0x81AF:
;   0x8180 (8 B): stack_top
;   0x8188 (8 B): ap_main entry pointer
;   0x8190 (8 B): target CR3 (for 64-bit Long Mode)
;   0x8198 (4 B): cpu_id
;   0x819C (4 B): done_flag
;   0x81A0 (4 B): target_bits (32 or 64)
;   0x81A4 (4 B): use_la57 (0 or 1)

bits 32
section .text

global smp_trampoline_start
global smp_trampoline_end
global smp_ipi_isr
global lm64_smp_ipi_isr
extern smp_ipi_handler
extern smp_lapic_base

smp_trampoline_start:
bits 16
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Load trampoline GDT using CS-relative offset (CS = 0x0800)
    db 0x2E, 0x0F, 0x01, 0x16
    dw (smp_gdt_desc - smp_trampoline_start)

    mov eax, cr0
    or al, 1
    mov cr0, eax

    ; Far jump to 32-bit entry at 0x8000 + offset
    db 0x66, 0xEA
    dd 0x8000 + (ap_pm32 - smp_trampoline_start)
    dw 0x08

bits 32
ap_pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    cmp dword [0x000081A0], 64
    jb ap_call_main32

ap_enter_lm64:
    ; Enable PAE + OSFXSR + OSXMMEXCPT (+ LA57 if active on BSP)
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)
    cmp dword [0x000081A4], 0
    je .no_la57
    or eax, (1 << 12)
.no_la57:
    mov cr4, eax

    mov eax, [0x00008190]
    mov cr3, eax

    ; Set IA32_EFER.LME
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)
    wrmsr

    ; Set CR0 PG|WP|MP|NE|PE and clear EM|TS to activate Long Mode
    mov eax, cr0
    and eax, ~((1 << 2) | (1 << 3))
    or eax, (1 << 31) | (1 << 16) | (1 << 5) | (1 << 1) | (1 << 0)
    mov cr0, eax

ap_call_main32:
    fninit
    mov esp, [0x00008180]
    and esp, 0xFFFFFFF0
    sub esp, 12
    push dword [0x00008198]
    mov eax, [0x00008188]
    call eax

ap_halt:
    cli
    hlt
    jmp ap_halt

align 8
smp_gdt:
    dq 0x0000000000000000   ; 0x00: Null
    dq 0x00CF9B000000FFFF   ; 0x08: 32-bit / Compatibility Code (L=0, D=1)
    dq 0x00CF93000000FFFF   ; 0x10: Data
    dq 0x00CF93000000FFFF   ; 0x18: GS / TLS Data
    dq 0x0000000000000000   ; 0x20: Reserved TSS
    dq 0x0000000000000000   ; 0x28: Reserved TSS
    dq 0x0000000000000000   ; 0x30: Reserved TSS
    dq 0x00AF9B000000FFFF   ; 0x38: 64-bit Native Code (L=1, D=0)

smp_gdt_desc:
    dw (smp_gdt_desc - smp_gdt - 1)
    dd 0x8000 + (smp_gdt - smp_trampoline_start)

smp_trampoline_end:

; SMP IPI handler (vector 0xE0)
bits 32
smp_ipi_isr:
    cld
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov ax, 0x18
    mov gs, ax

    mov ebp, esp
    and esp, 0xFFFFFFF0
    call smp_ipi_handler
    mov esp, ebp

    mov eax, [smp_lapic_base]
    mov dword [eax + 0xB0], 0

    pop gs
    pop fs
    pop es
    pop ds
    popa
    iretd

bits 64
lm64_smp_ipi_isr:
    cld
    jmp far dword [rel .ipi_ptr32]
align 4
.ipi_ptr32:
    dd .ipi_in32
    dw 0x08
bits 32
.ipi_in32:
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov ax, 0x18
    mov gs, ax

    mov ebp, esp
    and esp, 0xFFFFFFF0
    call smp_ipi_handler
    mov esp, ebp

    mov eax, [smp_lapic_base]
    mov dword [eax + 0xB0], 0

    pop gs
    pop fs
    pop es
    pop ds
    popa
    jmp 0x38:.ipi_out64
bits 64
.ipi_out64:
    iretq
