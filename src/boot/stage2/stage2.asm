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
    mov al, 1                  ; read 1 sector
    mov ch, 0
    mov cl, 3                  ; sector = 3
    mov dh, 0
    mov dl, 0x80
    int 0x13
    jc .read_failed

    ; print "LOADED" to COM1
    mov si, msg_loaded
    call print_serial

    jmp KERNEL_LOAD_ADDR


.read_failed:
    ; print "E?" — '?' = AH error code
    mov al, 'E'
    call serial_write
    mov al, ah
    call serial_write
    jmp hang


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
