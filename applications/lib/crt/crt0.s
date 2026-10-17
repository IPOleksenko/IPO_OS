.code32
.global _start
.global environ
.global __progname

.extern main
.extern __libc_start_main
.weak __bss_start
.weak __bss_end

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

    /* Align stack to 16 bytes for GCC/C ABI */
    andl $-16, %esp

    /* Push arguments for __libc_start_main(main, argc, argv, envp) */
    pushl %ecx  /* envp */
    pushl %edx  /* argv */
    pushl %eax  /* argc */
    pushl $main /* main function pointer */
    call __libc_start_main
    addl $16, %esp

    /* eax and edx contain the return value from main.
     * Clean up stack frame and return directly to process_trampoline!
     */
    movl %ebp, %esp
    popl %ebp
    ret

.section .data
.align 4
environ:
    .long 0
__progname:
    .long 0

