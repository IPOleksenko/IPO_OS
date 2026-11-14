%include "config.inc"

BITS 16
ORG 0x7C00

start_entry:
    jmp short start
banner: db "IPO_OS by IPOleksenko", 13, 10, 0

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
.print_loop:
    lodsb
    test al, al
    jz .print_done
    mov ah, 0x0E
    int 0x10
    jmp .print_loop
.print_done:

    ; Check INT 13h extensions (DL still holds BOOT_DRIVE)
    mov ah, 0x41
    mov bx, 0x55AA
    int 0x13
    jc disk_error
    cmp bx, 0xAA55
    jne disk_error

    ; Enable A20 early so high-memory copies above 1 MiB work cleanly
    in al, 0x92
    or al, 00000010b
    out 0x92, al

    ; DAP-based LBA read: read KERNEL_SECTORS from LBA 1 to bounce buffer 0x10000
    ; in 64-sector (32 KiB) chunks, then copy in PM to 0x100000+ (safe above VGA/ROM)
    mov cx, KERNEL_SECTORS
    mov edi, 0x00100000

.read_loop:
    jcxz .read_done

    mov ax, 64
    cmp cx, ax
    jae .do_read
    mov ax, cx

.do_read:
    mov [dap_sectors], ax
    push cx
    push edi
    mov si, dap
    mov dl, [BOOT_DRIVE]
    mov ah, 0x42
    int 0x13
    jc disk_error

    ; Copy chunk from 0x10000 to EDI while CR0.PE=1 using flat 4 GiB segments
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    mov bx, 0x10
    mov ds, bx
    mov es, bx

    pop edi
    mov esi, 0x00010000
    movzx ecx, word [dap_sectors]
    add [dap_lba], ecx
    shl ecx, 7                  ; dwords = sectors * (512 / 4)
    cld
    a32 rep movsd

    mov eax, cr0
    and al, 0xFE
    mov cr0, eax
    xor bx, bx
    mov ds, bx
    mov es, bx
    sti

    pop cx
    sub cx, [dap_sectors]
    jmp .read_loop

.read_done:
    ; Collect BIOS E820 entries at physical 0x6000 and store metadata at 0x8FE0..0x8FF0.
    ; Also stores a 32-bit usable-entry byte sum at 0x8FF0 as fallback.
    xor ebx, ebx
    mov di, 0x8FE0
    mov eax, 0x45383230             ; 'E820' magic
    stosd                           ; [0x8FE0]
    xor eax, eax
    stosd                           ; [0x8FE4] = 0 (count)
    mov ah, 0x60                    ; eax = 0x6000
    stosd                           ; [0x8FE8] = 0x6000 (ptr low)
    xor ax, ax                      ; eax = 0
    stosd                           ; [0x8FEC] = 0 (ptr high)
    stosd                           ; [0x8FF0] = 0 (low sum)
    push 0x0600
    pop es
    xor di, di

.detect_loop:
    mov eax, 0xE820
    push dword 20
    pop ecx
    mov edx, 0x534D4150
    int 0x15
    jc .detect_done
    cmp eax, 0x534D4150
    jne .detect_done

    inc dword [0x8FE4]              ; entry_count++

    cmp dword [es:di+16], 1
    jne .next_entry

    mov eax, [es:di+8]
    add dword [0x8FF0], eax

.next_entry:
    test ebx, ebx
    jz .detect_done
    add di, 20
    jmp .detect_loop

.detect_done:
    cmp dword [0x8FF0], 0
    jne .detect_exit

    ; Fallback: INT 15h AX=E801h (memory above 1 MiB)
    mov ax, 0xE801
    int 0x15
    jc .detect_exit
    test ax, ax
    jnz .have_e801
    mov ax, cx
    mov bx, dx
.have_e801:
    movzx edx, bx
    shl edx, 6
    movzx eax, ax
    add edx, eax
    shl edx, 10
    mov dword [0x8FF0], edx

.detect_exit:
    ; Hide BIOS text cursor while interrupts are enabled
    mov ah, 0x01
    mov cx, 0x2000
    int 0x10

    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or  al, 1
    mov cr0, eax
    jmp 0x08:pmode_entry

gdt_start:
; Null descriptor holds the 6-byte GDTR (architecturally unused by CPU)
gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start
    dw 0
    dq 0x00CF9A000000FFFF   ; code, base=0, limit=4GB
    dq 0x00CF92000000FFFF   ; data, base=0, limit=4GB
gdt_end:

[BITS 32]
pmode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    ; Validate 6-byte "IPO_OS" magic at kernel entry (0x00100002)
    mov esi, 0x00100002
    mov edi, banner
    cld
    cmpsd
    jne halt
    cmpsw
    jne halt
    jmp 0x00100000

[BITS 16]
disk_error:
    mov ax, 0x0E45
    int 0x10
halt:
    hlt
    jmp halt

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

times 446-($-$$) db 0

; MBR partition table (4 x 16-byte entries)
; Partition 1: Active, Type 0x83 (Linux / IPO_FS), Start LBA 2048
db 0x80                 ; Bootable / Active flag
db 0x00, 0x20, 0x21     ; CHS start
db 0x83                 ; Generic Linux filesystem partition type (not an IPO_FS identifier)
db 0xFE, 0xFF, 0xFF     ; CHS end
dd MBR_PARTITION_START_LBA ; LBA start (1 MiB offset)
dd MBR_PARTITION_SECTORS   ; Sector count (127 MiB)

; Partitions 2, 3, 4 (empty)
times 48 db 0

dw 0xAA55