BITS 32
section .text.entry
global _start
extern kmain
extern _kernel_bss_start
extern _kernel_bss_end

_start:
    jmp short .entry
    db "IPO_OS", 0, 0           ; Kernel magic signature at 0x00100002

.entry:
    mov ax, 0x10
    mov fs, ax
    mov gs, ax
    mov esp, 0x500000
    cld
    xor eax, eax
    mov edi, _kernel_bss_start
    mov ecx, _kernel_bss_end
    sub ecx, _kernel_bss_start
    rep stosb
    mov word [0xB8000], 0x074B  ; 'K' at top-left
    call kmain
.hang:
    hlt
    jmp .hang