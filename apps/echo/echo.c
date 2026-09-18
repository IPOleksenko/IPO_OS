/*
 * echo.c - Command-line echo application for IPO_OS
 *
 * Prints command-line arguments separated by space and a trailing newline.
 */

#include <stdio.h>

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            putchar(' ');
        }
        printf("%s", argv[i]);
    }
    putchar('\n');
    return 0;
}

