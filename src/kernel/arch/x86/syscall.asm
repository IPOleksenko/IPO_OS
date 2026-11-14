bits 32
section .text

global syscall_isr_entry
extern syscall_dispatch

syscall_isr_entry:
    cld
    push ds
    push es
    push fs
    push gs
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

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov ax, 0x18
    mov gs, ax

    call syscall_dispatch
    add esp, 24

    pop ebp
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop gs
    pop fs
    pop es
    pop ds
    iretd

extern cpu_exception_handler
extern cpu_page_fault_task_handler
extern cpu_double_fault_task_handler

global page_fault_task_entry
page_fault_task_entry:
    cli
    cld
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov ax, 0x18
    mov gs, ax
    mov ebp, esp        ; ebp points to hardware error_code
    and esp, 0xFFFFFFF0 ; align stack for C ABI
    sub esp, 12
    push dword [ebp]    ; pass error_code
    call cpu_page_fault_task_handler
    lea esp, [ebp + 4]  ; discard hardware error_code
    iretd
    jmp page_fault_task_entry

global double_fault_task_entry
double_fault_task_entry:
    cli
    cld
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov ax, 0x18
    mov gs, ax
    and esp, 0xFFFFFFF0
    call cpu_double_fault_task_handler
.double_fault_halt:
    hlt
    jmp .double_fault_halt

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
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

isr_common:
    cld
    push eax
    push ecx
    push edx
    push ebx
    push ebp
    push esi
    push edi
    mov ebx, esp
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov ax, 0x18
    mov gs, ax
    mov ebp, esp
    cmp esp, 0x10000
    jae .stack_ok
    mov esp, exception_stack_top
.stack_ok:
    and esp, 0xFFFFFFF0
    sub esp, 12
    push ebx
    call cpu_exception_handler
    mov esp, ebp
    pop gs
    pop fs
    pop es
    pop ds
    pop edi
    pop esi
    pop ebp
    pop ebx
    pop edx
    pop ecx
    pop eax
    add esp, 8
    iretd

extern timer_irq_handler
extern irq_common_dispatch

irq_common:
    cld
    push eax
    push ecx
    push edx
    push ebx
    push ebp
    push esi
    push edi
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov ax, 0x18
    mov gs, ax
    mov eax, [esp + 44]
    mov ebx, esp
    and esp, 0xFFFFFFF0
    sub esp, 112
    fnsave [esp]
    mov esi, esp
    test eax, eax
    jnz .dispatch_irq
    mov al, 0x20
    out 0x20, al
    call timer_irq_handler
    jmp .restore
.dispatch_irq:
    sub esp, 12
    push eax
    call irq_common_dispatch
.restore:
    mov esp, esi
    frstor [esp]
    mov esp, ebx
    pop gs
    pop fs
    pop es
    pop ds
    pop edi
    pop esi
    pop ebp
    pop ebx
    pop edx
    pop ecx
    pop eax
    add esp, 4
    iretd

global irq0_isr
irq0_isr:
    push dword 0
    jmp irq_common

%macro IRQ_STUB 1
global irq_stub_%1
irq_stub_%1:
    push dword %1
    jmp irq_common
%endmacro

%assign irq_n 1
%rep 15
    IRQ_STUB irq_n
%assign irq_n irq_n+1
%endrep

global irq_stub_table
irq_stub_table:
%assign irq_n 1
%rep 15
    dd irq_stub_%+irq_n
%assign irq_n irq_n+1
%endrep

global isr_stub_table
isr_stub_table:
%assign i 0
%rep 32
    dd isr_%+i
%assign i i+1
%endrep

section .bss
align 16
exception_stack_bottom:
    resb 8192
global exception_stack_top
exception_stack_top:
