%include "config.inc"
%include "stage2/config.inc"


org STAGE2_BASE
bits 16

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax

    mov si, msg

.print:
    lodsb
    test al, al
    jz hang
    mov ah, 0x0E
    int 0x10
    jmp .print

hang:
    hlt
    jmp hang

msg db 10, "STAGE2 OK",0
