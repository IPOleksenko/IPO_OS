BITS 32
global _start
extern kmain

_start:
    mov esp, 0x90000
    mov dword [0xB8000], 0x07204B    ; 'K' at top-left
    call kmain
.hang:  hlt
        jmp .hang