/**
 * exec_caller - launches other programs via IPO_SYSCALL_EXEC
 *
 * Usage:
 *   exec_caller <program> [args...]   run <program> with optional args
 *
 * Without arguments: interactive demo mode.
 */

#include <stdio.h>
#include <string.h>
#include <syscall.h>

int main(int argc, char **argv) {
    if (argc >= 2) {
        /* Direct invocation: exec_caller <program> [args...] */
        const char *prog = argv[1];
        int sub_argc = argc - 1;
        char **sub_argv = argv + 1;

        printf("Launching '%s' (argc=%d) via ipo_exec...\n", prog, sub_argc);
        int pid = ipo_exec(prog, sub_argc, sub_argv);
        if (pid < 0) {
            printf("ipo_exec failed: error %d\n", pid);
            return 1;
        }
        printf("Process '%s' finished (pid=%d).\n", prog, pid);
        return 0;
    }

    /* Interactive demo */
    printf("=== exec_caller demo ===\n");
    printf("Demonstrates launching processes via IPO_SYSCALL_EXEC.\n\n");
    printf("1. Run 'hello'\n");
    printf("2. Run 'sound_test mario'\n");
    printf("3. Run custom program\n");
    printf("Choice [1/2/3]: ");

    char choice[4];
    scanf("%[^\n]", choice);

    if (choice[0] == '1') {
        char *args[] = { "hello", NULL };
        printf("Running 'hello'...\n");
        int pid = ipo_exec("hello", 1, args);
        printf("Finished (pid=%d)\n", pid);

    } else if (choice[0] == '2') {
        char *args[] = { "sound_test", "mario", NULL };
        printf("Running 'sound_test mario'...\n");
        int pid = ipo_exec("sound_test", 2, args);
        printf("Finished (pid=%d)\n", pid);

    } else if (choice[0] == '3') {
        printf("Program name: ");
        char prog[128];
        scanf("%[^\n]", prog);

        printf("Arguments (space-separated, or empty): ");
        char argline[256];
        scanf("%[^\n]", argline);

        /* Split argline into argv */
        static char *sub_argv[32];
        sub_argv[0] = prog;
        int sub_argc = 1;

        char *p = argline;
        while (*p != '\0' && sub_argc < 31) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0') break;
            sub_argv[sub_argc++] = p;
            while (*p != ' ' && *p != '\t' && *p != '\0') p++;
            if (*p != '\0') { *p = '\0'; p++; }
        }
        sub_argv[sub_argc] = NULL;

        printf("Running '%s' (%d arg(s))...\n", prog, sub_argc - 1);
        int pid = ipo_exec(prog, sub_argc, sub_argv);
        printf("Finished (pid=%d)\n", pid);
    } else {
        printf("Unknown choice.\n");
    }

    return 0;
}
