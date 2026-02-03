bits 32
section .text

global outb
global inb
global outw
global inw
global outl
global inl
global io_wait

; void outb(uint16_t port, uint8_t value)
outb:
    mov edx, [esp + 4]   ; port
    mov eax, [esp + 8]   ; value
    out dx, al
    ret

; uint8_t inb(uint16_t port)
inb:
    mov edx, [esp + 4]
    xor eax, eax
    in al, dx
    ret

; void outw(uint16_t port, uint16_t value)
outw:
    mov edx, [esp + 4]
    mov eax, [esp + 8]
    out dx, ax
    ret

; uint16_t inw(uint16_t port)
inw:
    mov edx, [esp + 4]
    xor eax, eax
    in ax, dx
    ret

; void outl(uint16_t port, uint32_t value)
outl:
    mov edx, [esp + 4]
    mov eax, [esp + 8]
    out dx, eax
    ret

; uint32_t inl(uint16_t port)
inl:
    mov edx, [esp + 4]
    in eax, dx
    ret

; void io_wait(void)
io_wait:
    mov al, 0
    out 0x80, al
    ret
