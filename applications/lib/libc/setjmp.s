.intel_syntax noprefix
.text

.global setjmp
.type setjmp, @function
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
.size setjmp, .-setjmp

.global longjmp
.type longjmp, @function
longjmp:
    mov edx, [esp + 4]
    mov eax, [esp + 8]
    test eax, eax
    jnz 1f
    inc eax
1:
    mov ebx, [edx + 0]
    mov esi, [edx + 4]
    mov edi, [edx + 8]
    mov ebp, [edx + 12]
    mov esp, [edx + 16]
    jmp dword ptr [edx + 20]
.size longjmp, .-longjmp

