BITS 32
global _start
extern kmain
extern _kernel_bss_start
extern _kernel_bss_end

_start:
    mov esp, 0x500000
    cld
    xor eax, eax
    mov edi, _kernel_bss_start
    mov ecx, _kernel_bss_end
    sub ecx, _kernel_bss_start
    rep stosb
    mov dword [0xB8000], 0x07204B    ; 'K' at top-left
    call kmain
.hang:
    hlt
    jmp .hang