bits 32
section .text

global smp_trampoline_start
global smp_trampoline_end
global smp_ipi_isr
extern smp_ipi_handler

smp_trampoline_start:
bits 16
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Load GDT descriptor at 0x8078 using CS segment override
    db 0x2e, 0x0f, 0x01, 0x16
    dw 0x0078

    ; Enable protected mode (CR0.PE = 1)
    mov eax, cr0
    or al, 1
    mov cr0, eax

    ; Far jump to 32-bit protected mode at 0x8020
    db 0x66, 0xea
    dd 0x00008020
    dw 0x08

bits 32
    ; Pad to exactly 0x20
    times 32-($-smp_trampoline_start) db 0x90

    ; 32-bit entrypoint at 0x8020
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Read stack pointer from 0x80F0
    mov esp, [0x000080F0]
    ; System V ABI 16-byte stack alignment: passing 1 4-byte argument requires 12 bytes padding
    sub esp, 12
    ; Read target cpu_id from 0x80F4
    mov ecx, [0x000080F4]
    push ecx
    ; Read target function pointer from 0x80F8
    mov eax, [0x000080F8]
    call eax

.halt:
    cli
    hlt
    jmp .halt

; Pad to 0x60 (96 bytes)
times 96-($-smp_trampoline_start) db 0

; 0x8060: GDT (24 bytes, 8-byte aligned)
smp_gdt:
    dd 0, 0
    dd 0x0000FFFF, 0x00CF9A00   ; 0x08: 32-bit code (base=0, limit=4GB)
    dd 0x0000FFFF, 0x00CF9200   ; 0x10: 32-bit data (base=0, limit=4GB)

; 0x8078: GDT descriptor (6 bytes)
smp_gdt_desc:
    dw 23
    dd 0x00008060

smp_trampoline_end:

; SMP IPI Interrupt Service Routine (vector 0xE0)
smp_ipi_isr:
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 16-byte stack alignment for C handler
    sub esp, 4
    call smp_ipi_handler
    add esp, 4

    ; Acknowledge interrupt to Local APIC (EOI register at 0xFEE000B0)
    mov dword [0xFEE000B0], 0

    pop gs
    pop fs
    pop es
    pop ds
    popa
    iretd

