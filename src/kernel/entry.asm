bits 64
section .text
global _start

_start:
    mov rdi, 0xB8000
    mov rsi, msg

.print:
    lodsb
    test al, al
    jz halt
    mov ah, 0x0F
    mov [rdi], ax
    add rdi, 2
    jmp .print

halt:
    hlt
    jmp halt

section .rodata
msg db "Hello from ELF64 kernel!",0
