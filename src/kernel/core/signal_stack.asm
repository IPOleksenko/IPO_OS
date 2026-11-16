%ifidn __OUTPUT_FORMAT__, elf64
BITS 64

section .text
global ipo_signal_call_on_stack

; void ipo_signal_call_on_stack(handler, signo, info, context, stack_top)
; 64-bit ABI: rdi=handler, rsi=signo, rdx=info, rcx=context, r8=stack_top
ipo_signal_call_on_stack:
    push rbp
    mov rbp, rsp
    push rbx
    mov rbx, rsp

    mov rsp, r8
    and rsp, -16
    mov r9, rdi
    mov rdi, rsi
    mov rsi, rdx
    mov rdx, rcx
    call r9

    mov rsp, rbx
    pop rbx
    mov rsp, rbp
    pop rbp
    ret

%else
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
%endif
