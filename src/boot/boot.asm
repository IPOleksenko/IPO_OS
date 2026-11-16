%include "config.inc"

BITS 16
ORG 0x7C00

start_entry:
    jmp short start
banner: db "IPO_OS by IPOleksenko", 13, 10, 0

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [BOOT_DRIVE], dl

    mov si, banner
    mov ah, 0x0E
.print_loop:
    lodsb
    test al, al
    jz .print_done
    int 0x10
    jmp .print_loop
.print_done:

    ; Probe 32-bit EFLAGS (NT bit 14) before executing 32-bit ops
    pushf
    pop ax
    mov cx, ax
    xor ah, 0x40
    push ax
    popf
    pushf
    pop ax
    xor ax, cx
    jz halt

    ; Require INT 13h LBA extensions
    mov ah, 0x41
    mov bx, 0x55AA
    int 0x13
    jc disk_error
    cmp bx, 0xAA55
    jne disk_error

    ; Enable Fast A20 (port 0x92)
    in al, 0x92
    or al, 00000010b
    out 0x92, al

    ; Enter Unreal Mode temporarily to load 4 GiB limits into DS/ES
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    inc ax
    mov cr0, eax
    mov bx, 0x10
    mov ds, bx
    mov es, bx
    dec ax
    mov cr0, eax
    xor bx, bx
    mov ds, bx
    mov es, bx
    sti

    ; Load kernel from LBA 1 in 64-sector chunks through 0x10000 bounce buffer -> 1 MiB
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

    pop edi
    mov esi, 0x00010000
    movzx ecx, word [dap_sectors]
    add [dap_lba], ecx
    shl ecx, 7
    cld
    a32 rep movsd

    pop cx
    sub cx, [dap_sectors]
    jmp .read_loop

.read_done:
    ; Build E820 map at 0x6000; store header (magic, count, ptr, low sum) at 0x8FE0..0x8FF0
    xor ebx, ebx
    mov di, 0x8FE0
    mov eax, 0x45383230             ; 'E820' magic
    stosd                           ; [0x8FE0]
    xor eax, eax
    stosd                           ; [0x8FE4] = 0 (count)
    mov ah, 0x60                    ; eax = 0x6000
    stosd                           ; [0x8FE8] = 0x6000 (ptr low)
    xor ax, ax
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

    inc dword [0x8FE4]

    cmp dword [es:di+16], 1
    jne .next_entry

    mov eax, [es:di+8]
    add dword [0x8FF0], eax

.next_entry:
    add di, 20
    test ebx, ebx
    jnz .detect_loop

.detect_done:
    test di, di
    jnz .detect_exit

    mov ax, 0xE801
    int 0x15
    jc .detect_exit
    test ax, ax
    jnz .have_e801
    mov ax, cx
    mov bx, dx
.have_e801:
    cwde
    shl eax, 10
    mov [0x8FF0], eax
    add [0x8FF2], bx

.detect_exit:
    mov ah, 0x01
    mov cx, 0x2000
    int 0x10

    cli
    lgdt [gdt_desc]
    mov eax, cr0
    inc ax
    mov cr0, eax
    jmp 0x08:pmode_entry

gdt_start:
gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start
    dw 0
    dq 0x00CF9A000000FFFF   ; 0x08: code
    dq 0x00CF92000000FFFF   ; 0x10: data
gdt_end:

[BITS 32]
pmode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
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

dap:
    db 16
    db 0
dap_sectors:
    dw 0
    dw 0x0000
dap_seg:
    dw 0x1000
dap_lba:
    dd 1
    dd 0

times 446-($-$$) db 0

; MBR partition table (Partition 1 at LBA 2048)
db 0x80
db 0x00, 0x20, 0x21
db 0x83
db 0xFE, 0xFF, 0xFF
dd MBR_PARTITION_START_LBA
dd MBR_PARTITION_SECTORS

times 48 db 0

dw 0xAA55
