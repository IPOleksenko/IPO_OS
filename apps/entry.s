.global _start
.extern main

.section .entry, "ax"
_start:
    jmp main
