bits 32

global _start
global entry32_setup
extern kmain

SECTION .text

_start:
    call entry32_setup
    call kmain
    cli
.hang:
    hlt
    jmp .hang

entry32_setup:
    ; Set up flat data segments (selector 0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Setup stack
    mov esp, 0x90000

    ret

halt:
    cli
