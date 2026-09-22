bits 32
section .text

global setjmp
setjmp:
    mov eax, [esp + 4]
    mov [eax + 0], ebx
    mov [eax + 4], esi
    mov [eax + 8], edi
    mov [eax + 12], ebp
    lea edx, [esp + 4]
    mov [eax + 16], edx
    mov edx, [esp]
    mov [eax + 20], edx
    xor eax, eax
    ret

global longjmp
longjmp:
    mov edx, [esp + 4]
    mov eax, [esp + 8]
    test eax, eax
    jnz .ok
    inc eax
.ok:
    mov ebx, [edx + 0]
    mov esi, [edx + 4]
    mov edi, [edx + 8]
    mov ebp, [edx + 12]
    mov esp, [edx + 16]
    jmp dword [edx + 20]
