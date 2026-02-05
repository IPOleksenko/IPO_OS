/*
 * hello.c - Simple "Hello World" application for IPO_OS
 * 
 * This application demonstrates the IPOB executable format and
 * can be run from the terminal or autorun.
 */

#include <stdio.h>

/**
 * main - Application entry point
 * 
 * Called by the process manager after loading the IPOB executable.
 * Runs in kernel context with access to all kernel APIs.
 * 
 * @argc: Number of command-line arguments
 * @argv: Array of command-line argument strings
 */
int main(int argc, char **argv) {
    if (argc == 1) {
        printf("Hello, World!\n");
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        printf("Hello, ");
        printf(argv[i]);
        printf("!\n");
    }

    return 0;
}