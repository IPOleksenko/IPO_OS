bits 32
section .text

global syscall_isr_entry
extern syscall_dispatch

syscall_isr_entry:
    push ebx
    push ecx
    push edx
    push esi
    push edi
    push ebp

    push dword edi
    push dword esi
    push dword edx
    push dword ecx
    push dword ebx
    push dword eax
    call syscall_dispatch
    add esp, 24

    pop ebp
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    iretd
