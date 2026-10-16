%include "config.inc"

BITS 16
ORG 0x7C00

start:
    ; Immediately setup segments and stack
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [BOOT_DRIVE], dl

    mov si, banner
    call print

    ; check INT13h extensions
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc disk_error
    cmp bx, 0xAA55
    jne disk_error

    ; DAP-based LBA read: read KERNEL_SECTORS from LBA 1 to 0x10000 in safe chunks (max 64 sectors per read)
    mov cx, KERNEL_SECTORS
    mov dword [dap_lba], 1
    mov word [dap_seg], 0x1000

.read_loop:
    test cx, cx
    jz .read_done

    mov ax, cx
    cmp ax, 64
    jbe .do_read
    mov ax, 64

.do_read:
    mov [dap_sectors], ax
    mov si, dap
    mov dl, [BOOT_DRIVE]
    mov ah, 0x42
    int 0x13
    jc disk_error

    mov ax, [dap_sectors]
    sub cx, ax
    movzx eax, ax
    add dword [dap_lba], eax

    shl ax, 5
    add word [dap_seg], ax
    jmp .read_loop

.read_done:
    ; enable A20
    in al, 0x92
    or al, 00000010b
    out 0x92, al

    ; detect RAM size using BIOS E820 and store total bytes at 0x8FF0
    call detect_memory

    cli
    jmp pm_setup

; ---------------------------------------------------------------
; Detect available RAM using BIOS E820.
; Writes total RAM size in bytes to memory location 0x8FF0.
; ---------------------------------------------------------------
detect_memory:
    xor ebx, ebx
    mov dword [0x8FE0], 0x45383230  ; 'E820' magic
    mov dword [0x8FE4], 0           ; E820 entry count
    mov dword [0x8FE8], 0x6000      ; E820 table pointer (phys 0x6000)
    mov dword [0x8FF0], 0
    mov di, 0x0600
    mov es, di
    xor di, di

.detect_loop:
    mov eax, 0xE820
    mov ecx, 20
    mov edx, 0x534D4150
    int 0x15
    jc .done
    cmp eax, 0x534D4150
    jne .done

    inc dword [0x8FE4]              ; entry_count++

    cmp dword [es:di+16], 1
    jne .next_entry

    mov eax, [es:di+8]
    mov edx, [es:di+12]
    add dword [0x8FF0], eax

.next_entry:
    test ebx, ebx
    jz .done
    add di, 20
    jmp .detect_loop

.done:
    cmp dword [0x8FF0], 0
    jne .exit

    ; fallback: E801 for common BIOSes
    mov ax, 0xE801
    int 0x15
    jc .exit

    xor edx, edx
    mov dx, bx
    shl edx, 6
    add dx, ax
    shl edx, 10
    mov dword [0x8FF0], edx

.exit:
    ret

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
    ; hide BIOS text cursor
    mov ah, 0x01
    mov ch, 0x20
    mov cl, 0x00
    int 0x10

    lgdt [gdt_desc]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp 0x08:pmode_entry

[BITS 32]
pmode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, 0x500000
    jmp 0x00010000

[BITS 16]
disk_error:
    mov ax, 0x0E45
    int 0x10
.hang:
    hlt
    jmp .hang

print:
.print_loop:
    lodsb
    test al, al
    jz .print_done
    mov ah, 0x0E
    int 0x10
    jmp .print_loop
.print_done:
    ret

BOOT_DRIVE: db 0

; DAP: read kernel into 0x10000
dap:
    db 16
    db 0
dap_sectors:
    dw 0
    dw 0x0000               ; offset
dap_seg:
    dw 0x1000               ; segment 0x1000:0000 = phys 0x10000
dap_lba:
    dd 1                    ; LBA start
    dd 0

banner: db "IPO_OS by IPOleksenko", 13, 10, 0

times 446-($-$$) db 0

; ============================================================
; Standard MBR Partition Table (64 bytes: 4 x 16-byte entries)
; ============================================================
; Partition 1: Active, Type 0x83 (Linux / IPO_FS), Start LBA 2048
db 0x80                 ; Bootable / Active flag
db 0x00, 0x20, 0x21     ; CHS start
db 0x83                 ; Partition Type (0x83 = Linux / IPO_FS)
db 0xFE, 0xFF, 0xFF     ; CHS end
dd 2048                 ; LBA start (1 MB offset)
dd 260096               ; Sector count (127 MB default)

; Partitions 2, 3, 4 (empty)
times 48 db 0

dw 0xAA55