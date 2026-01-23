%include "config.inc"
%include "stage2/config.inc"

org STAGE2_BASE
bits 16

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax

    mov si, msg_s2
    call print

    call load_kernel
    jmp hang


; ====================
; print(SI)
; ====================
print:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp print
.done:
    ret


; ====================
; serial_write(AL)
; ====================
serial_write:
    mov dx, 0x3F8
    out dx, al
    ret


; ====================
; load_kernel
; ====================
load_kernel:
    mov bx, KERNEL_LOAD_ADDR
    mov ah, 0x02
    mov al, 1                  ; read 1 sector (kernel)
    mov ch, 0
    mov cl, 3                  ; sector = 3
    mov dh, 0
    mov dl, 0x80
    int 0x13
    jc read_failed

    ; print "LOADED" to COM1
    mov si, msg_loaded
    call print_serial

    ; Enable A20 via port 0x92 (simple method)
    in al, 0x92
    or al, 0x02
    out 0x92, al

    ; Setup a minimal GDT (null, code, data)
    cli
    lgdt [gdt_descriptor]

    ; Enable protected mode
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Far jump to flush prefetch and enter protected mode
    jmp 0x08:pm_after_ljmp

pm_after_ljmp:
    ; Load data segment selectors
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    ; Load stack selector and pointer
    mov ss, ax
    ; Force 32-bit operand-size for the following immediate write to ESP
    db 0x66
    mov esp, 0x90000

    ; Jump to kernel in protected mode (code selector 0x08)
    ; Use operand-size prefix so far-jmp contains a 32-bit offset
    db 0x66
    jmp 0x08:KERNEL_LOAD_ADDR


read_failed:
    ; print "E?" — '?' = AH error code
    mov al, 'E'
    call serial_write
    mov al, ah
    call serial_write
    jmp hang

; GDT: null descriptor, code (0x9A), data (0x92)
; descriptor layout: dw limit_low; dw base_low; db base_mid; db access; db granularity; db base_high
gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

gdt_start:
    ; NULL descriptor
    dw 0
    dw 0
    db 0
    db 0
    db 0
    db 0

    ; Code: limit=0xFFFF, base=0x0, access=0x9A, granularity=0xCF
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    db 0xCF
    db 0x00

    ; Data: limit=0xFFFF, base=0x0, access=0x92, granularity=0xCF
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92
    db 0xCF
    db 0x00

gdt_end:
    nop


; ====================
; print_serial(SI)
; ====================
print_serial:
    lodsb
    test al, al
    jz .done
    call serial_write
    jmp print_serial
.done:
    ret


hang:
    hlt
    jmp hang


msg_s2      db "STAGE2_OK", 10, 13, 0
msg_loaded  db "LOADED", 13, 0


times 510 - ($ - $$) db 0
dw 0xAA55
