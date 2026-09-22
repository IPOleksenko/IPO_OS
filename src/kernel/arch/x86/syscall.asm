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

extern cpu_exception_handler

%macro ISR_NOERR 1
global isr_%1
isr_%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr_%1
isr_%1:
    push dword %1
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

isr_common:
    push eax
    push ecx
    push edx
    push ebx
    push ebp
    push esi
    push edi
    push esp
    call cpu_exception_handler
    add esp, 4
    pop edi
    pop esi
    pop ebp
    pop ebx
    pop edx
    pop ecx
    pop eax
    add esp, 8
    iretd

global isr_stub_table
isr_stub_table:
%assign i 0
%rep 32
    dd isr_%+i
%assign i i+1
%endrep
