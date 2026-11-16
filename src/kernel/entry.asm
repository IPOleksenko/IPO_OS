; Kernel entry point and mode-switch helpers.
; Drives control-register, MSR, GDT, and paging transitions from x86_tx_params_t.

BITS 32
section .text.entry
global _start
global x86_execute_hw_switch
global x86_load_tables_lm64
extern kmain
extern _kernel_bss_start
extern _kernel_bss_end

_start:
    jmp short .entry
    db "IPO_OS", 0, 0           ; boot sector checks this signature at 0x00100002

.entry:
    cli
    cld
    lgdt [entry_gdt_desc]
    jmp 0x08:.reload_cs
.reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov ss, ax
    mov ax, 0x18
    mov gs, ax
    mov esp, 0x500000
    xor ebp, ebp

    ; Clear CR0.EM/TS and set MP/NE for FPU
    mov eax, cr0
    and eax, ~((1 << 2) | (1 << 3))
    or  eax, (1 << 1) | (1 << 5)
    mov cr0, eax
    fninit

    ; Zero kernel BSS
    xor eax, eax
    mov edi, _kernel_bss_start
    mov ecx, _kernel_bss_end
    sub ecx, _kernel_bss_start
    rep stosb

    mov word [0xB8000], 0x074B  ; 'K' at top-left
    push dword 0                ; handover_paddr high32 = 0
    push dword 0                ; handover_paddr low32 = 0
    call kmain
    add esp, 8

.hang:
    cli
    hlt
    jmp .hang

; void x86_execute_hw_switch(const x86_tx_params_t *params, uint64_t *out_regs)
; x86_tx_params_t offsets:
;   +0  : uint32_t root_pt_paddr
;   +4  : uint32_t target_exec_bits
;   +8  : uint64_t cs_desc_raw
;   +16 : uint64_t cr0_set_mask
;   +24 : uint64_t cr0_clear_mask
;   +32 : uint64_t cr4_set_mask
;   +40 : uint64_t cr4_clear_mask
;   +48 : uint32_t msr_op_count
;   +52 : uint16_t cs_selector
;   +54 : uint16_t ds_selector
;   +56 : const x86_tx_msr_op_t *msr_ops (24B each: msr_addr, res, set_mask, clear_mask)
; out_regs (7 uint64_t entries):
;   [0]=CR0, [1]=CR3, [2]=CR4, [3]=IA32_EFER, [4]=FLAGS, [5]=SP, [6]=CS
x86_execute_hw_switch:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    pushfd
    cli

    mov esi, [ebp + 8]          ; esi = params
    mov edi, [ebp + 12]         ; edi = out_regs

    ; Patch target CS descriptor into GDT if provided
    mov eax, [esi + 8]
    mov edx, [esi + 12]
    test eax, eax
    jnz .have_cs_desc
    test edx, edx
    jz .skip_cs_desc
.have_cs_desc:
    movzx ecx, word [esi + 52]  ; cs_selector
    and ecx, 0xFFF8
    cmp ecx, (entry_gdt_end - entry_gdt)
    jae .skip_cs_desc
    test ecx, ecx
    jz .skip_cs_desc
    mov [entry_gdt + ecx + 0], eax
    mov [entry_gdt + ecx + 4], edx
    lgdt [entry_gdt_desc]
.skip_cs_desc:

    ; Disable paging while updating CR4, CR3, and MSRs
    mov eax, cr0
    and eax, 0x7FFFFFFF
    mov cr0, eax

    mov eax, [esi + 32]
    or  eax, [esi + 40]
    jz .skip_cr4
    mov eax, cr4
    mov edx, [esi + 40]
    not edx
    and eax, edx
    or  eax, [esi + 32]
    mov cr4, eax
.skip_cr4:

    mov eax, [esi + 0]
    test eax, eax
    jz .skip_cr3
    mov cr3, eax
.skip_cr3:

    ; Apply MSR read-modify-write ops
    mov ebx, [esi + 48]         ; msr_op_count
    mov ebp, [esi + 56]         ; msr_ops pointer
    test ebp, ebp
    jz .msr_done
.msr_loop:
    test ebx, ebx
    jz .msr_done
    mov ecx, [ebp + 0]
    rdmsr
    mov ecx, [ebp + 16]
    not ecx
    and eax, ecx
    mov ecx, [ebp + 20]
    not ecx
    and edx, ecx
    or  eax, [ebp + 8]
    or  edx, [ebp + 12]
    mov ecx, [ebp + 0]
    wrmsr
    add ebp, 24
    dec ebx
    jmp .msr_loop
