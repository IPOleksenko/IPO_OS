bits 16
section .text
global main
extern kmain

main:
    mov si, msg
    call print

    call kmain

    cli

print:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp print
.done:
    ret
.hang:
    hlt
    jmp .hang

msg db "ENTRY_OK", 10, 13, 0