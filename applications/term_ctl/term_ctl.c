/**
 * term_ctl - terminal control utility
 *
 * Usage:
 *   term_ctl <text...>        inject text into the terminal and execute it
 *   term_ctl -w <text...>     inject text into the prompt only (no execution)
 *
 * Without arguments: interactive demo mode.
 */

#include <stdio.h>
#include <string.h>
#include <syscall.h>

static void join_args(int argc, char **argv, int start, char *out, int max) {
    int pos = 0;
    for (int i = start; i < argc; i++) {
        int len = (int)strlen(argv[i]);
        if (pos + len >= max - 1) len = max - 1 - pos;
        for (int j = 0; j < len; j++) out[pos++] = argv[i][j];
        if (i + 1 < argc && pos < max - 2) {
            out[pos++] = ' ';
        }
    }
    out[pos] = '\0';
}

int main(int argc, char **argv) {
    if (argc >= 3 && argv[1][0] == '-' && argv[1][1] == 'w' && argv[1][2] == '\0') {
        /* term_ctl -w <text...> -- write to prompt only, no execution */
        char buf[512];
        join_args(argc, argv, 2, buf, sizeof(buf));
        printf("Writing to prompt: %s\n", buf);
        ipo_terminal_input(buf, 0);
        return 0;
    }

    if (argc >= 2) {
        /* term_ctl <text...> -- inject and auto-execute */
        char buf[512];
        join_args(argc, argv, 1, buf, sizeof(buf));
        printf("Executing: %s\n", buf);
        ipo_terminal_input(buf, 1);
        return 0;
    }

    /* Interactive demo */
    printf("=== term_ctl demo ===\n");
    printf("1. Execute 'hello'\n");
    printf("2. Execute 'ps'\n");
    printf("3. Write custom text to prompt (no execute)\n");
    printf("4. Execute custom command\n");
    printf("Choice [1/2/3/4]: ");

    char choice[4];
    scanf("%[^\n]", choice);

    if (choice[0] == '1') {
        printf("Executing 'hello'...\n");
        ipo_terminal_input("hello", 1);
    } else if (choice[0] == '2') {
        printf("Executing 'ps'...\n");
        ipo_terminal_input("ps", 1);
    } else if (choice[0] == '3') {
        printf("Enter text: ");
        char text[256];
        scanf("%[^\n]", text);
        printf("Writing to prompt: %s\n", text);
        ipo_terminal_input(text, 0);
    } else if (choice[0] == '4') {
        printf("Enter command: ");
        char text[256];
        scanf("%[^\n]", text);
        printf("Executing: %s\n", text);
        ipo_terminal_input(text, 1);
    } else {
        printf("Unknown choice.\n");
    }

    return 0;
}