.msr_done:

    ; Apply CR0 masks (re-enables paging / target mode)
    mov eax, cr0
    mov edx, [esi + 24]
    not edx
    and eax, edx
    or  eax, [esi + 16]
    mov cr0, eax

    ; Test CS descriptor L-bit (high dword bit 21) to pick 64-bit vs 32-bit state readback
    test dword [esi + 12], (1 << 21)
    jnz .verify_wide

    ; 32-bit state readback
    test edi, edi
    jz .done_switch
    mov eax, cr0
    mov [edi + 0], eax
    mov dword [edi + 4], 0
    mov eax, cr3
    mov [edi + 8], eax
    mov dword [edi + 12], 0
    mov eax, cr4
    mov [edi + 16], eax
    mov dword [edi + 20], 0
    mov dword [edi + 24], 0
    mov dword [edi + 28], 0
    pushfd
    pop eax
    mov [edi + 32], eax
    mov dword [edi + 36], 0
    mov [edi + 40], esp
    mov dword [edi + 44], 0
    xor eax, eax
    mov ax, cs
    mov [edi + 48], eax
    mov dword [edi + 52], 0
    jmp .done_switch

.verify_wide:
    mov ax, [esi + 52]
    mov [.tx_far_ptr + 4], ax
    jmp far dword [.tx_far_ptr]

align 4
.tx_far_ptr:
    dd .in_wide
    dw 0x38

[BITS 64]
.in_wide:
    mov edi, edi
    test rdi, rdi
    jz .exit_wide
    mov rax, cr0
    mov [rdi + 0], rax
    mov rax, cr3
    mov [rdi + 8], rax
    mov rax, cr4
    mov [rdi + 16], rax
    mov ecx, 0xC0000080
    rdmsr
    shl rdx, 32
    or  rax, rdx
    mov [rdi + 24], rax
    pushfq
    pop rax
    mov [rdi + 32], rax
    mov [rdi + 40], rsp
    xor eax, eax
    mov ax, cs
    mov [rdi + 48], rax
.exit_wide:
    jmp far dword [rel .compat_ptr]
align 4
.compat_ptr:
    dd .done_switch
    dw 0x08

[BITS 32]
.done_switch:
    popfd
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; void x86_load_tables_lm64(const void *gdt_ptr10, const void *idt_ptr10, uint32_t load_tr)
; Switch to CS=0x38 to load 10-byte GDTR/IDTR and optionally TR (0x20).
x86_load_tables_lm64:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi

    mov edi, [ebp + 8]
    mov esi, [ebp + 12]
    mov edx, [ebp + 16]

    jmp 0x38:.lt_in64
[BITS 64]
.lt_in64:
    mov edi, edi
    mov esi, esi
    test rdi, rdi
    jz .lt_no_gdt
    lgdt [rdi]
.lt_no_gdt:
    test rsi, rsi
    jz .lt_no_idt
    lidt [rsi]
.lt_no_idt:
    test edx, edx
    jz .lt_back32
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov ss, ax
    mov ax, 0x18
    mov gs, ax
    mov ax, 0x20
    ltr ax
.lt_back32:
    jmp far dword [rel .lt_ptr32]
align 4
.lt_ptr32:
    dd .lt_done32
    dw 0x08
[BITS 32]
.lt_done32:
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

align 16
entry_gdt:
    dq 0x0000000000000000       ; 0x00: Null
    dq 0x00CF9B000000FFFF       ; 0x08: 32-bit / Compatibility Kernel Code (L=0, D=1)
    dq 0x00CF93000000FFFF       ; 0x10: Kernel Data
    dq 0x00CF93000000FFFF       ; 0x18: Kernel GS / TLS Data
    dq 0x0000000000000000       ; 0x20: TSS slot 0
    dq 0x0000000000000000       ; 0x28: TSS slot 1
    dq 0x0000000000000000       ; 0x30: TSS slot 2
    dq 0x00AF9B000000FFFF       ; 0x38: Wide Long Mode Kernel Code (L=1, D=0)
entry_gdt_end:

entry_gdt_desc:
    dw (entry_gdt_end - entry_gdt - 1)
    dd entry_gdt
    dd 0
