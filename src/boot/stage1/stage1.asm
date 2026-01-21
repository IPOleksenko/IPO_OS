%include "config.inc"
%include "stage1/config.inc"

org STAGE1_BASE
bits 16

; ============================================================
; Entry point
; ============================================================
start:
    cli                         ; Disable interrupts
    xor ax, ax
    mov ds, ax
    mov es, ax

    ; ------------------------------------------------------------
    ; Print boot messages
    ; ------------------------------------------------------------
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

    mov si, msg_s1
    call print

    ; ------------------------------------------------------------
    ; Load second stage
    ; ------------------------------------------------------------
    call load_stage2

    ; If load_stage2 returned — disk error
    mov si, new_line
    call print
    
    mov si, err
    call print
    jmp hang

; ============================================================
; Print ASCIIZ string (DS:SI)
; ============================================================
print:
    pusha
.print_loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E                ; BIOS teletype
    int 0x10
    jmp .print_loop
.done:
    popa
    ret

; ============================================================
; Load stage2 from disk
; ============================================================
load_stage2:
    mov bx, STAGE2_BASE         ; Destination offset
    mov ah, 0x02                ; BIOS read sectors
    mov al, STAGE2_SECTORS
    mov ch, 0
    mov cl, 2                   ; Sector after boot
    mov dh, 0
    mov dl, 0x80                ; First HDD
    int 0x13
    jc .error

    jmp 0x0000:STAGE2_BASE      ; Jump to stage2

.error:
    ret

; ============================================================
; Halt system
; ============================================================
hang:
    hlt
    jmp hang

; ============================================================
; Data
; ============================================================
msg         db "IPO_OS", 0
by_author   db "by IPOleksenko", 0
msg_s1      db "STAGE1_OK", 10, 13, 0
err         db "DISK ERROR", 10, 13, 0
tab         db "  ", 0
new_line    db 13, 10, 0

; ============================================================
; Boot sector padding & signature
; ============================================================
times 510 - ($ - $$) db 0
dw 0xAA55
