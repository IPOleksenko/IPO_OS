BITS 32

section .text
global ipo_signal_call_on_stack

; void ipo_signal_call_on_stack(handler, signo, info, context, stack_top)
ipo_signal_call_on_stack:
    push ebp
    mov ebp, esp
    push ebx
    mov ebx, esp

    mov esp, [ebp + 24]
    and esp, 0xFFFFFFF0
    push dword [ebp + 20]
    push dword [ebp + 16]
    push dword [ebp + 12]
    call dword [ebp + 8]

    mov esp, ebx
    pop ebx
    mov esp, ebp
    pop ebp
    ret
