.code32
.section .init
.global _init
_init:
    pushl %ebp
    movl %esp, %ebp
    leave
    ret

.section .fini
.global _fini
_fini:
    pushl %ebp
    movl %esp, %ebp
    leave
    ret

