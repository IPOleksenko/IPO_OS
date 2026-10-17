#ifndef _SYS_UCONTEXT_H
#define _SYS_UCONTEXT_H

#include <signal.h>
#include <stdint.h>
#include <stddef.h>

#define REG_GS   0
#define REG_FS   1
#define REG_ES   2
#define REG_DS   3
#define REG_EDI  4
#define REG_ESI  5
#define REG_EBP  6
#define REG_ESP  7
#define REG_EBX  8
#define REG_EDX  9
#define REG_ECX  10
#define REG_EAX  11
#define REG_TRAPNO 12
#define REG_ERR  13
#define REG_EIP  14
#define REG_CS   15
#define REG_EFL  16
#define REG_UESP 17
#define REG_SS   18

typedef int greg_t;
typedef greg_t gregset_t[19];

typedef struct {
    gregset_t gregs;
} mcontext_t;

typedef struct ucontext {
    unsigned long     uc_flags;
    struct ucontext  *uc_link;
    stack_t           uc_stack;
    mcontext_t        uc_mcontext;
    sigset_t          uc_sigmask;
} ucontext_t;

#endif

