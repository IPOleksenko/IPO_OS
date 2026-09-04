.global _start
.global exit
.extern main
.section .text
_start:
    mov %esp, initial_esp
    call main
    ret

exit:
    mov 4(%esp), %eax
    mov initial_esp, %esp
    ret

.section .data
initial_esp: .long 0


