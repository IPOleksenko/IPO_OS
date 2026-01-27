%include "config.inc"

BITS 16
ORG 0x7C00

start:
    mov si, msg
    call print
    
    mov si, tab
    call print
    
    mov si, tab
    call print

    mov si, by_author
    call print

    mov si, new_line
    call print
    
    mov si, new_line
    call print

    ; setup segments + stack
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov [BOOT_DRIVE], dl

    ; progress: A
    mov ax, 0x0E41
    int 0x10

    ; check INT13h extensions
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc disk_error
    cmp bx, 0xAA55
    jne disk_error

    ; DAP-based LBA read: read KERNEL_SECTORS from LBA 1 to 0x10000
    mov si, dap
    mov dl, [BOOT_DRIVE]
    mov ah, 0x42
    int 0x13
    jc disk_error

    ; progress: R
    mov ax, 0x0E52
    int 0x10

    ; enable A20
    in al, 0x92
    or al, 00000010b
    out 0x92, al

    cli
    jmp short pm_setup       ; >>> skip over GDT data <<<

align 8
gdt_start:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF   ; code, base=0, limit=4GB
    dq 0x00CF92000000FFFF   ; data, base=0, limit=4GB
gdt_end:

gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start

pm_setup:
    ; progress: G
    mov ax, 0x0E47
    int 0x10

    ; --- hide BIOS text cursor while still in real mode ---
    mov ah, 0x01
    mov ch, 0x20     ; bit 5 set to hide
    mov cl, 0x00
    int 0x10

    ; NOTE: BIOS ints no longer work fully after the code below.
    lgdt [gdt_desc]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    ; NOTE: after this far jump, BIOS ints no longer work at all.
    jmp 0x08:pmode_entry

[BITS 32]
pmode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, 0x90000

    ; Jump to kernel at linear 0x00010000
    jmp 0x00010000

[BITS 16]
disk_error:
    mov ax, 0x0E45          ; 'E'
    int 0x10
.hang:  hlt
        jmp .hang

BOOT_DRIVE: db 0

; DAP: read KERNEL_SECTORS from LBA=1 into 0x10000
dap:
    db 16
    db 0
    dw KERNEL_SECTORS
    dw 0x0000               ; offset
    dw 0x1000               ; segment 0x1000:0000 = phys 0x10000
    dd 1                    ; LBA start
    dd 0

; ============================================================
; Print
; ============================================================
print:
    pusha
.print_loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp .print_loop
.done:
    popa
    ret

msg         db "IPO_OS", 0
by_author   db "by IPOleksenko", 0
tab         db "  ", 0
new_line    db 13, 10, 0

times 510-($-$$) db 0
dw 0xAA55