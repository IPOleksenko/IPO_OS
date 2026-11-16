bits 32
section .text

global syscall_isr_entry
global lm64_syscall_isr_entry
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
extern cpu_page_fault_lm64_handler
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
    push dword [ebp]
    call cpu_page_fault_task_handler
    lea esp, [ebp + 4]  ; drop hardware error_code before iretd
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

; 64-bit Long Mode IDT entry shims (gates enter on CS = 0x38)

[BITS 64]
lm64_syscall_isr_entry:
    cld
    jmp far dword [rel .sys_ptr32]
align 4
.sys_ptr32:
    dd .sys_in32
    dw 0x08
[BITS 32]
.sys_in32:
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
    jmp 0x38:.sys_out64
[BITS 64]
.sys_out64:
    iretq

global lm64_page_fault_entry
lm64_page_fault_entry:
    cli
    cld
    jmp far dword [rel .pf_ptr32]
align 4
.pf_ptr32:
    dd .pf_in32
    dw 0x08
[BITS 32]
.pf_in32:
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
    lea ebx, [esp + 44]         ; [error_code (8B), iret_frame (40B)]
    mov eax, [ebx]              ; error_code low32
    lea ecx, [ebx + 8]          ; &iret_frame[0]
    mov ebp, esp
    and esp, 0xFFFFFFF0
    sub esp, 8
    push ecx
    push eax
    call cpu_page_fault_lm64_handler
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
    jmp 0x38:.pf_out64
[BITS 64]
.pf_out64:
    add rsp, 8
    iretq

global lm64_double_fault_entry
lm64_double_fault_entry:
    cli
    cld
    jmp far dword [rel .df_ptr32]
align 4
.df_ptr32:
    dd double_fault_task_entry
    dw 0x08

%macro LM64_ISR_NOERR 1
[BITS 64]
global lm64_isr_%1
lm64_isr_%1:
    push qword 0
    push qword %1
    jmp lm64_isr_common
%endmacro

%macro LM64_ISR_ERR 1
[BITS 64]
global lm64_isr_%1
lm64_isr_%1:
    push qword %1
    jmp lm64_isr_common
%endmacro

LM64_ISR_NOERR 0
LM64_ISR_NOERR 1
LM64_ISR_NOERR 2
LM64_ISR_NOERR 3
LM64_ISR_NOERR 4
LM64_ISR_NOERR 5
LM64_ISR_NOERR 6
LM64_ISR_NOERR 7
LM64_ISR_ERR   8
LM64_ISR_NOERR 9
LM64_ISR_ERR   10
LM64_ISR_ERR   11
LM64_ISR_ERR   12
LM64_ISR_ERR   13
LM64_ISR_ERR   14
LM64_ISR_NOERR 15
LM64_ISR_NOERR 16
LM64_ISR_ERR   17
LM64_ISR_NOERR 18
LM64_ISR_NOERR 19
LM64_ISR_NOERR 20
LM64_ISR_ERR   21
LM64_ISR_NOERR 22
LM64_ISR_NOERR 23
LM64_ISR_NOERR 24
LM64_ISR_NOERR 25
LM64_ISR_NOERR 26
LM64_ISR_NOERR 27
LM64_ISR_NOERR 28
LM64_ISR_ERR   29
LM64_ISR_ERR   30
LM64_ISR_NOERR 31

[BITS 64]
lm64_isr_common:
    cld
    jmp far dword [rel .isr_ptr32]
align 4
.isr_ptr32:
    dd .isr_in32
    dw 0x08
[BITS 32]
.isr_in32:
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
    mov ebp, esp
    cmp esp, 0x10000
    jae .lm_stack_ok
    mov esp, exception_stack_top
.lm_stack_ok:
    and esp, 0xFFFFFFF0
    sub esp, 48
    mov eax, [ebp + 60]         ; RIP low32
    mov [esp + 36], eax         ; frame[9] = eip
    mov eax, [ebp + 52]         ; error_code low32
    mov [esp + 32], eax         ; frame[8] = err_code
    mov eax, [ebp + 44]         ; vector low32
    mov [esp + 28], eax         ; frame[7] = vector
    mov eax, esp
    sub esp, 12
    push eax
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
    jmp 0x38:.isr_out64
[BITS 64]
.isr_out64:
    add rsp, 16
    iretq

[BITS 64]
lm64_irq_common:
    cld
    jmp far dword [rel .irq_ptr32]
align 4
.irq_ptr32:
    dd .irq_in32
    dw 0x08
[BITS 32]
.irq_in32:
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
    jnz .lm_dispatch_irq
    mov al, 0x20
    out 0x20, al
    call timer_irq_handler
    jmp .lm_restore
.lm_dispatch_irq:
    sub esp, 12
    push eax
    call irq_common_dispatch
.lm_restore:
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
    jmp 0x38:.irq_out64
[BITS 64]
.irq_out64:
    add rsp, 8
    iretq

[BITS 64]
global lm64_irq0_isr
lm64_irq0_isr:
    push qword 0
    jmp lm64_irq_common

%macro LM64_IRQ_STUB 1
[BITS 64]
global lm64_irq_stub_%1
lm64_irq_stub_%1:
    push qword %1
    jmp lm64_irq_common
%endmacro

%assign irq_n 1
%rep 15
    LM64_IRQ_STUB irq_n
%assign irq_n irq_n+1
%endrep

[BITS 32]
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

global lm64_irq_stub_table
lm64_irq_stub_table:
%assign irq_n 1
%rep 15
    dd lm64_irq_stub_%+irq_n
%assign irq_n irq_n+1
%endrep

global lm64_isr_stub_table
lm64_isr_stub_table:
%assign i 0
%rep 32
    dd lm64_isr_%+i
%assign i i+1
%endrep

section .bss
align 16
exception_stack_bottom:
    resb 8192
global exception_stack_top
exception_stack_top:
