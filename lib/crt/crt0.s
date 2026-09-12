.code32
.global _start
.global environ
.global __progname

.extern main
.extern exit
.extern __libc_init_array
.extern __bss_start
.extern __bss_end

.section .entry, "ax"
_start:
    /* Reset FPU state */
    fninit

    /* Zero out .bss section */
    movl $__bss_start, %edi
    movl $__bss_end, %ecx
    subl %edi, %ecx
    jbe .bss_done
    xorl %eax, %eax
    cld
    movl %ecx, %edx
    shrl $2, %ecx
    rep stosl
    movl %edx, %ecx
    andl $3, %ecx
    rep stosb
.bss_done:

    /* Set up stack frame */
    pushl %ebp
    movl %esp, %ebp

    /* Retrieve arguments passed on stack:
     * 8(%ebp)  = argc
     * 12(%ebp) = argv
     * 16(%ebp) = envp
     */
    movl 8(%ebp), %eax   /* argc */
    movl 12(%ebp), %edx  /* argv */
    movl 16(%ebp), %ecx  /* envp */

    /* Save environ and __progname */
    movl %ecx, environ
    testl %edx, %edx
    jz .no_progname
    movl (%edx), %esi
    movl %esi, __progname
.no_progname:

    /* Align stack to 16 bytes for GCC ABI */
    andl $-16, %esp

    /* Run global constructors (.init / .init_array) */
    call __libc_init_array

    /* Reload arguments from stack frame (clobbered by __libc_init_array) */
    movl 16(%ebp), %ecx  /* envp */
    movl 12(%ebp), %edx  /* argv */
    movl 8(%ebp), %eax   /* argc */

    /* Push main arguments: envp, argv, argc (16-byte stack alignment) */
    subl $4, %esp
    pushl %ecx
    pushl %edx
    pushl %eax
    call main

    /* Call exit(eax) */
    pushl %eax
    call exit

    /* Infinite loop if exit somehow returns */
.hang:
    hlt
    jmp .hang

.section .data
.align 4
environ:
    .long 0
__progname:
    .long 0

